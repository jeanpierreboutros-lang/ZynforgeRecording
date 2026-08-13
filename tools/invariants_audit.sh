#!/usr/bin/env bash
# ZynForge Recording — INVARIANTS gate.
#
# Sibling to design_audit.sh (which guards the brand/design system). This one
# guards the CORRECTNESS invariants that four separate audit passes each found
# violated in a NEW place after being "fixed" in an old one.
#
# The lesson that produced this file: `condemnAllStrips` was fixed four times
# across four call sites over three audits, while the identical hazard sat open
# in two other components nobody thought to check. Documentation didn't stop
# that. A grep that fails the build does.
#
# Every rule below is a bug CLASS with at least one shipped defect behind it.
# When you add a legitimate exception, add it to that rule's allow-list with a
# comment saying why -- don't loosen the pattern.
#
# Usage: Tools/invariants_audit.sh     (exit 0 = clean, 1 = violation)

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

SCOPE="Source/UI Source/Audio Source/Network Source/Capture"
FAIL=0

red()   { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }

# report <rule> <explanation> <matches>
report() {
    local rule="$1" why="$2" hits="$3"
    if [[ -n "$hits" ]]; then
        red "✗ $rule"
        printf '   %s\n' "$why"
        printf '%s\n' "$hits" | sed 's/^/     /'
        FAIL=1
    else
        green "✓ $rule"
    fi
}

# ── 1. Every consumer of a cached TrackState& must be condemnable ───────────
# A component that stores `TrackState&` (or TrackState*) and runs a timer will
# read freed memory when the recorder vector shrinks, until whatever rebuilds
# it next ticks. Three components hit this: ChannelStrip, EditPage::TrackRow,
# Meterbridge. Each must expose a detach/invalidate/condemn entry point, and
# MainComponent::condemnAllStrips() must call ALL of them.
CACHERS=$(grep -rln "TrackState& *[a-zA-Z_]*;" $SCOPE --include=*.h 2>/dev/null | sort)
MISSING=""
for f in $CACHERS; do
    grep -q "detach\|invalidate\|condemn" "$f" || MISSING+="$f (stores TrackState& with no detach/invalidate/condemn)"$'\n'
done
report "cached TrackState& consumers expose a detach hook" \
       "add detach()/invalidate() AND wire it into MainComponent::condemnAllStrips()" \
       "$(printf '%s' "$MISSING")"

# Guard the wiring itself: condemnAllStrips must reach all three surfaces.
# Strip comment lines first -- a commented-out call satisfied this check and
# let an injected regression through on the gate's own self-test.
CONDEMN_BODY=$(sed -n '/void MainComponent::condemnAllStrips/,/^}/p' Source/UI/MainComponentStrips.cpp 2>/dev/null \
               | sed 's,//.*,,')
WIRE=""
grep -q "s->invalidate()"            <<<"$CONDEMN_BODY" || WIRE+="condemnAllStrips no longer condemns the MIXER strips"$'\n'
grep -q "condemnAllRows"             <<<"$CONDEMN_BODY" || WIRE+="condemnAllStrips no longer condemns the EDIT rows"$'\n'
grep -q "Meterbridge::condemnAllMeters" <<<"$CONDEMN_BODY" || WIRE+="condemnAllStrips no longer condemns the meterbridge"$'\n'
report "condemnAllStrips covers every TrackState& surface" \
       "all three views cache a TrackState& on a timer; missing one reopens the UAF" \
       "$(printf '%s' "$WIRE")"

