#!/usr/bin/env python3
"""Run auto_stop.sh against disposable files and mocked clipboard/HTTP only."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class AutoStopTests(unittest.TestCase):
    def probe(self, mode):
        with tempfile.TemporaryDirectory(prefix="zf-autostop-test-") as temporary:
            root = Path(temporary)
            session = root / "session"
            audio = session / "Audio Files"
            audio.mkdir(parents=True)
            (audio / "Track_01.wav").write_bytes(b"fixture")
            script = Path(__file__).with_name("auto_stop.sh").read_text()
            # Never read, remove or create the real script's fixed /tmp files.
            for name in ("zynforge_autostop.log", "zynforge_autostop.stop", "zynforge_cmd_token"):
                script = script.replace("/tmp/" + name, str(root / name))
            copied = root / "auto_stop.sh"
            copied.write_text(script)
            executables = root / "bin"
            executables.mkdir()
            mocks = {
                "pbpaste": "print('http://localhost:9000/?t=' + 'a' * 64)\n",
                "sleep": "import os, time\nif os.environ['ZF_FIXTURE_MODE'] == 'timeout': time.sleep(0.2)\n",
                "curl": r'''
import json, os, pathlib, sys
root = pathlib.Path(os.environ["ZF_FIXTURE_ROOT"])
counter = root / "calls.json"
counts = json.loads(counter.read_text()) if counter.exists() else {"stop": 0, "state": 0}
mode = os.environ["ZF_FIXTURE_MODE"]
if "-X" in sys.argv:
    counts["stop"] += 1
    number = counts["stop"]
    if number == 1:
        result = {"ok": False, "error": "STOP armed; send STOP again within 2 seconds"}
        if mode == "abort":
            (root / "zynforge_autostop.stop").touch()
    elif mode == "failed":
        result = {"ok": False, "error": "Recording stopped, but finalization failed: metadata"}
    elif (mode == "late_success" and number < 7) or mode == "timeout":
        result = {"ok": False, "error": "recording stopped; finalization pending"}
    else:
        result = {"ok": True}
else:
    counts["state"] += 1
    result = {"recording": counts["stop"] < 2, "sessionPath": str(root / "session"),
              "sampleRate": 48000, "elapsedSamples": 48000000}
    if mode == "changed" and counts["stop"] > 0:
        result["sessionPath"] = str(root / "different-session")
        result["recording"] = True
counter.write_text(json.dumps(counts))
print(json.dumps(result))
''',
            }
            for name, code in mocks.items():
                command = executables / name
                command.write_text("#!" + sys.executable + "\n" + code)
                command.chmod(0o700)
            env = dict(os.environ, PATH=str(executables) + os.pathsep + os.environ.get("PATH", ""),
                       ZF_FIXTURE_ROOT=str(root), ZF_FIXTURE_MODE=mode,
                       ZYNFORGE_AUTOSTOP_MIN_SECONDS="0", ZYNFORGE_AUTOSTOP_SIZE_THRESHOLD="0",
                       ZYNFORGE_AUTOSTOP_HARD_CAP="1", ZYNFORGE_AUTOSTOP_POLL_SECONDS="0",
                       ZYNFORGE_AUTOSTOP_STOP_TIMEOUT="2" if mode == "timeout" else "30")
            completed = subprocess.run(["/bin/bash", str(copied), str(session)], env=env,
                                       capture_output=True, text=True, timeout=20,
                                       preexec_fn=lambda: os.umask(0o022))
            counts = json.loads((root / "calls.json").read_text())
            persisted_token = (root / "zynforge_cmd_token").exists()
            log = (root / "zynforge_autostop.log").read_text()
            return completed.returncode, counts, persisted_token, log

    def test_waits_for_successful_finalization_acknowledgement(self):
        status, counts, _, log = self.probe("late_success")
        self.assertEqual(status, 0, log)
        self.assertGreaterEqual(counts["stop"], 7, "exited on recording:false while finalization was pending")

    def test_finalization_failure_cannot_exit_successfully(self):
        status, _, _, log = self.probe("failed")
        self.assertNotEqual(status, 0, log)
        self.assertNotIn("confirmed stopped:", log)

    def test_token_is_not_persisted_to_a_shared_temporary_file(self):
        status, _, persisted, log = self.probe("success")
        self.assertEqual(status, 0, log)
        self.assertFalse(persisted, "unused bearer token remained in a predictable shared temporary file")

    def test_new_session_is_never_sent_a_retry_stop(self):
        status, counts, _, log = self.probe("changed")
        self.assertNotEqual(status, 0, log)
        self.assertEqual(counts["stop"], 1, "STOP retried after another session replaced the pinned take")

    def test_pending_finalization_has_a_bounded_timeout(self):
        status, counts, _, log = self.probe("timeout")
        self.assertNotEqual(status, 0, log)
        self.assertGreaterEqual(counts["stop"], 2, "fixture never reached finalization pending")
        self.assertIn("not acknowledged within", log)

    def test_abort_sentinel_prevents_further_stop_commands(self):
        status, counts, _, log = self.probe("abort")
        self.assertEqual(status, 0, log)
        self.assertEqual(counts["stop"], 1, "abort sentinel did not cancel STOP retries")
        self.assertNotIn("confirmed stopped:", log)


if __name__ == "__main__":
    unittest.main()
