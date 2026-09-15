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

# ── 11. Theme/ must not hardcode a foreground over a caller's accent ───────
# The DESIGN gate bans bare Colours::white/black -- but it EXCLUDES Theme/,
# because Theme/ is where the sanctioned helpers live. That exclusion is how
# ZynForgeLookAndFeel::drawToggleButton shipped a hardcoded white letter over
# whatever accent the call site set: correct only for record red, and about
# 1.2:1 (unreadable) on solo yellow -- the chip an engineer scans mid-show.
# A LookAndFeel paints for EVERY call site, so a wrong constant there is the
# most expensive place in the codebase to put one.
#
# Sanctioned producers of raw white/black are the two helpers themselves:
#   brand::gloss(a)   -- specular sheen
#   brand::onSignal(bg) -- picks the legible foreground FROM the background
# Anything else in Theme/ that names Colours::white/black must justify itself
# in the allow-list below.
HITS=""
while IFS= read -r m; do
    [[ -z "$m" ]] && continue
    f="${m%%:*}"
    # BrandColors.h defines gloss() and onSignal(); those ARE the sanctioned
    # producers. Everything else in Theme/ is a call site.
    [[ "$f" == *"BrandColors.h" ]] && continue
    HITS+="$m"$'\n'
done < <(grep -rn "Colours::white\|Colours::black" Source/Theme 2>/dev/null \
         | sed 's,//.*,,' | grep "Colours::")
report "Theme/ paints accents via brand::onSignal, not hardcoded white/black" \
       "a LookAndFeel paints for every call site -- a fixed foreground over a caller-supplied accent is unreadable on half the palette" \
       "$(printf '%s' "$HITS")"

# ── 12. A std::thread member is never left unjoined behind a flag check ────
# Destroying a joinable std::thread calls std::terminate() -- the whole process
# aborts, no crash dialog, no save. The trap is a reader thread that clears its
# OWN run flag when the peer disconnects: a teardown written as
#
#     if (! running.exchange (false)) return;   // <- skips everything below
#     socket->close();
#     if (reader.joinable()) reader.join();
#
# then early-returns without joining, and the destructor aborts. This shipped
# twice: CaptureLink fixed it in 2026, the console TCP transports repeated it,
# and a Yamaha/A&H desk dropping its link killed the app on the next Connect or
# on quit. Join UNCONDITIONALLY; the flag decides whether to do work, never
# whether to join.
HITS=""
while IFS= read -r m; do
    [[ -z "$m" ]] && continue
    f="${m%%:*}"; ln="${m#*:}"; ln="${ln%%:*}"
    # The candidate line itself must be CODE, not a comment describing the
    # anti-pattern. (Rules 1-7 were once blind to a commented-out call; this is
    # the same mistake mirrored -- a comment producing a false positive.)
    CODE=$(sed -n "${ln}p" "$f" 2>/dev/null | sed 's,//.*,,')
    grep -q "exchange *(false)) *return" <<<"$CODE" || continue
    END=$(( ln + 15 ))
    WIN=$(sed -n "${ln},${END}p" "$f" 2>/dev/null | sed 's,//.*,,')
    grep -q "\.join *()" <<<"$WIN" && HITS+="$m"$'\n'
done < <(grep -rn "exchange (false)) return\|exchange(false)) return" $SCOPE 2>/dev/null)
report "no std::thread join is skipped by an early flag return" \
       "destroying a joinable std::thread calls std::terminate() -- join unconditionally" \
       "$(printf '%s' "$HITS")"

# ── 13. Deferred callbacks never retain naked owner pointers ────────────────
# Timer::callAfterDelay callbacks are independent Timer objects. They can fire
# after a fast quit has destroyed MainComponent. A raw [this] capture shipped
# in every startup callback and made quit-during-launch a UAF window.
HITS=""
while IFS= read -r m; do
    [[ -z "$m" ]] && continue
    f="${m%%:*}"; rest="${m#*:}"; ln="${rest%%:*}"
    WIN=$(sed -n "${ln},$((ln + 2))p" "$f" 2>/dev/null | sed 's,//.*,,' )
    grep -q '\[this' <<<"$WIN" && HITS+="$m"$'\n'
done < <(grep -rn "Timer::callAfterDelay" Source/UI/MainComponent*.cpp 2>/dev/null)
report "MainComponent delayed callbacks use SafePointer" \
       "a delayed raw this capture can fire after fast-quit destroys the window" \
       "$(printf '%s' "$HITS")"

