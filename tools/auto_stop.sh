#!/usr/bin/env bash
#
# auto_stop.sh — unattended, clean stop for an overnight RF64 soak.
#
# 1. Captures the companion access token off the clipboard (copied when you
#    start the companion server or choose "Copy read-only confidence URL").
# 2. Watches the authenticated live capture status for one pinned session.
#    After the size/duration threshold (or hard cap), completes the companion
#    STOP confirmation and checks that the same session is no longer rolling.
#
# Read-only w.r.t. the app except confirmed HTTP STOP requests (never force-kills).
# Log: /tmp/zynforge_autostop.log    Abort: touch /tmp/zynforge_autostop.stop
set -uo pipefail

PORT=${ZYNFORGE_AUTOSTOP_PORT:-9000}
MIN_SECONDS=${ZYNFORGE_AUTOSTOP_MIN_SECONDS:-$(( 8 * 3600 ))}
SIZE_THRESHOLD=${ZYNFORGE_AUTOSTOP_SIZE_THRESHOLD:-4509715661}
HARD_CAP=${ZYNFORGE_AUTOSTOP_HARD_CAP:-34200}
POLL_SECONDS=${ZYNFORGE_AUTOSTOP_POLL_SECONDS:-120}
STOP_TIMEOUT=${ZYNFORGE_AUTOSTOP_STOP_TIMEOUT:-300}
CAPTURE_TIMEOUT=$(( 45 * 60 ))              # give up looking for the token after 45 min
LOG=/tmp/zynforge_autostop.log
STOP=/tmp/zynforge_autostop.stop
REQUESTED_SESSION="${1:-}"

log(){ printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }
read_state(){ curl -fsS -m 10 "http://127.0.0.1:${PORT}/state.json?t=${TOKEN}"; }
send_stop(){ curl -sS -m 10 -X POST "http://127.0.0.1:${PORT}/cmd?t=${TOKEN}" \
                    -H "Content-Type: application/json" -d '{"action":"stop"}'; }

rm -f "$STOP"
log "# auto_stop armed. waiting for companion token on the clipboard..."
log "# rule: stop when (elapsed >= ${MIN_SECONDS}s AND size >= ${SIZE_THRESHOLD}B) OR elapsed >= ${HARD_CAP}s"

# ── Phase A: capture the token from the clipboard ───────────────────────────
TOKEN=""
cap_deadline=$(( $(date +%s) + CAPTURE_TIMEOUT ))
while (( $(date +%s) < cap_deadline )); do
    [[ -f "$STOP" ]] && { log "aborted before capture"; exit 0; }
    clip="$(pbpaste 2>/dev/null || true)"
    tok="$(printf '%s' "$clip" | grep -oE "(localhost|127\.0\.0\.1):${PORT}/(confidence)?\?t=[0-9a-fA-F]+" | head -1 | grep -oE "[0-9a-fA-F]+$" || true)"
    if [[ -n "$tok" ]]; then TOKEN="$tok"; log "captured access token (${#tok} chars)"; break; fi
    sleep 3
done
if [[ -z "$TOKEN" ]]; then log "!! never saw a companion URL on the clipboard — cannot auto-stop. Start the companion server, or stop the take manually."; exit 1; fi

