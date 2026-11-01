#!/usr/bin/env python3
"""Headless checks of the two verification scripts' own judgement.

verify_take.sh must say which session it is judging and must not guess one.
verify_reaper_export.py must fail a render that stops short of the engine mix.
"""
import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

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


if __name__ == "__main__":
    unittest.main()
