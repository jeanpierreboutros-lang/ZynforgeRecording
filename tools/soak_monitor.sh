#!/usr/bin/env bash
#
# soak_monitor.sh — long-run stability + RF64 watch for a ZynForge recording.
#
# Samples the app process and the growing take every INTERVAL seconds and logs
# CPU / RAM, file growth, the RIFF -> RF64 promotion (the >4 GiB event), free
# disk, and any crash report since the monitor started. Built for an unattended
# multi-hour soak; runs independently of any shell (launch with nohup) and
# writes a timestamped log you can tail anytime.
#
# Usage:
#   nohup tools/soak_monitor.sh [interval_s] [max_hours] [logfile] [session_dir] >/dev/null 2>&1 &
#   # defaults: 60 s, 11 h, /tmp/zynforge_soak_<date>.log, and the session the
#   # app last recorded, read from its settings on every sample (session folders
#   # can live anywhere, so nothing is guessed from a fixed root)
#   # stop early:  touch /tmp/zynforge_soak.stop
#
# The summary says what was actually seen. A log with lines beginning "!!" did
# not observe a clean soak, whatever the columns look like.
#
# Reads only — never signals the app (force-killing wedges the HDSPe device).

set -uo pipefail

INTERVAL="${1:-60}"
MAX_HOURS="${2:-11}"
LOG="${3:-/tmp/zynforge_soak_$(date +%Y%m%d_%H%M%S).log}"
SESSION_ARG="${4:-}"
STOP="/tmp/zynforge_soak.stop"
SETTINGS="$HOME/Library/Application Support/Zynforge Recording/Zynforge Recording.settings"
CRASH_DIR="$HOME/Library/Logs/DiagnosticReports"

log(){ printf '%s\n' "$*" >> "$LOG"; }

# The same rule as verify_take.sh: the first key with a value decides.
session_dir(){
    if [[ -n "$SESSION_ARG" ]]; then printf '%s\n' "${SESSION_ARG%/}"; return; fi
    python3 -I - "$SETTINGS" 2>/dev/null <<'PY'
import sys
import xml.etree.ElementTree as ElementTree

values = {row.get("name"): row.get("val", "") for row in ElementTree.parse(sys.argv[1]).getroot().iter("VALUE")}
print(values.get("lastCaptureDir") or values.get("activeSessionDir") or "")
PY
}

# The newest take file of any strip, in any container. Renders and punch
# sidecars are not the take.
newest_take(){
    local audio="$1/Audio Files"
    [[ -d "$audio" ]] || audio="$1"
    find "$audio" -maxdepth 1 -type f \
        \( -name 'Track_*.wav' -o -name 'Track_*.aif' -o -name 'Track_*.aiff' -o -name 'Track_*.flac' \) \
        ! -name '*.punchbase*' ! -name '*_consolidated_*' -exec stat -f '%m %N' {} + 2>/dev/null \
        | sort -rn | head -1 | cut -d' ' -f2-
}

# Only this app's own reports: the name decides, because other ZynForge tools
# share the prefix, and -newer because the folder keeps every crash ever.
new_crashes(){
    find "$CRASH_DIR" -maxdepth 1 -type f \
        \( -name 'Zynforge Recording-*.ips' -o -name 'ZynforgeCapture-*.ips' \) \
        -newer "$MARK" 2>/dev/null | wc -l | tr -d ' '
}

MARK=$(mktemp "${TMPDIR:-/tmp}/zynforge_soak_mark.XXXXXX")
trap 'rm -f "$MARK"' EXIT

rm -f "$STOP"
log "# ZynForge soak monitor — started $(date)"
log "# interval=${INTERVAL}s  max=${MAX_HOURS}h  session=${SESSION_ARG:-the one the app last recorded}  stop: touch $STOP"
log "# time      pid     rss_MB  cpu%   mem%   file_GiB  hdr   grow_MB/min  free_GiB  new_crashes"

prev_size=-1; seen_pid=0; rf64=0; samples=0; peak_rss=0; peak_cpu=0
watched=""; watched_session=""; watched_samples=0; growing_samples=0; idle_warned=0; missing_warned=0; crashes=0
maxticks=$(awk -v h="$MAX_HOURS" -v i="$INTERVAL" 'BEGIN{print int(h*3600/i)}'); ticks=0

