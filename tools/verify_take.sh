#!/usr/bin/env bash
#
# verify_take.sh — turnkey post-take integrity check for a ZynForge session.
#
# Closes the manual half of the RF64 / throughput field soak (FIELD-TEST.md
# section "Throughput + RF64"). Point it at a session folder; it checks every
# recorded WAV, AIFF or FLAC the way you'd otherwise do by hand:
#
#   • opens each file and reads its duration with ffprobe
#   • validates each container header; WAVs over 4 GiB must be RF64 + ds64
#   • continuation/split parts have a playable Track_NN base
#   • matches session.report.json: track count, complete per-file sha256,
#     and reports `missedSamples`
#
# Usage:
#   tools/verify_take.sh [SESSION_DIR]
#
# With no argument it verifies the capture the app last recorded, then its
# active session, as stored in the app's settings. It never guesses from folder
# dates: an older session that was only opened looks newest, and a pass on it
# would say nothing about the show.
# Exit code 0 = all green, 1 = a problem was found, 2 = nothing could be checked.
#
# Requires: ffprobe, xxd, shasum, jq, python3.

set -uo pipefail

FOUR_GIB=$((4 * 1024 * 1024 * 1024))
fail=0
red()   { printf '\033[31m%s\033[0m\n' "$*"; }
grn()   { printf '\033[32m%s\033[0m\n' "$*"; }
ylw()   { printf '\033[33m%s\033[0m\n' "$*"; }
hdr()   { printf '\n\033[1m%s\033[0m\n' "$*"; }

for bin in ffprobe xxd shasum jq python3; do
    command -v "$bin" >/dev/null || { red "missing required tool: $bin"; exit 2; }
done

# ── Resolve the session directory ───────────────────────────────────────────
SESSION="${1:-}"
if [[ -z "$SESSION" ]]; then
    settings="$HOME/Library/Application Support/Zynforge Recording/Zynforge Recording.settings"
    # The first key that has a value decides: a recorded capture that has since
    # gone must stop here, not fall through to some other session.
    # Which key it was comes first, then a tab: the open session is no proof
    # of a recording.
    chosen=$(python3 -I - "$settings" 2>/dev/null <<'PY'
import sys
import xml.etree.ElementTree as ElementTree

values = {row.get("name"): row.get("val", "") for row in ElementTree.parse(sys.argv[1]).getroot().iter("VALUE")}
if values.get("lastCaptureDir"):
    print("capture\t" + values["lastCaptureDir"])
elif values.get("activeSessionDir"):
    print("open\t" + values["activeSessionDir"])
PY
)
    SESSION="${chosen#*$'\t'}"
    if [[ -z "$chosen" || -z "$SESSION" ]]; then
        red "no session given and the app's settings name no recorded session"
        red "pass the session folder: tools/verify_take.sh \"/path/to/session\""
        exit 2
    fi
    if [[ "${chosen%%$'\t'*}" == "capture" ]]; then
        ylw "No session given; verifying the one the app last recorded."
    else
        # No recorded capture is named after Remove Last Capture, or after a
        # RECORD the capture helper never answered.
        ylw "No session given, and the app's settings name no recorded capture; verifying the session it"
        ylw "last had open instead. That may not be the show: pass the take's folder to be sure."
    fi
fi
SESSION="${SESSION%/}"
[[ -d "$SESSION" ]] || { red "not a directory: $SESSION"; exit 2; }

AUDIO="$SESSION/Audio Files"
[[ -d "$AUDIO" ]] || AUDIO="$SESSION"   # legacy: takes in the session root
REPORT="$SESSION/session.report.json"

hdr "Session: $SESSION"

# ── Continuation parts need a playable base file ────────────────────────────
hdr "1. Continuation-part check"
parts=$(find "$AUDIO" -maxdepth 1 -type f \( -name 'Track_*_part*.wav' -o -name 'Track_*_part*.aif' -o -name 'Track_*_part*.aiff' -o -name 'Track_*_part*.flac' \) | sort)
if [[ -n "$parts" ]]; then
    while IFS= read -r part; do
        name=$(basename "$part")
        base="${name%%_part*}.${name##*.}"
        if [[ ! -f "$AUDIO/$base" ]]; then
            red "orphan continuation part: $name (missing $base)"
            fail=1
        fi
    done <<< "$parts"
    (( fail == 0 )) && grn "OK — every continuation part has a base file."
else
    grn "OK — no continuation parts."
fi

