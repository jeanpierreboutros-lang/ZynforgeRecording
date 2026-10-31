#!/usr/bin/env python3
"""Drive notarize.sh with mocked Apple tools: nothing is signed or submitted."""
import os
from pathlib import Path
import plistlib
import shutil
import stat
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
SCRIPT = TOOLS / "notarize.sh"
IDENTITY = "Developer ID Application: Test Owner (ABCDE12345)"

CODESIGN = r'''#!/usr/bin/env python3
import os, pathlib, sys
state = pathlib.Path(os.environ["ZF_MOCK_STATE"])
args = sys.argv[1:]
with (state / "calls.log").open("a") as log:
    log.write("codesign " + " ".join(args) + "\n")
target = args[-1]
if os.environ.get("ZF_MOCK_FAIL_SIGN") and "--sign" in args and target.endswith(os.environ["ZF_MOCK_FAIL_SIGN"]):
    print("mock codesign failure", file=sys.stderr)
    sys.exit(1)
key = state / ("ent-" + target.replace("/", "_"))
if "--sign" in args and "--entitlements" in args:
    key.write_text(pathlib.Path(args[args.index("--entitlements") + 1]).read_text())
if args[:1] == ["-dv"]:
    print("CodeDirectory v=20500 size=1 flags=0x10000(runtime) hashes=1+1", file=sys.stderr)
    print("Authority=Developer ID Application: Test Owner (ABCDE12345)", file=sys.stderr)
    print("Timestamp=Oct 31, 2026", file=sys.stderr)
    print("TeamIdentifier=ABCDE12345", file=sys.stderr)
if "--entitlements" in args and "--sign" not in args:
    sys.stdout.write(key.read_text())
'''

XCRUN = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
state = pathlib.Path(os.environ["ZF_MOCK_STATE"])
args = sys.argv[1:]
with (state / "calls.log").open("a") as log:
    log.write("xcrun " + " ".join(args) + "\n")
if args[0] == "--find":
    print("/mock/" + args[1])
elif args[:2] == ["notarytool", "submit"]:
    status = os.environ.get("ZF_MOCK_NOTARY", "Accepted")
    print(json.dumps({"id": "mock-submission", "status": status, "message": "mock"}))
elif args[:2] == ["notarytool", "log"]:
    pathlib.Path(args[-1]).write_text('{"issues": ["mock issue"]}')
elif args[:2] == ["notarytool", "history"]:
    sys.exit(int(os.environ.get("ZF_MOCK_HISTORY_EXIT", "0")))
elif args[:2] == ["stapler", "staple"]:
    sys.exit(int(os.environ.get("ZF_MOCK_STAPLE_EXIT", "0")))
'''

SECURITY = r'''#!/usr/bin/env python3
import os
if os.environ.get("ZF_MOCK_NO_IDENTITY") != "1":
    print('  1) AAAA "Developer ID Application: Test Owner (ABCDE12345)"')
    print('  2) BBBB "Apple Development: Test Owner (ZZZZZ99999)"')
'''

SPCTL = r'''#!/usr/bin/env python3
import os, pathlib, sys
state = pathlib.Path(os.environ["ZF_MOCK_STATE"])
with (state / "calls.log").open("a") as log:
    log.write("spctl " + " ".join(sys.argv[1:]) + "\n")
if os.environ.get("ZF_MOCK_SPCTL") == "reject":
    print("rejected", file=sys.stderr)
    sys.exit(3)
print(sys.argv[-1] + ": accepted", file=sys.stderr)
print("source=" + os.environ.get("ZF_MOCK_SPCTL", "Notarized Developer ID"), file=sys.stderr)
'''

HDIUTIL = r'''#!/usr/bin/env python3
import os, pathlib, shutil, sys
state = pathlib.Path(os.environ["ZF_MOCK_STATE"])
args = sys.argv[1:]
with (state / "calls.log").open("a") as log:
    log.write("hdiutil " + " ".join(args) + "\n")
if args[0] == "create":
    pathlib.Path(args[-1]).write_bytes(b"mock dmg")
    shutil.copytree(args[args.index("-srcfolder") + 1], state / "dmg-root", symlinks=True)
elif args[0] == "attach":
    mount = pathlib.Path(args[args.index("-mountpoint") + 1])
    for item in (state / "dmg-root").iterdir():
        target = mount / item.name
        if item.is_symlink():
            continue
        (shutil.copytree if item.is_dir() else shutil.copy2)(item, target)
