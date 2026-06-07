#!/usr/bin/env bash
#
# soak_monitor.sh — long-run stability + RF64 watch for a ZynForge recording.
#
# Samples the app process and the growing take every INTERVAL seconds and logs
# CPU / RAM, file growth, the RIFF -> RF64 promotion (the >4 GiB event), free
# disk, and any crash report. Built for an unattended multi-hour soak; runs
# independently of any shell (launch with nohup) and writes a timestamped log
# you can tail anytime.
#
# Usage:
#   nohup tools/soak_monitor.sh [interval_s] [max_hours] [logfile] >/dev/null 2>&1 &
#   # defaults: 60 s, 11 h, /tmp/zynforge_soak_<date>.log
#   # stop early:  touch /tmp/zynforge_soak.stop
#
# Reads only — never signals the app (force-killing wedges the HDSPe device).

set -uo pipefail

INTERVAL="${1:-60}"
MAX_HOURS="${2:-11}"
LOG="${3:-/tmp/zynforge_soak_$(date +%Y%m%d_%H%M%S).log}"
STOP="/tmp/zynforge_soak.stop"
SESS_BASE="$HOME/Music/Zynforge Sessions"

log(){ printf '%s\n' "$*" >> "$LOG"; }

newest_wav(){ ls -t "$SESS_BASE"/*/"Audio Files"/Track_01.wav 2>/dev/null | head -1; }

rm -f "$STOP"
log "# ZynForge soak monitor — started $(date)"
log "# interval=${INTERVAL}s  max=${MAX_HOURS}h  stop: touch $STOP"
log "# time      pid     rss_MB  cpu%   mem%   file_GiB  hdr   grow_MB/min  free_GiB  crashes"

prev_size=-1; seen_pid=0; rf64=0; samples=0; peak_rss=0; peak_cpu=0
maxticks=$(( MAX_HOURS * 3600 / INTERVAL )); ticks=0

while :; do
    ts=$(date +%H:%M:%S)
    pid=$(pgrep -f "Zynforge Recording" | head -1)
    if [[ -n "$pid" ]]; then
        seen_pid=1
        read -r rss cpu mem <<<"$(ps -p "$pid" -o rss=,%cpu=,%mem= 2>/dev/null)"
        rss_mb=$(( ${rss:-0} / 1024 ))
        (( rss_mb > peak_rss )) && peak_rss=$rss_mb
        peak_cpu=$(awk -v a="$peak_cpu" -v b="${cpu:-0}" 'BEGIN{print (b>a)?b:a}')
    else
        rss_mb=0; cpu="-"; mem="-"
        (( seen_pid == 1 )) && log "!! $ts  APP NOT RUNNING (stopped or crashed)"
    fi

    wav="$(newest_wav)"
    if [[ -n "$wav" && -f "$wav" ]]; then
        size=$(stat -f%z "$wav")
        gib=$(awk -v s="$size" 'BEGIN{printf "%.3f", s/1073741824}')
        hdr=$(xxd -l 4 -p "$wav" 2>/dev/null); tag="RIFF"; [[ "$hdr" == "52463634" ]] && tag="RF64"
        if [[ "$tag" == "RF64" && $rf64 -eq 0 ]]; then
            rf64=1; log "** $ts  RF64 PROMOTION — file crossed 4 GiB at ${gib} GiB **"
        fi
        if (( prev_size >= 0 )); then
            grow=$(awk -v a="$prev_size" -v b="$size" -v i="$INTERVAL" 'BEGIN{printf "%.1f",(b-a)/1048576/(i/60)}')
        else grow="-"; fi
        prev_size=$size
    else size=0; gib="0.000"; tag="--"; grow="-"; fi

    free_gib=$(df -g "$(dirname "${wav:-$SESS_BASE}")" 2>/dev/null | awk 'NR==2{print $4}')
    crashes=$(ls "$HOME/Library/Logs/DiagnosticReports/Zynforge"*.ips 2>/dev/null | wc -l | tr -d ' ')

    log "$(printf '%-9s %-7s %-7s %-6s %-6s %-9s %-5s %-12s %-9s %s' \
        "$ts" "${pid:--}" "$rss_mb" "${cpu}" "${mem}" "$gib" "$tag" "$grow" "${free_gib:-?}" "$crashes")"
    samples=$((samples+1))

    [[ -f "$STOP" ]] && { log "# stop sentinel — exiting"; break; }
    ticks=$((ticks+1)); (( ticks >= maxticks )) && { log "# max duration reached"; break; }
    sleep "$INTERVAL"
done

log "# SUMMARY  samples=$samples  peak_rss=${peak_rss}MB  peak_cpu=${peak_cpu}%  rf64=$([[ $rf64 -eq 1 ]] && echo YES || echo NO)  crashes=$crashes"
log "# ended $(date)"