# A member that retains AudioEngine& must be declared after the engine so its
# entire construction/destruction lifetime is nested inside the engine's.
ENGINE_LINE=$(grep -n "AudioEngine *engine" Source/UI/MainComponent.h | head -1 | cut -d: -f1)
MIRROR_LINE=$(grep -n "SessionMirror sessionMirror" Source/UI/MainComponent.h | head -1 | cut -d: -f1)
HITS=""
if [[ -z "$ENGINE_LINE" || -z "$MIRROR_LINE" || "$MIRROR_LINE" -le "$ENGINE_LINE" ]]; then
    HITS="Source/UI/MainComponent.h: SessionMirror must be declared after AudioEngine"
fi
report "engine-reference members are nested inside AudioEngine lifetime" \
       "constructing SessionMirror first binds a reference before AudioEngine lives and destroys it after AudioEngine dies" \
       "$HITS"

# MessageManager jobs may execute after the owner's destructor. AudioEngine's
# shared async handle is the only sanctioned capture in these adapters.
HITS=$(grep -rn "MessageManager::callAsync" Source/Audio Source/Network 2>/dev/null \
       | grep -E '\[eng(,|\])')
report "queued engine callbacks capture the invalidatable async handle" \
       "a raw AudioEngine pointer in a queued callback becomes UAF on shutdown" \
       "$HITS"

HITS=""
grep -q "performRemoteTransport (actionToRun" Source/Network/CompanionServer.cpp \
    || HITS="Source/Network/CompanionServer.cpp: companion transport bypasses AudioEngine's host boundary"
grep -q "performRemoteTransport (AudioEngine::RemoteTransportAction::StartRecord" Source/Audio/OscRemote.cpp \
    || HITS+=$'\nSource/Audio/OscRemote.cpp: OSC record bypasses AudioEngine\047s host boundary'
report "remote transports share the host interception boundary" \
       "daemon mode must intercept phone/OSC record and stop instead of operating the local recorder" \
       "$HITS"

HITS=""
grep -q "pathsafety::isSameOrDescendant (source, dest)" Source/UI/MainComponentSessionIO.cpp \
    || HITS="Source/UI/MainComponentSessionIO.cpp: Save As uses lexical source/destination containment"
grep -q "child.isSymbolicLink()" Source/UI/MainComponentSessionIO.cpp \
    || HITS+=$'\nSource/UI/MainComponentSessionIO.cpp: session copy follows directory symlinks'
report "session copies reject canonical recursion and symlink traversal" \
       "lexical paths let a symlink recurse into the source or copy private files from outside the session" \
       "$HITS"

# Component-owned asynchronous UI callbacks must carry a SafePointer.  Event
# handlers stored by the component itself are synchronous and intentionally not
# covered; only independent timers, menus, choosers, callouts and modal alerts
# can outlive their owner.
HITS=""
while IFS= read -r m; do
    [[ -z "$m" ]] && continue
    f="${m%%:*}"; rest="${m#*:}"; ln="${rest%%:*}"
    WIN=$(sed -n "${ln},$((ln + 8))p" "$f" 2>/dev/null | sed 's,//.*,,' )
    if grep -q '\[this' <<<"$WIN" && ! grep -q 'SafePointer' <<<"$WIN"; then
        HITS+="$m"$'\n'
    fi
done < <(grep -rn -E "MessageManager::callAsync|Timer::callAfterDelay|showAsync|showMenuAsync|launchAsynchronously|launchAsync" Source/UI 2>/dev/null)
report "asynchronous component callbacks carry a SafePointer" \
       "menus, choosers, callouts and modal callbacks can fire after their owning view is destroyed" \
       "$(printf '%s' "$HITS")"

HITS=""
grep -q "child.start (arguments, 0)" Source/Network/CloudUpload.h \
    || HITS="Source/Network/CloudUpload.h: uploader captures pipes owned by a short-lived ChildProcess"
report "detached uploads do not inherit short-lived output pipes" \
       "closing an undrained pipe can SIGPIPE a verbose uploader while the UI reports success" \
       "$HITS"

HITS=""
EXPORT_BODY=$(sed -n '/void MainComponent::startExportTracksTo/,/^}/p' \
              Source/UI/MainComponentSessionIO.cpp 2>/dev/null)
grep -q "sessionIoBusy.exchange (true)" <<<"$EXPORT_BODY" \
    || HITS="Source/UI/MainComponentSessionIO.cpp: background track export is not marked busy"
grep -q "sessionIoBusy.store (false)" <<<"$EXPORT_BODY" \
    || HITS+=$'\nSource/UI/MainComponentSessionIO.cpp: background track export never clears its busy state'
report "background exports hold the session-I/O exclusion gate" \
       "without the gate, Save As/session switching can overlap an export and a second export can block the UI joining a multi-hour encode" \
       "$HITS"

HITS=""
IMPORT_BODY=$(sed -n '/void MainComponent::onImportAudioFiles/,/^}/p' \
              Source/UI/MainComponentSessionIO.cpp 2>/dev/null)
