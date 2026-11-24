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

    def test_the_fallback_says_it_is_the_open_session_not_the_recorded_one(self):
        _, output, _, _ = self.run_with(lambda real: settings_xml(activeSessionDir=str(real)))
        self.assertNotIn("the one the app last recorded", output)
        self.assertIn("last had open", output)

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
    def run_on(self, build, edit_report=None, cwd=None, take_name="Track_01.wav"):
        with tempfile.TemporaryDirectory(prefix="zf-verify-take-") as raw:
            session = pathlib.Path(raw) / "Show"
            audio = session / "Audio Files"
            audio.mkdir(parents=True)
            take = audio / take_name
            write_wav(take)
            report = {"numTracks": 1, "missedSamples": 0, "sha256Pending": False, "sampleRate": 48000,
                      "tracks": [{"files": [take.name], "totalSamplesPrimary": 4800,
                                  "sha256": [hashlib.sha256(take.read_bytes()).hexdigest()]}]}
            if edit_report:
                edit_report(report)
            (session / "session.report.json").write_text(json.dumps(report))
            build(audio)
            done = subprocess.run(["/bin/bash", str(TOOLS / "verify_take.sh"), str(session)],
                                  capture_output=True, text=True, timeout=60, cwd=cwd)
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

    def test_a_report_still_pending_says_how_to_finish_it(self):
        status, output = self.run_on(lambda audio: None, lambda report: report.update(sha256Pending=True))
        self.assertEqual(status, 1, output)
        self.assertNotIn("re-run after it finishes", output)
        self.assertIn("open it in the app", output)

    def test_a_copy_that_could_not_be_hashed_is_a_note_for_a_whole_primary(self):
        def copy_failed(report):
            report["sha256Failed"] = True
            report["tracks"][0]["backupFiles"] = ["Track_01.wav"]
            report["tracks"][0]["backupSha256"] = [""]
        status, output = self.run_on(lambda audio: None, copy_failed)
        self.assertEqual(status, 0, output)
        self.assertIn("backup or mirror", output)

    def test_a_copy_flagged_on_its_own_is_a_note(self):
        status, output = self.run_on(lambda audio: None, lambda report: report.update(copySha256Failed=True))
        self.assertEqual(status, 0, output)
        self.assertIn("backup or mirror", output)

    def test_a_primary_that_could_not_be_hashed_still_fails(self):
        def primary_failed(report):
            report["sha256Failed"] = True
            report["tracks"][0]["sha256"] = [""]
        status, output = self.run_on(lambda audio: None, primary_failed)
        self.assertEqual(status, 1, output)
        self.assertIn("report hashing failed", output)

    def test_python_in_the_folder_it_is_run_from_never_runs(self):
        # Run from a folder holding a json.py, the report check imported that
        # file: the current folder came first on Python's import path.
        with tempfile.TemporaryDirectory(prefix="zf-verify-cwd-") as raw:
            planted = pathlib.Path(raw)
            marker = planted / "ran"
            (planted / "json.py").write_text(
                "import pathlib\npathlib.Path(%r).write_text('ran')\n" % str(marker))
            status, output = self.run_on(lambda audio: None, cwd=str(planted))
            self.assertFalse(marker.exists(), "verify_take.sh ran Python from the current folder")
            self.assertEqual(status, 0, output)

    def test_a_session_with_only_renders_has_no_take(self):
        def only_renders(audio):
            (audio / "Track_01.wav").rename(audio / "Track_01_consolidated_1.wav")
        status, output = self.run_on(only_renders)
        self.assertEqual(status, 1, output)
        self.assertIn("no take files (Track_NN or NN Name) found", output)

    # Takes named for their strips (Source/Audio/TakeNames.h, 2026-11-16).
    def test_a_named_take_passes(self):
        status, output = self.run_on(lambda audio: None, take_name="01 KiCK.wav")
        self.assertEqual(status, 0, output)

    def test_a_named_take_with_a_render_passes(self):
        status, output = self.run_on(lambda audio: write_wav(audio / "01 KiCK_consolidated_1.wav"),
                                     take_name="01 KiCK.wav")
        self.assertEqual(status, 0, output)
        self.assertIn("Consolidate render", output)

    def test_an_unlisted_named_take_fails(self):
        status, output = self.run_on(lambda audio: write_wav(audio / "02 SNARE.wav"),
                                     take_name="01 KiCK.wav")
        self.assertEqual(status, 1, output)
        self.assertIn("unlisted take file: 02 SNARE.wav", output)

    def test_an_orphan_named_part_fails(self):
        status, output = self.run_on(lambda audio: write_wav(audio / "02 SNARE_part02.wav"),
                                     take_name="01 KiCK.wav")
        self.assertEqual(status, 1, output)
        self.assertIn("orphan continuation part: 02 SNARE_part02.wav (missing 02 SNARE.wav)", output)

    def test_a_named_part_after_a_gap_fails(self):
        status, output = self.run_on(lambda audio: write_wav(audio / "01 KiCK_part03.wav"),
                                     take_name="01 KiCK.wav")
        self.assertEqual(status, 1, output)
        self.assertIn("01 KiCK.wav: missing part numbers [2]", output)