while :; do
    ts=$(date +%H:%M:%S)
    # -x: the daemon lives in the same bundle, so a match on the path would
    # still find "the app" with the window dead.
    pid=$(pgrep -x "Zynforge Recording" | head -1)
    if [[ -n "$pid" ]]; then
        seen_pid=1
        read -r rss cpu mem <<<"$(ps -p "$pid" -o rss=,%cpu=,%mem= 2>/dev/null)"
        rss_mb=$(( ${rss:-0} / 1024 ))
        (( rss_mb > peak_rss )) && peak_rss=$rss_mb
        peak_cpu=$(awk -v a="$peak_cpu" -v b="${cpu:-0}" 'BEGIN{print (b>a)?b:a}')
    else
        rss_mb=0; cpu="-"; mem="-"
        if pgrep -x ZynforgeCapture >/dev/null; then
            log "!! $ts  APP NOT RUNNING (stopped or crashed); the capture daemon is still running"
        else
            log "!! $ts  APP NOT RUNNING (stopped or crashed)"
        fi
    fi

    session="$(session_dir)"
    # One file while it grows, or the growth column compares different files:
    # every armed file grows at once. A part roll or the end of the take is
    # what lets another one take over.
    if [[ "$session" != "$watched_session" || -z "$watched" || ! -f "$watched" ]] || (( idle_warned == 1 )); then
        next=""
        [[ -n "$session" ]] && next="$(newest_take "$session")"
        if [[ "$next" != "$watched" ]]; then watched="$next"; prev_size=-1; idle_warned=0; fi
        watched_session="$session"
    fi

    if [[ -n "$watched" && -f "$watched" ]]; then
        missing_warned=0
        watched_samples=$((watched_samples+1))
        size=$(stat -f%z "$watched")
        gib=$(awk -v s="$size" 'BEGIN{printf "%.3f", s/1073741824}')
        case "$(xxd -l 4 -p "$watched" 2>/dev/null)" in
            52494646) tag="RIFF" ;;
            52463634) tag="RF64" ;;
            464f524d) tag="AIFF" ;;
            664c6143) tag="FLAC" ;;
            *)        tag="??" ;;
        esac
        if [[ "$tag" == "RF64" && $rf64 -eq 0 ]]; then
            rf64=1; log "** $ts  RF64 PROMOTION — file crossed 4 GiB at ${gib} GiB **"
        fi
        if (( prev_size >= 0 )); then
            grow=$(awk -v a="$prev_size" -v b="$size" -v i="$INTERVAL" 'BEGIN{printf "%.1f",(b-a)/1048576/(i/60)}')
            if (( size > prev_size )); then
                growing_samples=$((growing_samples+1)); idle_warned=0
            elif (( idle_warned == 0 )); then
                idle_warned=1
                log "!! $ts  TAKE FILE NOT GROWING: $(basename "$watched")"
            fi
        else grow="-"; fi
        prev_size=$size
    else
        size=0; gib="0.000"; tag="--"; grow="-"
        if (( missing_warned == 0 )); then
            missing_warned=1
            log "!! $ts  NOTHING TO WATCH: no take file in ${session:-any session the app names}; pass the session folder as the 4th argument"
        fi
    fi

    free_gib=$(df -g "$(dirname "${watched:-${session:-$HOME}/.}")" 2>/dev/null | awk 'NR==2{print $4}')
    found=$(new_crashes)
    (( found > crashes )) && log "!! $ts  NEW CRASH REPORT in $CRASH_DIR"
    crashes=$found

    log "$(printf '%-9s %-7s %-7s %-6s %-6s %-9s %-5s %-12s %-9s %s' \
        "$ts" "${pid:--}" "$rss_mb" "${cpu}" "${mem}" "$gib" "$tag" "$grow" "${free_gib:-?}" "$crashes")"
    samples=$((samples+1))

    [[ -f "$STOP" ]] && { log "# stop sentinel — exiting"; break; }
    ticks=$((ticks+1)); (( ticks >= maxticks )) && { log "# max duration reached"; break; }
    sleep "$INTERVAL"
done

log "# SUMMARY  samples=$samples  app_seen=$([[ $seen_pid -eq 1 ]] && echo YES || echo NO)  take_file_samples=$watched_samples  growing_samples=$growing_samples  peak_rss=${peak_rss}MB  peak_cpu=${peak_cpu}%  rf64=$([[ $rf64 -eq 1 ]] && echo YES || echo NO)  new_crashes=$crashes"
(( growing_samples > 0 )) || log "!! NO TAKE FILE WAS SEEN GROWING: this log says nothing about a recording"
log "# ended $(date)"
