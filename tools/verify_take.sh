#!/usr/bin/env bash
#
# verify_take.sh — turnkey post-take integrity check for a ZynForge session.
#
# Closes the manual half of the RF64 / throughput field soak (FIELD-TEST.md
# section "Throughput + RF64"). Point it at a session folder; it checks every
# recorded WAV the way you'd otherwise do by hand:
#
#   • opens at full length (ffprobe duration + frame count)
#   • header is RIFF (<4 GiB) or RF64 + ds64 (>4 GiB) — and flags any file
#     that crossed 4 GiB but did NOT get promoted to RF64 (the failure mode)
#   • continuation/split parts have a playable Track_NN base
#   • matches session.report.json: track count, complete per-file sha256,
#     and reports `missedSamples`
#
# Usage:
#   tools/verify_take.sh [SESSION_DIR]
#
# With no argument it picks the most-recently-modified session under
# ~/Music/Zynforge Sessions. Exit code 0 = all green, 1 = a problem was found.
#
# Requires: ffprobe, xxd, shasum, jq, python3 (all stock on a dev Mac).

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
    base="$HOME/Music/Zynforge Sessions"
    [[ -d "$base" ]] || { red "no session dir given and $base does not exist"; exit 2; }
    SESSION=$(ls -dt "$base"/*/ 2>/dev/null | head -1)
    [[ -n "$SESSION" ]] || { red "no sessions found under $base"; exit 2; }
fi
SESSION="${SESSION%/}"
[[ -d "$SESSION" ]] || { red "not a directory: $SESSION"; exit 2; }

AUDIO="$SESSION/Audio Files"
[[ -d "$AUDIO" ]] || AUDIO="$SESSION"   # legacy: WAVs in the session root
REPORT="$SESSION/session.report.json"

hdr "Session: $SESSION"

# ── Continuation parts need a playable base file ────────────────────────────
hdr "1. Continuation-part check"
parts=$(find "$AUDIO" -name 'Track_*_part*.wav' 2>/dev/null | sort)
if [[ -n "$parts" ]]; then
    while IFS= read -r part; do
        name=$(basename "$part")
        base="${name%%_part*}.wav"
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
wavs=("$AUDIO"/Track_*.wav)
if (( ${#wavs[@]} == 0 )); then
    red "no Track_*.wav files found in $AUDIO"; exit 1
fi
for w in "${wavs[@]}"; do
    name=$(basename "$w")
    size=$(stat -f%z "$w")
    magic=$(xxd -l 4 -p "$w")                       # 52494646=RIFF  52463634=RF64
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$w" 2>/dev/null)
    frames=$(ffprobe -v error -select_streams a:0 -show_entries stream=duration_ts -of csv=p=0 "$w" 2>/dev/null)
    human=$(python3 -c "print(f'{$size/1e9:.2f} GB')" 2>/dev/null)

    tag="RIFF"; [[ "$magic" == "52463634" ]] && tag="RF64"
    line="  $name  $human  hdr=$tag  dur=${dur:-?}s"

    if [[ -z "$dur" || "$dur" == "N/A" ]]; then
        red "$line  -> FAILS TO OPEN"; fail=1; continue
    fi
    if (( size > FOUR_GIB )) && [[ "$tag" != "RF64" ]]; then
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
    echo "  numTracks=$ntracks  missedSamples=$missed  sha256Pending=$pending"
    if [[ "$ntracks" != "$manifestTracks" ]]; then
        red "  track manifest has $manifestTracks entries, report says $ntracks"
        fail=1
    fi
    [[ "$missed" == "0" ]] && grn "  OK — missedSamples = 0" || { red "  missedSamples = $missed (DROPPED AUDIO)"; fail=1; }

    if [[ "$pending" == "true" ]]; then
        red "  sha256 still hashing — verification is incomplete; re-run after it finishes."
        fail=1
    else
        # Re-hash each listed primary file and compare to the manifest.
        # The inner read loop uses process substitution (not a pipe) so it
        # runs in THIS shell and a mismatch marker actually propagates.
        hdr "4. sha256 manifest match (re-hashing on disk)"
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