# A stand-in for /usr/bin/log: it prints the lines it was given, whatever it is
# asked, the way `log show` can print lines from outside the range asked for.
FAKE_LOG_TOOL = r"""#!/bin/bash
printf '%s\n' "$@" >> "$FAKE_LOG_ARGS"
if [[ -n "${FAKE_LOG_FAIL:-}" ]]; then echo "log: Could not open the log store" >&2; exit 64; fi
cat "$FAKE_LOG_LINES"
"""

# The take the log tests judge: it stopped at TAKE_STOP (with a fraction of a
# second) after TAKE_SECONDS in all, the last pass PASS_SECONDS long.
TAKE_STOP = "2020-03-01T20:30:00.900+00:00"
TAKE_SECONDS = 600.0
PASS_SECONDS = 60.0
OVERLOAD = "HALC_ProxyIOContext::IOWorkLoop: skipping cycle due to overload"


def log_line(offset, process, message="IssueReporting.cpp:492   RTAID [ use_case=Generic ]"):
    """A log line `offset` seconds after the take's stop (negative: before it)."""
    import datetime
    when = datetime.datetime.fromisoformat(TAKE_STOP) + datetime.timedelta(seconds=offset)
    path = ("/usr/sbin/coreaudiod" if process == "coreaudiod"
            else "/Applications/Zynforge Recording.app/Contents/MacOS/" + process)
    return json.dumps({"timestamp": when.strftime("%Y-%m-%d %H:%M:%S.%f+0000"),
                       "processImagePath": path, "eventMessage": message})


def heartbeat(start=-TAKE_SECONDS - 60, end=60):
    """coreaudiod's steady lines, every 10 s, around the whole take."""
    return [log_line(offset, "coreaudiod") for offset in range(int(start), int(end), 10)]


@unittest.skipUnless(all(shutil.which(tool) for tool in ("ffprobe", "jq", "xxd", "shasum")),
                     "verify_take.sh needs ffprobe, jq, xxd and shasum")
