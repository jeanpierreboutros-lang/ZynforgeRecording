#!/usr/bin/env python3
"""Prove an exported .RPP in real REAPER, and validate a .dawproject.

Takes the fixture session the "Session interchange" suite writes when
ZF_EXPORT_FIXTURE_DIR is set, then:

  1. renders a copy of the project in REAPER and compares the result with the
     engine's own mix, sample for sample;
  2. opens the untouched project and reads every track, item, source, marker
     and note back through REAPER's scripting API;
  3. with --schemas DIR (holding Project.xsd and MetaData.xsd from
     github.com/bitwig/dawproject), validates the .dawproject with xmllint.
     Without it the .dawproject is not checked, and the verdict says so.

Exit code 0 = everything verified, 1 = a check failed, 2 = what ran passed but
the .dawproject was not validated.

REAPER runs from a private resource folder, in the background, and quits itself.
The owner's settings are never read or written, and nothing here signals a
REAPER that does not carry this script's private folder on its command line.

  ZF_EXPORT_FIXTURE_DIR=/tmp/fx "...Tests" --test-filter="Session interchange"
  tools/verify_reaper_export.py /tmp/fx --work /tmp/fx-work --schemas /tmp/xsd
"""
import argparse
import array
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

REAPER = "/Applications/REAPER.app"
TOLERANCE = 0.003        # REAPER's equal-power shape 7 is sin(t*pi/2) to 0.003 of full level


def read_wav(path):
    data = open(path, "rb").read()
    if data[:4] not in (b"RIFF", b"RF64"):
        raise ValueError("not a WAV: " + path)
    pos, fmt, body = 12, None, None
    while pos + 8 <= len(data):
        chunk, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if chunk == b"fmt ":
            fmt = struct.unpack("<HHIIHH", data[pos + 8:pos + 24])
        elif chunk == b"data":
            body = data[pos + 8:pos + 8 + size]
            break
        pos += 8 + size + (size & 1)
    tag, channels, rate, _, _, bits = fmt
    if bits != 32 or tag not in (3, 0xFFFE):
        raise ValueError("expected 32-bit float: " + path)
    samples = array.array("f")
    samples.frombytes(body[:len(body) - len(body) % 4])
    return rate, [samples[c::channels] for c in range(channels)]


def lua_string(text):
    return json.dumps(text)


def run_reaper(res, args, limit=40):
    """One background instance on the private folder; True if it quit by itself."""
    ini = os.path.join(res, "reaper.ini")
    novst = os.path.join(res, "no-plugins")
    os.makedirs(novst, exist_ok=True)
    # REAPER can rewrite this on exit; every launch starts from the same file.
    # Without the device keys it stops at a "select an audio device" prompt, and
    # without the VST keys a first run scans every plug-in on the machine.
    with open(ini, "w") as f:
        f.write("[reaper]\nrenderclosewhendone=4\nvst_scan=2\nvstpath64=%s\nvstpath_root=%s\n"
                "autosaveint=0\ncoreaudiooutdevnew=zf-none\ncoreaudioindevnew=zf-none\n"
                "audiocloseinactive=1\n" % (novst, novst))

    def ours():
        found = subprocess.run(["pgrep", "-f", "MacOS/REAPER.*" + res], capture_output=True, text=True)
        return [int(p) for p in found.stdout.split()]

    subprocess.run(["open", "-g", "-n", "-a", REAPER, "--args", "-newinst", "-nosplash",
                    "-ignoreerrors", "-cfgfile", ini] + args, check=True)
    started = time.time()
    while time.time() - started < 3 and not ours():
        time.sleep(0.1)
    while time.time() - started < limit:
        if not ours():
            return True
        time.sleep(0.2)
    for pid in ours():
        try:
            os.kill(pid, 15)
        except ProcessLookupError:
            pass
    return False


def render_settings(path):
    return ('  RENDER_FILE "%s"\n  RENDER_PATTERN ""\n  RENDER_FMT 0 2 48000\n  RENDER_1X 0\n'
            "  RENDER_RANGE 1 0 0 0 1000\n  RENDER_RESAMPLE 3 0 1\n  RENDER_ADDTOPROJ 0\n"
            "  RENDER_STEMS 0\n  RENDER_DITHER 0\n  <RENDER_CFG\n    ZXZhdyAAAQ==\n  >\n" % path)