# ── Phase B: pin the live take, then stop on the rule ────────────────────────
target=""
state_failures=0
while :; do
    [[ -f "$STOP" ]] && { log "abort sentinel — exiting without stopping"; exit 0; }
    if ! state=$(read_state) || ! printf '%s' "$state" | jq -e 'type == "object" and (.recording | type == "boolean")' >/dev/null 2>&1; then
        state_failures=$((state_failures + 1))
        log "!! cannot read authenticated capture status (${state_failures}/3)"
        (( state_failures >= 3 )) && exit 1
        sleep "$POLL_SECONDS"
        continue
    fi
    state_failures=0
    recording=$(printf '%s' "$state" | jq -r '.recording')
    session=$(printf '%s' "$state" | jq -r '.sessionPath // empty')
    if [[ "$recording" != "true" ]]; then
        if [[ -n "$target" ]]; then log "take stopped before auto-stop threshold"; exit 0; fi
        sleep "$POLL_SECONDS"
        continue
    fi
    if [[ -z "$session" ]]; then log "!! rolling capture has no session path; refusing an unscoped STOP"; exit 1; fi
    if [[ -n "$REQUESTED_SESSION" && "$session" != "${REQUESTED_SESSION%/}" ]]; then
        log "!! live session is $session, expected $REQUESTED_SESSION; refusing STOP"
        exit 1
    fi
    if [[ -z "$target" ]]; then target="$session"; log "pinned live session: $target"; fi
    if [[ "$session" != "$target" ]]; then log "!! live session changed to $session; refusing STOP"; exit 1; fi

    wav="$target/Audio Files/Track_01.wav"
    if [[ ! -f "$wav" ]]; then log "!! no live Track_01.wav in pinned session; RF64 soak cannot be monitored"; exit 1; fi
    size=$(stat -f%z "$wav" 2>/dev/null || echo 0)
    elapsed=$(printf '%s' "$state" | jq -r 'if .sampleRate > 0 then (.elapsedSamples / .sampleRate | floor) else 0 end')
    gib=$(awk -v s="$size" 'BEGIN{printf "%.3f", s/1073741824}')

    if (( (elapsed >= MIN_SECONDS && size >= SIZE_THRESHOLD) || elapsed >= HARD_CAP )); then
        log "STOP condition met (elapsed=${elapsed}s size=${gib}GiB)"
        # The host deliberately requires two STOP presses within two seconds.
        # A 409 'STOP armed' is the first confirmation, not a failed take.
        # Capture becoming idle precedes asynchronous media/metadata completion.
        # Only an affirmative STOP reply acknowledges the completed outcome.
        acknowledged=0
        attempt=0
        stop_deadline=$(( SECONDS + STOP_TIMEOUT ))
        while (( SECONDS < stop_deadline )); do
            [[ -f "$STOP" ]] && { log "abort sentinel — exiting without further STOP requests"; exit 0; }
            if (( attempt > 0 )); then
                # A pending completion may require many retries. Pin each one
                # to the same current session instead of trusting the original
                # status snapshot after another take has been opened.
                if ! state=$(read_state) || ! printf '%s' "$state" | jq -e \
                        'type == "object" and (.recording | type == "boolean")' >/dev/null 2>&1; then
                    log "!! cannot revalidate the pinned session; withholding STOP retry"
                    sleep 1
                    continue
                fi
                session=$(printf '%s' "$state" | jq -r '.sessionPath // empty')
                if [[ "$session" != "$target" ]]; then
                    log "!! live session changed; refusing further STOP requests"
                    exit 1
                fi
            fi
            attempt=$(( attempt + 1 ))
            if ! resp=$(send_stop); then
                log "!! STOP request ${attempt} could not reach the companion"
            elif printf '%s' "$resp" | jq -e '.ok == true' >/dev/null 2>&1; then
                log "STOP accepted by host"
                acknowledged=1
                break
            else
                error=$(printf '%s' "$resp" | jq -r '.error // "invalid response"' 2>/dev/null)
                log "STOP request ${attempt}: $error"
                case "$error" in
                    *"STOP armed"*|*"finalization pending"*) ;;
                    *) log "!! host did not acknowledge a successful finalization"; exit 1 ;;
                esac
            fi
            sleep 1
        done
        if (( acknowledged == 0 )); then
            log "!! finalization was not acknowledged within ${STOP_TIMEOUT}s"
            exit 1
        fi
        for check in 1 2 3 4 5 6; do
            if state=$(read_state) && printf '%s' "$state" | jq -e --arg target "$target" \
                    '.recording == false and .sessionPath == $target' >/dev/null 2>&1; then
                log "confirmed stopped: $target"
                log "# done. run tools/verify_take.sh to validate the take."
                exit 0
            fi
            sleep 1
        done
        log "!! STOP was not confirmed; recording may still be running"
        exit 1
    fi
    sleep "$POLL_SECONDS"
done