class VerifyTakeSystemLog(unittest.TestCase):
    def run_with_log(self, lines, report_fields=None, fail=False, skip=False):
        with tempfile.TemporaryDirectory(prefix="zf-verify-log-") as raw:
            root = pathlib.Path(raw)
            session = root / "Show"
            audio = session / "Audio Files"
            audio.mkdir(parents=True)
            take = audio / "01 KiCK.wav"
            write_wav(take)
            report = {"numTracks": 1, "missedSamples": 0, "sha256Pending": False, "sampleRate": 48000,
                      "stoppedAt": TAKE_STOP, "totalSeconds": TAKE_SECONDS, "passSeconds": PASS_SECONDS,
                      "preRollSeconds": 0,
                      "tracks": [{"files": [take.name], "totalSamplesPrimary": 4800,
                                  "sha256": [hashlib.sha256(take.read_bytes()).hexdigest()]}]}
            report.update(report_fields or {})
            (session / "session.report.json").write_text(json.dumps(report))
            tool = root / "log"
            tool.write_text(FAKE_LOG_TOOL)
            tool.chmod(0o755)
            (root / "lines").write_text("\n".join(lines) + "\n")
            env = dict(os.environ, ZF_VERIFY_LOG_TOOL=str(tool), FAKE_LOG_LINES=str(root / "lines"),
                       FAKE_LOG_ARGS=str(root / "args"))
            env.pop("ZF_VERIFY_SKIP_SYSLOG", None)
            if fail:
                env["FAKE_LOG_FAIL"] = "1"
            if skip:
                env["ZF_VERIFY_SKIP_SYSLOG"] = "1"
            done = subprocess.run(["/bin/bash", str(TOOLS / "verify_take.sh"), str(session)], env=env,
                                  capture_output=True, text=True, timeout=120)
            args = (root / "args").read_text() if (root / "args").exists() else ""
            return done.returncode, done.stdout + done.stderr, args

    def assertUnchecked(self, status, output):
        self.assertEqual(status, 0, output)
        self.assertIn("could not check this Mac's log for the take", output)
        self.assertNotIn("no CoreAudio overload", output)

    # SES-01: a log that could not be read is no evidence of a clean take.
    def test_a_log_that_cannot_be_read_is_a_note_not_a_pass(self):
        status, output, _ = self.run_with_log(heartbeat(), fail=True)
        self.assertUnchecked(status, output)

    def test_a_log_with_nothing_from_the_take_is_a_note_not_a_pass(self):
        # Retention ran out, or the take was made on another Mac: what the log
        # tool prints is all from another day.
        status, output, _ = self.run_with_log(heartbeat(start=86400, end=86400 + 900))
        self.assertUnchecked(status, output)

    def test_a_log_that_starts_after_the_take_began_is_a_note(self):
        status, output, _ = self.run_with_log(heartbeat(start=-120, end=60))
        self.assertUnchecked(status, output)

    def test_a_clean_log_for_the_whole_take_passes(self):
        status, output, _ = self.run_with_log(heartbeat())
        self.assertEqual(status, 0, output)
        self.assertIn("no CoreAudio overload", output)
        self.assertNotIn("could not check", output)

    def test_an_overload_in_the_take_fails(self):
        status, output, _ = self.run_with_log(heartbeat() + [log_line(-30, "Zynforge Recording", OVERLOAD)])
        self.assertEqual(status, 1, output)
        self.assertIn("1 CoreAudio overload", output)

    def test_an_overload_in_an_earlier_pass_fails(self):
        # The report's totals add up every pass; the last pass is 60 s long.
        status, output, _ = self.run_with_log(heartbeat() + [log_line(-400, "Zynforge Recording", OVERLOAD)])
        self.assertEqual(status, 1, output)
        self.assertIn("1 CoreAudio overload", output)

    def test_the_last_fraction_of_a_second_is_checked(self):
        # The take stopped at 20:30:00.9: an overload at .6 was in it, one at
        # 20:30:01.4 was not. The range asked for runs to 20:30:01 or later.
        status, output, args = self.run_with_log(heartbeat() + [log_line(-0.3, "Zynforge Recording", OVERLOAD),
                                                               log_line(0.5, "Zynforge Recording", OVERLOAD)])
        self.assertEqual(status, 1, output)
        self.assertIn("1 CoreAudio overload", output)
        import datetime
        ends = [line for previous, line in zip(args.splitlines(), args.splitlines()[1:]) if previous == "--end"]
        self.assertTrue(ends, args)
        # log show reads a time without a zone as this Mac's.
        latest = max(datetime.datetime.strptime(end, "%Y-%m-%d %H:%M:%S").astimezone() for end in ends)
        self.assertGreaterEqual(latest, datetime.datetime.fromisoformat(TAKE_STOP), args)

    # SES-02: only the process that captured the take lost input.
    def test_overloads_before_the_take_started_do_not_count(self):
        status, output, _ = self.run_with_log(heartbeat() + [log_line(-TAKE_SECONDS - 3, "Zynforge Recording",
                                                                      OVERLOAD)])
        self.assertEqual(status, 0, output)
        self.assertIn("no CoreAudio overload", output)

    def test_a_take_in_the_capture_helper_counts_only_the_helper(self):
        helper = {"captureProcess": "ZynforgeCapture"}
        window = heartbeat() + [log_line(-30, "Zynforge Recording", OVERLOAD)]
        status, output, _ = self.run_with_log(window, helper)
        self.assertEqual(status, 0, output)
        self.assertIn("no CoreAudio overload", output)
        status, output, _ = self.run_with_log(window + [log_line(-20, "ZynforgeCapture", OVERLOAD)], helper)
        self.assertEqual(status, 1, output)
        self.assertIn("1 CoreAudio overload", output)

    def test_helper_overloads_with_no_capture_process_in_the_report_are_a_note(self):
        status, output, _ = self.run_with_log(heartbeat() + [log_line(-30, "ZynforgeCapture", OVERLOAD)])
        self.assertEqual(status, 0, output)
        self.assertIn("ZynforgeCapture logged 1", output)

    def test_skipping_the_log_says_so(self):
        status, output, _ = self.run_with_log(heartbeat(), skip=True)
        self.assertUnchecked(status, output)


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