def render_and_compare(project, reference, res, work):
    text = open(project, encoding="utf-8").read()
    output = os.path.join(work, "reaper-render.wav")
    if os.path.exists(output):
        os.remove(output)
    head, rest = text.split("\n", 1)
    copy = os.path.join(os.path.dirname(project), "verify-render.RPP")
    with open(copy, "w", encoding="utf-8") as f:
        f.write(head + "\n" + render_settings(output) + rest)
    script = os.path.join(work, "render.lua")
    with open(script, "w", encoding="utf-8") as f:
        # -renderproject races REAPER's start-up prompt about the audio device
        # on anything but a tiny project; a script runs once start-up is over.
        # 42230 renders with the project's settings and closes the dialog.
        f.write("reaper.Main_openProject(%s)\nreaper.Main_OnCommand(42230, 0)\n"
                "reaper.Main_OnCommand(40004, 0)\n" % lua_string(copy))
    try:
        # The first launch on a fresh folder can spend its time populating it.
        for _ in range(3):
            if run_reaper(res, [script]) and os.path.exists(output):
                break
    finally:
        os.remove(copy)
    if not os.path.exists(output):
        print("FAIL: REAPER rendered nothing")
        return False

    rate, ours = read_wav(output)
    _, theirs = read_wav(reference)
    return compare_renders(rate, ours, theirs)


def compare_renders(rate, ours, theirs):
    n = min(len(ours[0]), len(theirs[0]))
    print("render: %d Hz, %d frames (engine mix %d frames)" % (rate, len(ours[0]), len(theirs[0])))
    worst, at = 0.0, 0
    per_second = {}
    for c in (0, 1):
        for i in range(n):
            error = abs(ours[c][i] - theirs[c][i])
            second = i // rate
            per_second[second] = max(per_second.get(second, 0.0), error)
            if error > worst:
                worst, at = error, i
    print("peak difference from the engine mix: %.6f at %.4f s" % (worst, at / rate))
    print("per second: " + " ".join("%d:%.5f" % (s, e) for s, e in sorted(per_second.items())))
    level = max(max(abs(x) for x in theirs[0]), max(abs(x) for x in theirs[1]))
    print("mix peak: %.4f, residual %.1f dB below it" % (level, 20.0 * math.log10(max(worst, 1e-12) / level)))
    # Only the shared length was compared above, so a short render needs its
    # own check. A tail past the engine mix is fine: REAPER may render one.
    long_enough = len(ours[0]) >= len(theirs[0])
    if not long_enough:
        print("render is %d frames short of the engine mix" % (len(theirs[0]) - len(ours[0])))
    ok = worst < TOLERANCE and long_enough
    print("PASS" if ok else "FAIL")
    return ok


READBACK = r'''
local out = io.open(%s, "w")
local function line(...)
  local t = {...}
  for i, v in ipairs(t) do t[i] = tostring(v) end
  out:write(table.concat(t, "\t") .. "\n")
end
reaper.Main_openProject(%s)
line("project", reaper.CountTracks(0), reaper.CountMediaItems(0),
     reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 0, false),
     reaper.GetSetProjectInfo(0, "PROJECT_SRATE_USE", 0, false),
     reaper.Master_GetTempo())
local master = reaper.GetMasterTrack(0)
line("master", reaper.GetMediaTrackInfo_Value(master, "D_VOL"), reaper.GetMediaTrackInfo_Value(master, "B_MUTE"))
line("notes", (reaper.GetSetProjectNotes(0, false, ""):gsub("[\r\n]+", " | ")))
for i = 0, reaper.CountTracks(0) - 1 do
  local tr = reaper.GetTrack(0, i)
  local _, name = reaper.GetTrackName(tr)
  local r, g, b = reaper.ColorFromNative(reaper.GetTrackColor(tr) & 0xffffff)
  line("track", name, reaper.GetMediaTrackInfo_Value(tr, "D_VOL"), reaper.GetMediaTrackInfo_Value(tr, "D_PAN"),
       reaper.GetMediaTrackInfo_Value(tr, "I_PANMODE"), reaper.GetMediaTrackInfo_Value(tr, "D_DUALPANL"),
       reaper.GetMediaTrackInfo_Value(tr, "D_DUALPANR"), reaper.GetMediaTrackInfo_Value(tr, "B_MUTE"),
       reaper.GetMediaTrackInfo_Value(tr, "I_NCHAN"), r, g, b, reaper.GetTrackColor(tr) ~= 0)
  for j = 0, reaper.CountTrackMediaItems(tr) - 1 do
    local it = reaper.GetTrackMediaItem(tr, j)
    local tk = reaper.GetActiveTake(it)
    local src = reaper.GetMediaItemTake_Source(tk)
    local len = reaper.GetMediaSourceLength(src)
    local _, take = reaper.GetSetMediaItemTakeInfo_String(tk, "P_NAME", "", false)
    line("item", take, reaper.GetMediaItemInfo_Value(it, "D_POSITION"), reaper.GetMediaItemInfo_Value(it, "D_LENGTH"),
         reaper.GetMediaItemTakeInfo_Value(tk, "D_STARTOFFS"), reaper.GetMediaItemInfo_Value(it, "D_VOL"),
         reaper.GetMediaItemInfo_Value(it, "D_FADEINLEN"), reaper.GetMediaItemInfo_Value(it, "D_FADEOUTLEN"),
         reaper.GetMediaItemInfo_Value(it, "C_FADEINSHAPE"), reaper.GetMediaItemInfo_Value(it, "B_MUTE"),
         reaper.GetMediaItemTakeInfo_Value(tk, "I_CHANMODE"), reaper.GetMediaSourceType(src, ""),
         reaper.GetMediaSourceNumChannels(src), reaper.GetMediaSourceSampleRate(src), len,
         reaper.GetMediaSourceFileName(src, ""))
  end
end
local _, markers = reaper.CountProjectMarkers(0)
for m = 0, markers - 1 do
  local _, _, pos, _, name = reaper.EnumProjectMarkers3(0, m)
  line("marker", pos, name)
end
out:close()
reaper.Main_SaveProjectEx(0, %s, 0)
reaper.Main_OnCommand(40004, 0)
'''


