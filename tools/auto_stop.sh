#!/usr/bin/env bash
#
# auto_stop.sh — unattended, clean stop for an overnight RF64 soak.
#
# 1. Captures the companion access token off the clipboard (it lands there
#    when you start the companion server: Session ▸ "Start companion server").
# 2. Waits until the take is BOTH past a size threshold (so RF64 has fired)
#    AND a minimum duration, then POSTs the companion `stop` command — the
#    real stopRecording() path (finalises RF64 + writes the report), no quit
#    dialog, app stays running. Hard backstop caps total runtime.
#
# Read-only w.r.t. the app except the one HTTP stop (never force-kills).
# Log: /tmp/zynforge_autostop.log    Abort: touch /tmp/zynforge_autostop.stop
set -uo pipefail

PORT=9000
MIN_SECONDS=$(( 8 * 3600 ))                 # don't stop before 8 h of capture
SIZE_THRESHOLD=$(( 4509715661 ))            # 4.2 GiB — safely past the 4 GiB RF64 point
HARD_CAP=$(( 34200 ))                       # 9.5 h after recording starts
CAPTURE_TIMEOUT=$(( 45 * 60 ))              # give up looking for the token after 45 min
LOG=/tmp/zynforge_autostop.log
STOP=/tmp/zynforge_autostop.stop
TOKFILE=/tmp/zynforge_cmd_token
SESS_BASE="$HOME/Music/Zynforge Sessions"

log(){ printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }
newest_wav(){ ls -t "$SESS_BASE"/*/"Audio Files"/Track_01.wav 2>/dev/null | head -1; }

rm -f "$STOP" "$TOKFILE"
log "# auto_stop armed. waiting for companion token on the clipboard..."
log "# rule: stop when (elapsed >= ${MIN_SECONDS}s AND size >= ${SIZE_THRESHOLD}B) OR elapsed >= ${HARD_CAP}s"

# ── Phase A: capture the token from the clipboard ───────────────────────────
TOKEN=""
cap_deadline=$(( $(date +%s) + CAPTURE_TIMEOUT ))
while (( $(date +%s) < cap_deadline )); do
    [[ -f "$STOP" ]] && { log "aborted before capture"; exit 0; }
    clip="$(pbpaste 2>/dev/null || true)"
    tok="$(printf '%s' "$clip" | grep -oE "127\.0\.0\.1:${PORT}/\?t=[0-9a-fA-F]+" | head -1 | grep -oE "[0-9a-fA-F]+$" || true)"
    if [[ -n "$tok" ]]; then TOKEN="$tok"; printf '%s' "$tok" > "$TOKFILE"; log "captured access token (${#tok} chars)"; break; fi
    sleep 3
done
if [[ -z "$TOKEN" ]]; then log "!! never saw a companion URL on the clipboard — cannot auto-stop. Start the companion server, or stop the take manually."; exit 1; fi

# ── Phase B: wait for the recording, then stop on the rule ──────────────────
t0=0
while :; do
    [[ -f "$STOP" ]] && { log "abort sentinel — exiting without stopping"; exit 0; }
    wav="$(newest_wav)"; size=0
    [[ -n "$wav" && -f "$wav" ]] && size=$(stat -f%z "$wav" 2>/dev/null || echo 0)
    now=$(date +%s)
    if (( size > 0 && t0 == 0 )); then t0=$now; log "recording detected ($wav) — clock started"; fi
    elapsed=0; (( t0 > 0 )) && elapsed=$(( now - t0 ))
    gib=$(awk -v s="$size" 'BEGIN{printf "%.3f", s/1073741824}')

    if (( t0 > 0 )); then
        if (( elapsed >= MIN_SECONDS && size >= SIZE_THRESHOLD )) || (( elapsed >= HARD_CAP )); then
            why="elapsed=${elapsed}s size=${gib}GiB"
            log "STOP condition met ($why) — sending companion stop"
            resp=$(curl -s -m 10 -X POST "http://127.0.0.1:${PORT}/cmd?t=${TOKEN}" \
                        -H "Content-Type: application/json" -d '{"action":"stop"}' 2>&1 || true)
            log "companion responded: ${resp:-<no response>}"
            sleep 5
            wav2="$(newest_wav)"; sz2=0; [[ -n "$wav2" ]] && sz2=$(stat -f%z "$wav2" 2>/dev/null || echo 0)
            log "post-stop file size: $(awk -v s="$sz2" 'BEGIN{printf "%.3f GiB", s/1073741824}')  (should be static now)"
            log "# done. run tools/verify_take.sh to validate the take."
            exit 0
        fi
    fi
    sleep 120
done