grep -q "exportThread = std::thread" <<<"$IMPORT_BODY" \
    || HITS="Source/UI/MainComponentSessionIO.cpp: audio import no longer runs on the owned worker"
grep -q "audioimport::importFiles" <<<"$IMPORT_BODY" \
    || HITS+=$'\nSource/UI/MainComponentSessionIO.cpp: audio decode/resample has returned to the message thread'
report "audio import stays off the message thread" \
       "decoding/resampling a show-length file inline freezes every control and meter until import ends" \
       "$HITS"

# Session/project metadata and user-facing exports must be staged before
# replacement. A direct replaceWithText truncates an existing file first, so a
# disk-full/crash error destroys the last known-good state.
HITS=$(grep -rn "replaceWithText" $SCOPE --include='*.cpp' --include='*.h' 2>/dev/null \
       | grep -v 'Source/Audio/AtomicFile.h')
report "persistent text writes use atomic replacement" \
       "direct replaceWithText can truncate the previous session/report on failure" \
       "$HITS"

HITS=""
grep -q "exportThread = std::thread" Source/UI/MainComponentTools.cpp \
    || HITS="Source/UI/MainComponentTools.cpp: click-track render returned to the message thread"
grep -q "temporary.replaceFileIn (destination)" Source/Audio/ClickTrackRenderer.cpp \
    || HITS+=$'\nSource/Audio/ClickTrackRenderer.cpp: click render replaces/deletes the previous file before success'
grep -q "installCompletedExport" Source/Audio/TrackExporter.cpp \
    || HITS+=$'\nSource/Audio/TrackExporter.cpp: track export no longer installs transactionally'
grep -q "installBounce" Source/Audio/AudioEngineClips.cpp \
    || HITS+=$'\nSource/Audio/AudioEngineClips.cpp: bounce no longer installs transactionally'
report "long renders are cancellable and transactionally installed" \
       "UI-thread rendering freezes the show UI, while early destination deletion loses the previous deliverable on failure" \
       "$HITS"

HITS=""
grep -q "eng.setRecordStereoMix (stereoMixB.getToggleState())" Source/UI/MainComponentMenu.cpp \
    || HITS="Source/UI/MainComponentMenu.cpp: optional stereo-mix recorder has no user-facing control"
grep -q "stereoMixWriter->write" Source/Audio/AudioEngine.cpp \
    && grep -q "stereoMixWriteFailed.store (true" Source/Audio/AudioEngine.cpp \
    || HITS+=$'\nSource/Audio/AudioEngine.cpp: stereo-mix FIFO/write failure is ignored'
report "optional stereo-mix capture is controllable and failure-visible" \
       "an unreachable option or ignored writer failure produces no mix while the operator believes it is enabled" \
       "$HITS"

HITS=""
grep -q 'findExecutableInPath' Source/Audio/TrackExporter.cpp \
    || HITS="Source/Audio/TrackExporter.cpp: MP3 encoder lookup can regress to a blocking child-process probe"
grep -q 'CharPointer_UTF8::isValidString' Source/Network/CompanionServer.cpp \
    || HITS+=$'\nSource/Network/CompanionServer.cpp: unauthenticated request bodies are decoded without UTF-8 validation'
report "external inputs fail bounded and validated" \
       "a timed-out child-process read or malformed pre-auth network bytes can freeze/assert the app" \
       "$HITS"

HITS=""
grep -q 'COMMAND /usr/bin/codesign --force --sign -' CMakeLists.txt \
    || HITS="CMakeLists.txt: local app bundle is not resealed after the capture helper is embedded"
report "packaged app is sealed after helper embedding" \
       "copying the helper after link invalidates the bundle signature and makes strict verification fail" \
       "$HITS"

HITS=""
DEPLOY_LINE=$(grep -n 'CMAKE_OSX_DEPLOYMENT_TARGET "12.0"' CMakeLists.txt | head -1 | cut -d: -f1)
PROJECT_LINE=$(grep -n '^project(' CMakeLists.txt | head -1 | cut -d: -f1)
if [[ -z "$DEPLOY_LINE" ]]; then
    HITS="CMakeLists.txt: deployment target is outside Xcode 27's supported macOS 12.0+ range"
elif [[ -z "$PROJECT_LINE" || "$DEPLOY_LINE" -ge "$PROJECT_LINE" ]]; then
    HITS="CMakeLists.txt: deployment target is assigned after project(), too late for compiler/toolchain initialization"
fi
report "deployment target is accepted by the supported Xcode toolchain" \
       "Xcode 27 rejects macOS 11, while a target assigned after project() silently links a clean build for the host OS" \
       "$HITS"

echo
if [[ $FAIL -eq 0 ]]; then
    green "invariants audit: CLEAN"
else
    red "invariants audit: VIOLATIONS — see above"
fi
exit $FAIL