def read_back(project, res, work):
    report = os.path.join(work, "reaper-readback.txt")
    saved = os.path.join(work, "reaper-saved.rpp")
    for leftover in (report, saved):
        if os.path.exists(leftover):
            os.remove(leftover)
    script = os.path.join(work, "readback.lua")
    with open(script, "w", encoding="utf-8") as f:
        f.write(READBACK % (lua_string(report), lua_string(project), lua_string(saved)))
    for _ in range(3):
        if run_reaper(res, [script]) and os.path.exists(report):
            break
    if not os.path.exists(report):
        print("FAIL: REAPER did not run the read-back script")
        return False
    rows = [line.rstrip("\n").split("\t") for line in open(report, encoding="utf-8")]
    ok = True
    for row in rows:
        print("  " + " | ".join(row))
        if row[0] == "item":
            offline = float(row[14]) <= 0.0
            if offline or not os.path.exists(row[15]):
                print("  ^^ FAIL: source offline")
                ok = False
    # What REAPER itself writes back after loading: the settings that have no
    # scripting getter, and that a render alone would not show were kept.
    settings = [line.strip() for line in open(saved, encoding="utf-8")
                if line.split()[:1] in (["ITEMMIX"], ["TIMELOCKMODE"], ["TEMPOENVLOCKMODE"])]
    print("  kept by REAPER: " + ", ".join(settings))
    if sorted(settings) != ["ITEMMIX 1", "TEMPOENVLOCKMODE 0", "TIMELOCKMODE 0"]:
        print("  ^^ FAIL: settings not kept")
        ok = False
    notes = [r for r in rows if r[0] == "notes"]
    if not notes or len(notes[0]) < 2 or not notes[0][1].strip():
        print("  ^^ FAIL: project notes missing")
        ok = False
    tracks = sum(1 for r in rows if r[0] == "track")
    items = sum(1 for r in rows if r[0] == "item")
    markers = sum(1 for r in rows if r[0] == "marker")
    print("read back %d tracks, %d items, %d markers" % (tracks, items, markers))
    return ok and tracks > 0 and items > 0


def validate_dawproject(path, schemas):
    with tempfile.TemporaryDirectory() as folder, zipfile.ZipFile(path) as archive:
        print("zip entries:", archive.namelist())
        archive.extractall(folder)
        ok = True
        for document, schema in (("project.xml", "Project.xsd"), ("metadata.xml", "MetaData.xsd")):
            result = subprocess.run(["/usr/bin/xmllint", "--noout", "--schema",
                                     os.path.join(schemas, schema), os.path.join(folder, document)],
                                    capture_output=True, text=True)
            print(document, "validates" if result.returncode == 0 else "FAILS")
            if result.returncode != 0:
                print(result.stderr[:2000])
                ok = False
        return ok


def verdict(ok, dawproject_checked):
    if not ok:
        return "NOT VERIFIED", 1
    if not dawproject_checked:
        return "RPP VERIFIED; DAWPROJECT NOT VALIDATED (no --schemas)", 2
    return "ALL VERIFIED", 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("fixture", help="ZF_EXPORT_FIXTURE_DIR of the test run")
    parser.add_argument("--work", required=True, help="scratch folder; REAPER's private resources live here")
    parser.add_argument("--schemas", help="folder with Project.xsd and MetaData.xsd")
    options = parser.parse_args()

    res = os.path.join(os.path.abspath(options.work), "reaper-resources")
    os.makedirs(res, exist_ok=True)
    sessions = [d for d in os.listdir(options.fixture) if os.path.isdir(os.path.join(options.fixture, d))]
    exports = os.path.join(options.fixture, sessions[0], "Export Files")
    files = sorted(os.listdir(exports))
    project = os.path.join(exports, next(f for f in files if f.endswith(".RPP")))
    dawproject = os.path.join(exports, next(f for f in files if f.endswith(".dawproject")))

    ok = render_and_compare(project, os.path.join(options.fixture, "engine-mix.wav"), res, options.work)
    ok = read_back(project, res, options.work) and ok
    if options.schemas:
        ok = validate_dawproject(dawproject, options.schemas) and ok
    text, status = verdict(ok, bool(options.schemas))
    print(text)
    return status


if __name__ == "__main__":
    sys.exit(main())