'''


class NotarizeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zf-notarize-test-")
        self.root = Path(self.temporary.name)
        self.state = self.root / "state"
        self.state.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for name, body in (("codesign", CODESIGN), ("xcrun", XCRUN), ("security", SECURITY),
                           ("spctl", SPCTL), ("hdiutil", HDIUTIL)):
            path = self.bin / name
            path.write_text(body)
            path.chmod(path.stat().st_mode | stat.S_IEXEC)
        self.app = self.build_app()
        self.output = self.root / "out"

    def tearDown(self):
        self.temporary.cleanup()

    def build_app(self):
        app = self.root / "Zynforge Recording.app"
        macos = app / "Contents" / "MacOS"
        macos.mkdir(parents=True)
        shutil.copy("/usr/bin/true", macos / "Zynforge Recording")
        shutil.copy("/usr/bin/true", macos / "ZynforgeCapture")
        frameworks = app / "Contents" / "Frameworks"
        frameworks.mkdir()
        shutil.copy("/usr/bin/true", frameworks / "libextra.dylib")
        with (app / "Contents" / "Info.plist").open("wb") as plist:
            plistlib.dump({
                "CFBundleExecutable": "Zynforge Recording",
                "CFBundleIdentifier": "com.zynforge.recording",
                "NSMicrophoneUsageDescription": "records audio",
            }, plist)
        return app

    def run_script(self, *arguments, full_arguments=True, **environment):
        command = ["bash", str(SCRIPT)]
        if full_arguments:
            command += ["--app", str(self.app), "--identity", IDENTITY, "--profile", "zf-notary",
                        "--output", str(self.output), "--label", "test"]
        command += list(arguments)
        env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}",
                   ZF_MOCK_STATE=str(self.state), **environment)
        for name in ("ZF_APP", "ZF_SIGN_IDENTITY", "ZF_NOTARY_PROFILE", "ZF_OUTPUT_DIR"):
            env.pop(name, None)
        return subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)

    def calls(self):
        log = self.state / "calls.log"
        return log.read_text().splitlines() if log.exists() else []

    def index_of(self, *fragments):
        for index, call in enumerate(self.calls()):
            if all(fragment in call for fragment in fragments):
                return index
        self.fail(f"no call containing {fragments}:\n" + "\n".join(self.calls()))

    def assert_no_call(self, *fragments):
        for call in self.calls():
            self.assertFalse(all(fragment in call for fragment in fragments), call)

    def test_full_run_signs_inside_out_then_staples_before_packaging(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        dylib = self.index_of("codesign", "--sign", "libextra.dylib")
        helper = self.index_of("codesign", "--sign", "MacOS/ZynforgeCapture")
        app = self.index_of("codesign", "--sign", "ZynforgeRecording.entitlements")
        self.assertLess(dylib, helper)
        self.assertLess(helper, app)
        verify = self.index_of("--verify", "--deep", "--strict")
        submit_app = self.index_of("notarytool submit", "app-for-notarization.zip")
        staple_app = self.index_of("stapler staple", ".app")
        assess_app = self.index_of("spctl", "--type execute")
        create = self.index_of("hdiutil create")
        sign_dmg = self.index_of("codesign", "--sign", ".dmg")
        submit_dmg = self.index_of("notarytool submit", ".dmg")
        staple_dmg = self.index_of("stapler staple", ".dmg")
        self.assertLess(app, verify)
        self.assertLess(verify, submit_app)
        self.assertLess(submit_app, staple_app)
        self.assertLess(staple_app, assess_app)
        self.assertLess(assess_app, create)
        self.assertLess(create, sign_dmg)
        self.assertLess(sign_dmg, submit_dmg)
        self.assertLess(submit_dmg, staple_dmg)
        dmg = self.output / "Zynforge-Recording-test-macOS-universal.dmg"
        self.assertTrue(dmg.exists())
        self.assertTrue(dmg.with_name(dmg.name + ".sha256").exists())
        self.assertTrue((self.state / "dmg-root" / "LICENSE").exists())
        self.assertTrue((self.state / "dmg-root" / "THIRD-PARTY-NOTICES.md").exists())
        self.assertTrue((self.state / "dmg-root" / "Applications").is_symlink())

    def test_the_build_tree_is_never_signed(self):
        self.assertEqual(self.run_script().returncode, 0)
        for call in self.calls():
            if call.startswith("codesign") and "--sign" in call:
                self.assertNotIn(str(self.app), call)

    def test_rejected_submission_fails_and_fetches_the_log_without_stapling(self):
        result = self.run_script(ZF_MOCK_NOTARY="Invalid")
        self.assertEqual(result.returncode, 1)
        self.assertIn("did not accept", result.stderr)
        self.assertIn("mock issue", result.stderr)
        self.index_of("notarytool log", "mock-submission")
        self.assert_no_call("stapler staple")
        self.assert_no_call("hdiutil create")

    def test_failed_helper_signature_stops_before_the_app(self):
        result = self.run_script(ZF_MOCK_FAIL_SIGN="ZynforgeCapture")
        self.assertEqual(result.returncode, 1)
        self.assertIn('FAILED during "Signing inside-out', result.stderr)
        self.assert_no_call("codesign", "--sign", "ZynforgeRecording.entitlements")
        self.assert_no_call("notarytool submit")

    def test_failed_staple_stops_before_packaging(self):
        result = self.run_script(ZF_MOCK_STAPLE_EXIT="1")
        self.assertEqual(result.returncode, 1)
        self.assert_no_call("hdiutil create")

    def test_gatekeeper_rejection_fails(self):
        result = self.run_script(ZF_MOCK_SPCTL="reject")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Gatekeeper rejected", result.stderr)
        self.assert_no_call("hdiutil create")

    def test_gatekeeper_acceptance_that_is_not_notarized_fails(self):
        result = self.run_script(ZF_MOCK_SPCTL="Developer ID")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not as notarized", result.stderr)

    def test_check_passes_without_signing_or_submitting(self):
        result = self.run_script("--check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_no_call("--sign")
        self.assert_no_call("notarytool submit")
        self.assert_no_call("hdiutil")
        self.assertFalse(self.output.exists())

    def test_check_reports_every_problem_at_once(self):
        (self.app / "Contents" / "MacOS" / "ZynforgeCapture").unlink()
        result = self.run_script("--check", ZF_MOCK_NO_IDENTITY="1", ZF_MOCK_HISTORY_EXIT="1")
        self.assertEqual(result.returncode, 1)
        self.assertIn("ZynforgeCapture is not embedded", result.stderr)
        self.assertIn("identity is not in the keychain", result.stderr)
        self.assertIn("notary profile is missing or rejected", result.stderr)
        self.assertIn("found 3 problem(s)", result.stderr)

    def test_missing_helper_stops_a_real_run_before_signing(self):
        (self.app / "Contents" / "MacOS" / "ZynforgeCapture").unlink()
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assert_no_call("--sign")

    def test_non_developer_id_identity_is_refused(self):
        result = self.run_script("--check", "--identity", "Apple Development: Test Owner (ZZZZZ99999)")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not a Developer ID Application", result.stderr)

    def test_ad_hoc_identity_cannot_be_notarized(self):
        result = self.run_script("--identity", "-")
        self.assertEqual(result.returncode, 1)
        self.assert_no_call("notarytool submit")

    def test_nested_bundle_is_refused_not_guessed(self):
        (self.app / "Contents" / "Frameworks" / "Extra.framework").mkdir()
        result = self.run_script("--check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("nested bundle", result.stderr)

    def test_an_existing_release_is_never_overwritten(self):
        self.output.mkdir()
        existing = self.output / "Zynforge-Recording-test-macOS-universal.dmg"
        existing.write_bytes(b"released")
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(existing.read_bytes(), b"released")

    def test_secret_passed_as_an_option_is_never_echoed(self):
        secret = "hunter2-app-specific"
        for form in (["--password", secret], [f"--password={secret}"]):
            result = self.run_script(*form, full_arguments=False)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn(secret, result.stdout + result.stderr)

    def test_xtrace_cannot_leak_the_run(self):
        command = ["bash", "-x", str(SCRIPT), "--check", "--app", str(self.app), "--identity", IDENTITY,
                   "--profile", "zf-sensitive-profile", "--output", str(self.output), "--label", "test"]
        env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}", ZF_MOCK_STATE=str(self.state))
        result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("zf-sensitive-profile", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