# Which file the monitor watches does not depend on the app, so these run with
# it open too (the monitor only reads: pgrep and ps).
class SoakMonitorTakeNames(unittest.TestCase):
    def watch(self, *names, newest=None):
        with tempfile.TemporaryDirectory(prefix="zf-soak-names-") as raw:
            root = pathlib.Path(raw)
            home = root / "home"
            (home / "Library/Logs/DiagnosticReports").mkdir(parents=True)
            audio = root / "The Show/Audio Files"
            audio.mkdir(parents=True)
            for age, name in enumerate(reversed(names)):
                (audio / name).write_bytes(b"RIFF" + b"\0" * 4096)
                os.utime(audio / name, (2000 - age, 2000 - age))
            if newest:
                (audio / newest).write_bytes(b"XXXX" + b"\0" * 4096)
                os.utime(audio / newest, (5000, 5000))
            log = root / "soak.log"
            subprocess.run(["/bin/bash", str(TOOLS / "soak_monitor.sh"), "1", "0", str(log), str(root / "The Show")],
                           env=dict(os.environ, HOME=str(home)), timeout=30)
            return log.read_text()

    # SES-03: takes have been named for their strips since 2026-11-16.
    def test_a_named_take_is_watched(self):
        output = self.watch("01 KiCK.wav")
        self.assertNotIn("NOTHING TO WATCH", output)
        self.assertRegex(output, r"\bRIFF\b")
        self.assertIn("take_file_samples=1", output)

    def test_named_parts_and_three_digit_strips_are_watched(self):
        for name in ("01 KiCK_part02.wav", "100 Lead Vox.flac", "07 OH L.aiff", "Track_03.aif"):
            output = self.watch(name)
            self.assertIn("take_file_samples=1", output, name)

    def test_a_named_render_or_punch_sidecar_is_not_the_take(self):
        for other in ("01 KiCK_consolidated_1.wav", "01 KiCK.punchbase.wav"):
            output = self.watch("01 KiCK.wav", newest=other)
            self.assertRegex(output, r"\bRIFF\b", other)


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