# ── 2. getTrack() must never be bounded by a player-derived count ───────────
# player.getNumTracks() comes from FILES ON DISK; recorder.getNumTracks() is the
# mixer. Bounding a TrackState loop on jmax(recorder, player) indexed past the
# end of the vector whenever a session had more takes than strips (crashed the
# stereo-mix bounce). jmax is fine as a pure range CHECK -- flag it only in
# functions that also dereference getTrack().
HITS=""
while IFS= read -r file; do
    [[ -z "$file" ]] && continue
    if grep -q "jmax (recorder.getNumTracks(), player.getNumTracks())" "$file" \
       && grep -q "recorder.getTrack (" "$file"; then
        # Narrow it: flag only when the jmax result feeds a loop that derefs.
        BAD=$(awk '/jmax \(recorder.getNumTracks\(\), player.getNumTracks\(\)\)/{n=NR; name=$0}
                   /recorder\.getTrack \(/{ if (n && NR-n < 40) print FILENAME": "NR": deref within 40 lines of a jmax(recorder,player) bound" }' "$file")
        [[ -n "$BAD" ]] && HITS+="$BAD"$'\n'
    fi
done < <(grep -rl "player.getNumTracks()" $SCOPE 2>/dev/null)
report "no getTrack() bounded by a player-derived count" \
       "the MIXER is authoritative for anything reading mixer state" \
       "$(printf '%s' "$HITS")"

# ── 3. TrackState::name is written only through the locked setter ───────────
# A raw `state.name = ...` races the companion server's getNameThreadSafe() on
# its worker thread -- a torn/freed juce::String. Two sites shipped this.
# `=` not followed by `=` (an == comparison is a message-thread READ, which the
# TrackState contract explicitly allows), and never a comment line.
HITS=$(grep -rn "state\.name *=[^=]\|st\.name *=[^=]\|\.getTrack ([^)]*)\.name *=[^=]" $SCOPE 2>/dev/null \
       | grep -v "setNameThreadSafe" \
       | grep -v ":[0-9]*: *//")
report "TrackState::name written only via setNameThreadSafe" \
       "raw assignment races the companion server's locked read" \
       "$HITS"

# ── 4. Take globs cover every container and exclude punch sidecars ──────────
# "Takes are not always WAV" (CLAUDE.md). A .wav-only glob silently skipped
# FLAC/AIFF sessions in the transient cache, the timeline CSV and the Crop
# multi-part guard. And Track_NN.punchbase.<ext> matches a bare Track_* glob --
# SessionPlayer stitched a crash-orphaned sidecar into the take as part 1.
HITS=$(grep -rn '"Track_\*\.wav"' $SCOPE 2>/dev/null)
report "no .wav-only Track_* globs" \
       "match .wav;.flac;.aif;.aiff -- takes are not always WAV" \
       "$HITS"

HITS=""
while IFS= read -r m; do
    [[ -z "$m" ]] && continue
    f="${m%%:*}"; ln="${m#*:}"; ln="${ln%%:*}"
    # The exclusion must live NEAR the glob (next 25 lines = the loop body), not
    # somewhere in the same file -- an unrelated comment mentioning punchbase
    # used to satisfy this and hid an injected regression.
    WIN=$(sed -n "${ln},$((ln + 25))p" "$f" 2>/dev/null | sed 's,//.*,,')
    grep -q "punchbase" <<<"$WIN" || HITS+="$m"$'\n'
done < <(grep -rn 'findChildFiles.*"Track_\*"' $SCOPE 2>/dev/null)
report "bare Track_* globs account for .punchbase sidecars" \
       "Track_NN.punchbase.<ext> passes an extension filter; exclude it by name" \
       "$(printf '%s' "$HITS")"

# ── 5. New sessions honour the Local Storage override ───────────────────────
# Three record entry points (OSC, companion, transport bar) hardcoded
# ~/Music/Zynforge Sessions, so a remote-triggered take landed on the wrong
# drive. AudioEngine::getSessionsRoot() is the single resolver.
HITS=$(grep -rn 'getChildFile ("Zynforge Sessions")' $SCOPE 2>/dev/null \
       | grep -v "AudioEngine.cpp")
report "sessions root resolved via AudioEngine::getSessionsRoot()" \
       "hardcoding ~/Music ignores the engineer's Local Storage override" \
       "$HITS"

# ── 6. Shared .settings writers reload-to-REPLACE ───────────────────────────
# Five PropertiesFile instances share one file; a bare reload() MERGES, so a
# key another writer just deleted comes back on the next save.
# reload() is only legal as the second half of a clear()+reload() pair, so look
# at the PRECEDING line rather than the match itself.
HITS=$(grep -rn -B1 -- "->reload();" $SCOPE 2>/dev/null \
       | awk '/->reload\(\);$/ { if (prev !~ /clear\(\)/) print; } { prev = $0 }')
report "no bare reload() on the shared settings file" \
       "use reloadAppPropsBeforeWrite() / reloadReplace() -- a plain reload merges" \
       "$HITS"

# ── 7. The audio thread's automation read is never used offline ─────────────
# automationValueAt is try-lock + fall-back-to-static (correct for the audio
# thread). A render that used it baked static fader values into the file.
HITS=$(sed -n '/forEachStereoMixWindow/,/^    bool AudioEngine::renderStereoMix/p' \
       Source/Audio/AudioEngineClips.cpp 2>/dev/null | grep -n "automationValueAt (")
report "offline renders use automationValueAtOffline" \
       "the RT try-lock variant silently bakes static values into a bounce" \
       "$HITS"

# ── 8. Device reconfiguration is guarded against a live take ───────────────
# setAudioDeviceSetup()/initialise() RESTART the device, and
# AudioEngine::audioDeviceStopped -> recorder.release() -> stopRecording(). So
# ANY caller that reconfigures the device mid-take silently ENDS it. The guard
# existed in MainComponent::applySessionSettings and nowhere else -- the DEVICE
# panel let you stop a show by nudging the buffer-size combo.
# AudioEngine's own ctor/init calls are exempt (there's no take at boot).
HITS=""
while IFS= read -r m; do
    [[ -z "$m" ]] && continue
    f="${m%%:*}"; ln="${m#*:}"; ln="${ln%%:*}"
    [[ "$f" == *"AudioEngine.cpp" ]] && continue          # boot-time init
    # The guard must be nearby: same function, so look back 40 lines for an
    # isRecording()/blockedWhileRecording() check. Comments stripped so a
    # commented-out guard can't satisfy it.
    START=$(( ln > 40 ? ln - 40 : 1 ))
    WIN=$(sed -n "${START},${ln}p" "$f" 2>/dev/null | sed 's,//.*,,')
    grep -q "isRecording\|blockedWhileRecording" <<<"$WIN" || HITS+="$m"$'\n'
done < <(grep -rn "setAudioDeviceSetup (\|deviceManager.initialise (\|getDeviceManager().initialise (" $SCOPE 2>/dev/null)
report "device reconfiguration is guarded against a live take" \
       "setAudioDeviceSetup/initialise restart the device, which stops the recorder" \
       "$(printf '%s' "$HITS")"

echo
if [[ $FAIL -eq 0 ]]; then
    green "invariants audit: CLEAN"
else
    red "invariants audit: VIOLATIONS — see above"
fi
exit $FAIL