# ── Per-file: header + length ───────────────────────────────────────────────
hdr "2. Per-file header + length"
shopt -s nullglob
media=()
renders=0
# A Consolidate render sits beside the takes and is no part of one: the report
# never lists it. QC, Export and the handoff skip the same exact name.
for w in "$AUDIO"/Track_*.wav "$AUDIO"/Track_*.aif "$AUDIO"/Track_*.aiff "$AUDIO"/Track_*.flac; do
    if [[ "$(basename "$w")" =~ ^Track_[0-9]{1,3}_consolidated_[0-9]+\.(wav|aif|aiff|flac)$ ]]; then
        ((renders+=1))
    else
        media+=("$w")
    fi
done
(( renders == 0 )) || ylw "  $renders Consolidate render(s) skipped: they are not takes"
if (( ${#media[@]} == 0 )); then
    red "no Track_* take files found in $AUDIO"; exit 1
fi
for w in "${media[@]}"; do
    name=$(basename "$w")
    ext="${name##*.}"
    size=$(stat -f%z "$w")
    magic=$(xxd -l 4 -p "$w")                       # 52494646=RIFF  52463634=RF64
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$w" 2>/dev/null)
    human=$(python3 -I -c "print(f'{$size/1e9:.2f} GB')" 2>/dev/null)

    case "$magic" in
        52494646) tag="RIFF" ;;
        52463634) tag="RF64" ;;
        464f524d) tag="AIFF" ;;
        664c6143) tag="FLAC" ;;
        *) tag="UNKNOWN" ;;
    esac
    line="  $name  $human  hdr=$tag  dur=${dur:-?}s"

    if [[ -z "$dur" || "$dur" == "N/A" ]]; then
        red "$line  -> FAILS TO OPEN"; fail=1; continue
    fi
    headerValid=1
    case "$ext" in
        wav) [[ "$tag" == "RIFF" || "$tag" == "RF64" ]] || headerValid=0 ;;
        aif|aiff) [[ "$tag" == "AIFF" ]] || headerValid=0 ;;
        flac) [[ "$tag" == "FLAC" ]] || headerValid=0 ;;
    esac
    if (( headerValid == 0 )); then
        red "$line  -> container header does not match extension"; fail=1; continue
    fi
    if [[ "$ext" == "wav" ]] && (( size > FOUR_GIB )) && [[ "$tag" != "RF64" ]]; then
        red "$line  -> >4 GiB but NOT RF64 (header overflow risk)"; fail=1; continue
    fi
    if [[ "$tag" == "RF64" ]]; then
        # Confirm a ds64 chunk follows the RF64/size header (bytes 12..15).
        ds64=$(xxd -s 12 -l 4 -p "$w")               # 64733634 = 'ds64'
        if [[ "$ds64" == "64733634" ]]; then
            grn "$line  +ds64 OK"
        else
            red "$line  -> RF64 without a ds64 chunk"; fail=1
        fi
    else
        grn "$line"
    fi
done

# ── Cross-check against session.report.json ─────────────────────────────────
hdr "3. session.report.json cross-check"
if [[ ! -f "$REPORT" ]]; then
    red "no session.report.json — the report must be written on stop."; fail=1
