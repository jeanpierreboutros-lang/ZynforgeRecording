#!/usr/bin/env python3
"""Headless checks of the verification scripts' own judgement.

verify_take.sh must say which session it is judging and must not guess one.
verify_reaper_export.py must fail a render that stops short of the engine mix.
soak_monitor.sh must not log a clean soak it did not observe.
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import unittest.mock
import wave

TOOLS = pathlib.Path(__file__).resolve().parent


def settings_xml(**values):
    rows = "".join('<VALUE name="%s" val="%s"/>' % item for item in values.items())
    return '<?xml version="1.0" encoding="UTF-8"?>\n<PROPERTIES>%s</PROPERTIES>\n' % rows


class VerifyTakeSessionChoice(unittest.TestCase):
    def run_with(self, settings):
        with tempfile.TemporaryDirectory(prefix="zf-verify-take-") as raw:
            root = pathlib.Path(raw)
            home = root / "home"
            app = home / "Library/Application Support/Zynforge Recording"
            app.mkdir(parents=True)
            stale = home / "Music/Zynforge Sessions/Newest but wrong"
            (stale / "Audio Files").mkdir(parents=True)
            real = root / "Custom Root/The Show"
            (real / "Audio Files").mkdir(parents=True)
            if settings is not None:
                text = settings(real) if callable(settings) else settings
                (app / "Zynforge Recording.settings").write_text(text)
            env = dict(os.environ, HOME=str(home))
            done = subprocess.run(["/bin/bash", str(TOOLS / "verify_take.sh")], env=env,
                                  capture_output=True, text=True, timeout=60)
            return done.returncode, done.stdout + done.stderr, real, stale

    def test_no_argument_verifies_the_recorded_capture_not_the_newest_folder(self):
        status, output, real, stale = self.run_with(
            lambda real: settings_xml(lastCaptureDir=str(real), activeSessionDir="/nowhere"))
        self.assertIn("Session: %s" % real, output)
        self.assertNotIn(str(stale), output)

    def test_no_argument_falls_back_to_the_active_session(self):
        _, output, real, _ = self.run_with(lambda real: settings_xml(activeSessionDir=str(real)))
        self.assertIn("Session: %s" % real, output)

    def test_no_argument_refuses_to_guess(self):
        for settings in (None, settings_xml(), "not xml"):
            status, output, _, stale = self.run_with(settings)
            self.assertEqual(status, 2, output)
            self.assertNotIn(str(stale), output)

    def test_a_recorded_capture_that_is_gone_is_not_replaced_by_a_guess(self):
        status, output, _, stale = self.run_with(settings_xml(lastCaptureDir="/no/such/session"))
        self.assertEqual(status, 2, output)
        self.assertNotIn(str(stale), output)


def write_wav(path, frames=4800):
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(48000)
        out.writeframes(b"\x10\x00" * frames)


@unittest.skipUnless(all(shutil.which(tool) for tool in ("ffprobe", "jq", "xxd", "shasum")),
                     "verify_take.sh needs ffprobe, jq, xxd and shasum")
class VerifyTakeFiles(unittest.TestCase):
    def run_on(self, build):
        with tempfile.TemporaryDirectory(prefix="zf-verify-take-") as raw:
            session = pathlib.Path(raw) / "Show"
            audio = session / "Audio Files"
            audio.mkdir(parents=True)
            take = audio / "Track_01.wav"
            write_wav(take)
            report = {"numTracks": 1, "missedSamples": 0, "sha256Pending": False, "sampleRate": 48000,
                      "tracks": [{"files": [take.name], "totalSamplesPrimary": 4800,
                                  "sha256": [hashlib.sha256(take.read_bytes()).hexdigest()]}]}
            (session / "session.report.json").write_text(json.dumps(report))
            build(audio)
            done = subprocess.run(["/bin/bash", str(TOOLS / "verify_take.sh"), str(session)],
                                  capture_output=True, text=True, timeout=60)
            return done.returncode, done.stdout + done.stderr

    def test_a_healthy_take_passes(self):
        status, output = self.run_on(lambda audio: None)
        self.assertEqual(status, 0, output)

    def test_a_consolidate_render_beside_the_take_is_not_a_take(self):
        status, output = self.run_on(lambda audio: write_wav(audio / "Track_01_consolidated_1.wav"))
        self.assertEqual(status, 0, output)
        self.assertNotIn("invalid take filename", output)
        self.assertNotIn("unlisted take file", output)
        self.assertIn("Consolidate render", output)

    def test_any_other_stray_name_still_fails(self):
        status, output = self.run_on(lambda audio: write_wav(audio / "Track_01_rescued.wav"))
        self.assertEqual(status, 1, output)
        self.assertIn("invalid take filename", output)

    def test_an_unlisted_take_still_fails(self):
        status, output = self.run_on(lambda audio: write_wav(audio / "Track_02.wav"))
        self.assertEqual(status, 1, output)
        self.assertIn("unlisted take file: Track_02.wav", output)

    def test_a_session_with_only_renders_has_no_take(self):
        def only_renders(audio):
            (audio / "Track_01.wav").rename(audio / "Track_01_consolidated_1.wav")
        status, output = self.run_on(only_renders)
        self.assertEqual(status, 1, output)
        self.assertIn("no Track_* take files found", output)


APP_RUNNING = subprocess.run(["pgrep", "-x", "Zynforge Recording"], capture_output=True).returncode == 0


@unittest.skipIf(APP_RUNNING, "the real app is running on this Mac")
class SoakMonitorObservation(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory(prefix="zf-soak-")
        self.addCleanup(folder.cleanup)
        self.root = pathlib.Path(folder.name)
        self.home = self.root / "home"
        self.crashes = self.home / "Library/Logs/DiagnosticReports"
        self.crashes.mkdir(parents=True)
        self.log = self.root / "soak.log"

    def run_monitor(self, *session, hours="0", during=None):
        env = dict(os.environ, HOME=str(self.home))
        args = ["/bin/bash", str(TOOLS / "soak_monitor.sh"), "1", hours, str(self.log), *session]
        monitor = subprocess.Popen(args, env=env)
        if during:
            during()
        monitor.wait(timeout=30)
        return self.log.read_text()

    # The monitor's sample row, which ends in the crash count.
    def wait_for_first_sample(self):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if self.log.exists() and any(line[:1].isdigit() for line in self.log.read_text().splitlines()):
                return
            time.sleep(0.01)
        self.fail("the monitor took no sample")

    def recorded_session(self, name="Custom Root/The Show"):
        session = self.root / name
        (session / "Audio Files").mkdir(parents=True)
        app = self.home / "Library/Application Support/Zynforge Recording"
        app.mkdir(parents=True)
        (app / "Zynforge Recording.settings").write_text(settings_xml(lastCaptureDir=str(session)))
        return session

    def test_a_dead_window_is_seen_with_the_daemon_still_running(self):
        bundle = self.root / "Zynforge Recording.app/Contents/MacOS"
        bundle.mkdir(parents=True)
        daemon = bundle / "ZynforgeCapture"
        shutil.copy("/bin/sleep", daemon)
        rolling = subprocess.Popen([str(daemon), "20"])
        self.addCleanup(rolling.wait)
        self.addCleanup(rolling.terminate)
        time.sleep(0.2)
        output = self.run_monitor()
        self.assertIn("APP NOT RUNNING", output)
        self.assertIn("capture daemon is still running", output)
        self.assertIn("app_seen=NO", output)

    def test_it_finds_the_take_where_the_app_recorded_it_in_any_container(self):
        session = self.recorded_session()
        (session / "Audio Files/Track_07.aif").write_bytes(b"FORM" + b"\0" * 4096)
        output = self.run_monitor()
        self.assertNotIn("NOTHING TO WATCH", output)
        self.assertRegex(output, r"\bAIFF\b")
        self.assertIn("take_file_samples=1", output)

    def test_renders_and_punch_sidecars_are_not_the_take(self):
        session = self.recorded_session()
        audio = session / "Audio Files"
        (audio / "Track_01.wav").write_bytes(b"RIFF" + b"\0" * 4096)
        os.utime(audio / "Track_01.wav", (1000, 1000))
        (audio / "Track_01_consolidated_1.wav").write_bytes(b"XXXX" + b"\0" * 4096)
        (audio / "Track_01.punchbase.wav").write_bytes(b"XXXX" + b"\0" * 4096)
        output = self.run_monitor()
        self.assertRegex(output, r"\bRIFF\b")

    def test_a_session_named_on_the_command_line_wins(self):
        session = self.root / "Elsewhere"
        (session / "Audio Files").mkdir(parents=True)
        (session / "Audio Files/Track_01.flac").write_bytes(b"fLaC" + b"\0" * 4096)
        output = self.run_monitor(str(session))
        self.assertRegex(output, r"\bFLAC\b")

    def test_a_soak_with_nothing_to_watch_says_so(self):
        output = self.run_monitor()
        self.assertIn("NOTHING TO WATCH", output)
        self.assertIn("take_file_samples=0", output)
        self.assertIn("NO TAKE FILE WAS SEEN GROWING", output)

    def test_a_growing_take_is_observed(self):
        session = self.recorded_session()
        take = session / "Audio Files/Track_02.wav"
        take.write_bytes(b"RIFF" + b"\0" * 4096)
        def grow():
            self.wait_for_first_sample()
            with take.open("ab") as out:
                out.write(b"\0" * 1048576)
        output = self.run_monitor(hours="0.0006", during=grow)
        self.assertIn("growing_samples=1", output)
        self.assertNotIn("NO TAKE FILE WAS SEEN GROWING", output)

    def test_only_this_apps_new_crash_reports_count(self):
        (self.crashes / "Zynforge Recording-2026-01-01-000000.ips").write_text("old")
        (self.crashes / "ZynforgeLive-2026-01-01-000000.ips").write_text("another tool")
        os.utime(self.crashes / "Zynforge Recording-2026-01-01-000000.ips", (1000, 1000))
        def crash():
            self.wait_for_first_sample()
            (self.crashes / "ZynforgeMeasure-2026-01-02-000000.ips").write_text("another tool")
            (self.crashes / "Zynforge Recording Tests-2026-01-02-000000.ips").write_text("the tests")
            (self.crashes / "ZynforgeCapture-2026-01-02-000000.ips").write_text("new")
        output = self.run_monitor(hours="0.0006", during=crash)
        self.assertIn("new_crashes=1", output)
        self.assertIn("NEW CRASH REPORT", output)


class ReaperRenderComparison(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("verify_reaper_export",
                                                      TOOLS / "verify_reaper_export.py")
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def compare(self, ours, theirs):
        return self.module.compare_renders(48000, [ours, ours], [theirs, theirs])

    def test_identical_renders_pass(self):
        self.assertTrue(self.compare([0.5] * 4800, [0.5] * 4800))

    def test_a_render_that_stops_short_fails(self):
        self.assertFalse(self.compare([0.5] * 4000, [0.5] * 4800))

    def test_a_render_with_a_tail_passes(self):
        self.assertTrue(self.compare([0.5] * 4800 + [0.0] * 480, [0.5] * 4800))

    def test_a_different_render_fails(self):
        self.assertFalse(self.compare([0.5] * 4800, [0.25] * 4800))

    def run_main(self, *extra, checks=True):
        with tempfile.TemporaryDirectory(prefix="zf-verify-reaper-") as raw:
            root = pathlib.Path(raw)
            exports = root / "Show/Export Files"
            exports.mkdir(parents=True)
            (exports / "Show.RPP").write_text("x")
            (exports / "Show.dawproject").write_text("x")
            printed = io.StringIO()
            with unittest.mock.patch.object(self.module, "render_and_compare", return_value=checks), \
                 unittest.mock.patch.object(self.module, "read_back", return_value=checks), \
                 unittest.mock.patch.object(self.module, "validate_dawproject", return_value=True), \
                 unittest.mock.patch.object(sys, "argv", ["verify", str(root), "--work", str(root / "work"), *extra]), \
                 contextlib.redirect_stdout(printed):
                return self.module.main(), printed.getvalue()

    def test_all_verified_means_the_dawproject_was_validated(self):
        status, output = self.run_main("--schemas", "/somewhere")
        self.assertEqual(status, 0)
        self.assertIn("ALL VERIFIED", output)

    def test_a_skipped_dawproject_is_never_all_verified(self):
        status, output = self.run_main()
        self.assertEqual(status, 2)
        self.assertNotIn("ALL VERIFIED", output)
        self.assertIn("DAWPROJECT NOT VALIDATED", output)

    def test_a_failed_check_is_a_failure_either_way(self):
        status, output = self.run_main(checks=False)
        self.assertEqual(status, 1)
        self.assertIn("NOT VERIFIED", output)


if __name__ == "__main__":
    unittest.main()