else
    if ! jq -e 'type == "object" and (.tracks | type == "array")' "$REPORT" >/dev/null 2>&1; then
        red "session.report.json is invalid or has no track manifest"
        exit 1
    fi
    pending=$(jq -r '.sha256Pending // false' "$REPORT")
    ntracks=$(jq -r '.numTracks // 0' "$REPORT")
    manifestTracks=$(jq -r '.tracks | length' "$REPORT")
    missed=$(jq -r '.missedSamples // 0' "$REPORT")
    deviceLost=$(jq -r '.captureDeviceLost // false' "$REPORT")
    echo "  numTracks=$ntracks  missedSamples=$missed  sha256Pending=$pending"
    if [[ "$ntracks" != "$manifestTracks" ]]; then
        red "  track manifest has $manifestTracks entries, report says $ntracks"
        fail=1
    fi
    [[ "$missed" == "0" ]] && grn "  OK — missedSamples = 0" || { red "  missedSamples = $missed (DROPPED AUDIO)"; fail=1; }
    [[ "$deviceLost" == "false" ]] || { red "  capture audio device stopped during the take"; fail=1; }
    stallMs=$(jq -r '.deviceStallMs // 0' "$REPORT")
    [[ "$stallMs" == "0" ]] || { red "  no audio from the device for $stallMs ms during the take"; fail=1; }
    # Only the primary is verified here, and step 5 re-hashes every file of it.
    # A copy whose drive was gone or hung when its hashes were made is that
    # copy's problem, not this take's: a note, unless a primary hash is missing.
    primaryUnhashed=$(jq -r '[.tracks[] | select(((.files // []) | length) != ((.sha256 // []) | length)
                              or ([(.sha256 // [])[] | select(type != "string" or (test("^[0-9a-fA-F]{64}$") | not))]
                                  | length > 0))] | length' "$REPORT")
    if [[ "$(jq -r '.sha256Failed // false' "$REPORT")" == "true" ]]; then
        if [[ "$pending" != "true" && "$primaryUnhashed" == "0" ]]; then
            ylw "  hashing failed on a backup or mirror; every primary file has its hash (checked in step 5)"
        else
            red "  report hashing failed"
            fail=1
        fi
    fi
    # Copy hashes are kept apart from the primary's. A copy file that would
    # not read back is a copy to review on its own drive; one whose drive was
    # gone is owed. Neither says anything about the take checked here.
    if [[ "$(jq -r '.copySha256Failed // false' "$REPORT")" == "true" ]]; then
        ylw "  a backup or mirror file could not be read back to hash: check that copy on its own drive"
    fi
    if [[ "$(jq -r '.copySha256Missing // false' "$REPORT")" == "true" ]]; then
        ylw "  backup or mirror hashes are owed: that drive was not there. Reopen the session with it connected."
    fi
    for field in primaryFailed backupFailed mirrorFailed punchSpliceFailed; do
        if [[ "$(jq -r --arg field "$field" '.[$field] // false' "$REPORT")" == "true" ]]; then
            red "  $field is true — capture needs review"
            fail=1
        fi
    done
    if [[ "$(jq -r '.mirrorsSkipped // 0' "$REPORT")" != "0" ]]; then
        red "  configured mirrors were skipped"
        fail=1
    fi
    if jq -e '[.tracks[].mirrors[]? | select(.failed == true)] | length > 0' "$REPORT" >/dev/null; then
        red "  a mirror writer failed"
        fail=1
    fi
    # Notes, not failures: neither says anything is wrong with the files.
    alarms=$(jq -r '.channelAlarms // [] | length' "$REPORT")
    (( alarms == 0 )) || ylw "  $alarms dead-channel alarm(s) during the take: see channelAlarms in the report"
    stampFailures=$(jq -r '.timeReferenceStampFailures // [] | length' "$REPORT")
    (( stampFailures == 0 )) || ylw "  $stampFailures file(s) kept their estimated BWF time: see timeReferenceStampFailures"

    # A hash proves only that a file still matches this report. Check the
    # report's frame counts and the complete part sequence as well: otherwise
    # a short but correctly hashed file, or a missing middle part, looks green.
    hdr "4. frame counts + continuation sequence"
    if ! python3 -I - "$REPORT" "$AUDIO" <<'PY'
import json, pathlib, re, subprocess, sys
from fractions import Fraction

report = json.loads(pathlib.Path(sys.argv[1]).read_text())
audio = pathlib.Path(sys.argv[2])
pattern = re.compile(r"^Track_(\d{1,3})(?:_part(\d+))?\.(wav|aif|aiff|flac)$")
render = re.compile(r"^Track_\d{1,3}_consolidated_\d+\.(wav|aif|aiff|flac)$")
groups = {}
problems = []
for path in audio.iterdir():
    if not path.is_file() or not path.name.startswith("Track_"):
        continue
    if path.suffix.lower() not in (".wav", ".aif", ".aiff", ".flac"):
        continue
    if render.fullmatch(path.name):
        continue
    match = pattern.fullmatch(path.name)
    if not match:
        problems.append(f"invalid take filename: {path.name}")
        continue
    track, part, ext = match.groups()
    groups.setdefault((int(track), ext), set()).add(int(part) if part else 1)
for (track, ext), parts in groups.items():
    missing = sorted(set(range(1, max(parts) + 1)) - parts)
    if missing:
        problems.append(f"Track_{track:02d}.{ext}: missing part numbers {missing}")

def frames(path):
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
         "stream=sample_rate,time_base,duration_ts,duration", "-of", "json", str(path)],
        capture_output=True, text=True)
    if result.returncode:
        raise ValueError("ffprobe could not read the audio stream")
    streams = json.loads(result.stdout).get("streams", [])
    if not streams:
        raise ValueError("no audio stream")
    stream = streams[0]
    rate = int(stream["sample_rate"])
    if "duration_ts" in stream and "time_base" in stream:
        count = Fraction(int(stream["duration_ts"])) * Fraction(stream["time_base"]) * rate
    else:
        count = Fraction(stream["duration"]) * rate
    return round(count), rate

for index, track in enumerate(report["tracks"], 1):
    names = track.get("files", [])
    if not names or "totalSamplesPrimary" not in track:
        continue  # older reports may omit the frame count
    actual = 0
    for name in names:
        path = audio / name
        if not path.is_file():
            continue  # the SHA check reports the missing file below
        try:
            count, rate = frames(path)
            actual += count
            expected_rate = report.get("sampleRate")
            if expected_rate and abs(rate - float(expected_rate)) > 0.5:
                problems.append(f"{name}: sample rate {rate} differs from report {expected_rate}")
        except (ValueError, KeyError, json.JSONDecodeError) as error:
            problems.append(f"{name}: cannot count audio frames ({error})")
    claimed = int(track["totalSamplesPrimary"])
    if abs(actual - claimed) > 1:
        problems.append(f"track {index}: {actual} audio frames, report claims {claimed}")

for problem in problems:
    print("  FAIL — " + problem)
if not problems:
    print("  OK — frame counts and continuation parts agree")
sys.exit(bool(problems))
PY
    then
        fail=1
    fi

    if [[ "$pending" == "true" ]]; then
        # Hashes are made only inside the app: a quit stops them and opening
        # the session starts them again, so re-running this alone never helps.
        red "  sha256 not finished — verification is incomplete. Hashes are made only while the app has"
        red "  this session open: open it in the app, leave it open until hashing finishes, then re-run."
        fail=1
    else
        # Re-hash each listed primary file and compare to the manifest.
        # The inner read loop uses process substitution (not a pipe) so it
        # runs in THIS shell and a mismatch marker actually propagates.
        hdr "5. sha256 manifest match (re-hashing on disk)"
        marker=$(mktemp)
        listed=0
        n=$(jq '.tracks | length' "$REPORT")
        for ((i=0; i<n; i++)); do
            fileCount=$(jq ".tracks[$i].files // [] | length" "$REPORT")
            hashCount=$(jq ".tracks[$i].sha256 // [] | length" "$REPORT")
            if [[ "$fileCount" != "$hashCount" ]]; then
                red "  track $((i + 1)): $fileCount files but $hashCount hashes"
                echo x >> "$marker"
                continue
            fi
            while IFS=$'\t' read -r f expect; do
                [[ -z "$f" ]] && continue
                ((listed+=1))
                if [[ ! "$expect" =~ ^[0-9a-fA-F]{64}$ ]]; then
                    red "  missing/invalid SHA-256 for $f"
                    echo x >> "$marker"
                    continue
                fi
                if [[ "$f" == */* || "$f" == *..* ]]; then
                    red "  invalid manifest filename: $f"
                    echo x >> "$marker"
                    continue
                fi
                path="$AUDIO/$f"
                [[ -f "$path" ]] || { red "  missing file from manifest: $f"; echo x >> "$marker"; continue; }
                got=$(shasum -a 256 "$path" | awk '{print $1}')
                if [[ "$got" == "$expect" ]]; then grn "  OK  $f"; else red "  MISMATCH  $f"; echo x >> "$marker"; fi
            done < <(paste <(jq -r ".tracks[$i].files // [] | .[]" "$REPORT") \
                           <(jq -r ".tracks[$i].sha256 // [] | .[]" "$REPORT"))
        done
        if (( listed == 0 )); then
            red "  no primary files listed in the final manifest"
            echo x >> "$marker"
        fi
        # A valid hash for each listed file is insufficient if the folder
        # contains an unlisted take, or the manifest lists one file twice.
        duplicates=$(jq -r '[.tracks[].files[]?] | group_by(.)[] | select(length > 1) | .[0]' "$REPORT")
        if [[ -n "$duplicates" ]]; then
            red "  duplicate take filename(s) in manifest: $duplicates"
            echo x >> "$marker"
        fi
        for w in "${media[@]}"; do
            name=$(basename "$w")
            if ! jq -e --arg name "$name" '[.tracks[].files[]?] | index($name) != null' "$REPORT" >/dev/null; then
                red "  unlisted take file: $name"
                echo x >> "$marker"
            fi
        done
        [[ -s "$marker" ]] && fail=1
        rm -f "$marker"
    fi
fi

hdr "Result"
if (( fail == 0 )); then
    grn "ALL CHECKS PASSED — the take is intact."
    exit 0
else
    red "PROBLEMS FOUND — see red lines above."
    exit 1
fi
