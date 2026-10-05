# October 3 codebase audit fixes

All 28 initial findings and the 14 follow-up findings below have fixes. Validation completed on 2026-10-04; at that checkpoint `c563b00` was installed on both Macs. Both Macs and the current DMG now have the later `443c769` follow-up. See [INSTALL.md](INSTALL.md) for the current package, hashes, startup checks and old-version cleanup.

| # | Severity | Finding and change |
|---|---|---|
| 1 | High | Backup/mirror basename collisions: destinations now carry primary-session provenance; foreign or unverifiable existing copies are refused. Writers never truncate unexpected files. Identity survives relocation, while a still-existing source prevents a copied session from claiming its backup. |
| 2 | High | Rearmed continuation tracks: pad each destination from its own prior length to the take's common continuation position. A newly connected mirror also receives the correct leading gap. |
| 3 | High | Pre-roll channel drift: detach all histories and enable live FIFO capture at one audio-block boundary. Disk writes read the detached snapshot. Shorter histories receive leading silence. |
| 4 | High | Mono/stereo changes between takes: refuse incompatible channel layouts/sample rates before creating parts; multipart readers also reject incompatible parts. |
| 5 | High | PRE changes during live input: allocate replacement buffers off the callback and swap them at the capture boundary. Old storage is released after the swap. |
| 6 | High | Relocation read failures: require successful input/output status, exact copied lengths, and unchanged source length before considering a copy complete and deleting the original. |
| 7 | High | Missing/read-failed arrangement media: propagate failure through rendering, strip-silence analysis, consolidation, and bounce; failed work preserves original clips/files. |
| 8 | High | Device loss: local capture records the failure; daemon warnings survive STOP and remain available to the UI. |
| 9 | Medium | Pre-roll transport position: include committed history in the sample counter used by the timeline and markers. |
| 10 | Medium | Fresh daemon takes: use continuation only when an actual base take exists, preserving fresh-take pre-roll. |
| 11 | Medium | Daemon STOP after edited continuation: explicitly append new recorded media when reloading preserved clip lists. |
| 12 | Medium | Legacy stereo bounce: resolve root-level media even when an Audio Files folder has subsequently been created. |
| 13 | Medium | Locked consolidation: reject ranges intersecting locked clips before rendering or replacement, including asynchronous jobs. |
| 14 | Medium | Missing/invalid mixer fallback: clearing a session resets strip controls, bus/send/group state, routing, and gain compensation so the previous show cannot leak into the next. |
| 15 | Medium | VCA persistence: save/restore gain, mute, and solo; reset controls and pending ramps at session boundaries. |
| 16 | Medium | Stereo aux-bus bounce: render source audio and accumulate pre/post-fader sends before solo filtering, including automation and source mute. |
| 17 | Medium | Offline mirrors: retain configured paths across startup so readiness can report them and reconnect can reuse them. |
| 18 | Medium | Explicit input None: preserve -1 when restoring routing with a live device. |
| 19 | Medium | Auto-arm: enable input metering for unarmed/unmonitored strips while input detection is enabled; do not auto-arm during capture or session LOCK. |
| 20 | Medium | Group cut/paste: copy every selected editable clip before deletion, retain track/time offsets, source-channel identity, fades, gain and mute, and validate all paste destinations first. |
| 21 | Medium | Slow snapped dragging: retain unapplied pointer movement until it crosses a snap boundary. |
| 22 | Medium | Group drag matching: resolve all peer indices before mutating any source clip. |
| 23 | Medium | Healing: require matching gain, mute, source channel, fade curve and an unfaded internal join. |
| 24 | Medium | Remote LOCK: enforce the engine's published lock state at companion, OSC and MCU entry points and queued MCU callbacks. |
| 25 | Medium | MCU stereo gain: resolve either physical half to its stereo partner and use the engine gain setters for both. |
| 26 | Medium | Stereo throughput: count both interleaved channels in live and silence-padding byte estimates. |
| 27 | Medium | Test preferences: all five PropertiesFile writers share a per-process temporary file; isolation remains enabled even when individual tests restore audio initialization. |
| 28 | Medium | Automation shape: preserve tension on copy/paste and preserve curve/tension while dragging points. |

## Compatibility and operation

Existing backups without provenance are deliberately not adopted automatically: ownership cannot be established from a session basename. Record-start errors identify the conflicting destination and request an empty backup/mirror folder. Existing audio is preserved. Changing the recorded mono/stereo layout or sample rate similarly requires restoring the original setup or starting a new session.

`--run-tests` uses isolated temporary preferences. `--isolated-settings` launches the normal UI with temporary preferences for smoke checks. Neither mode writes the engineer's saved settings.

## Validation

- Universal Release build passed for both the GUI and embedded capture helper (`arm64` and `x86_64`).
- The built app passed **424 test groups with zero failures** on Apple Silicon, including 20 new October regression groups.
- Design audit, invariant audit, whitespace checks, and strict/deep bundle-signature verification passed.
- A 30-second isolated UI launch reached the Welcome window, used approximately 115–127 MiB RSS and 2.5–4.3% CPU, produced no runtime log output, and created no new Zynforge crash report. The disposable smoke process was then closed.
- SHA-256 checks confirmed both existing settings files remained unchanged after tests and smoke validation.

The regression suite includes recording/rearming, concurrent pre-roll, replica collisions and relocation, layout refusal, missing media, legacy bounce, locked consolidation, session/VCA state, daemon warnings, auto-arm, edited continuation, solo aux sends, healing, automation, offline mirrors, grouped clipboard, snapped group dragging, OSC/MCU controls and settings isolation. The daemon and companion integration tests also exercise the corrected paths.

Physical interface removal/reconnection, removable-volume failures, control-surface hardware, and the planned full rig rehearsal still require field acceptance. Automated synthetic audio and loopback tests do not substitute for those checks.


## Follow-up audit: 14 additional findings

The next read-only review found the following failures. Each now has a source fix and coverage in `OctoberRegressionTests.cpp`; validation completed on 2026-10-04.

| # | Severity | Fix and regression |
|---|---|---|
| 1 | P1 | Reject overlapping stereo links in UI/engine and invalid persisted pair layouts before opening capture files. Capture a valid three-channel layout and reject a corrupted overlap without writing media. |
| 2 | P1 | Persist generated click beds as reference media. Exclude them from default append position and recorder length reconciliation while retaining deliberate cursor/rolling punches. Test first capture, reopen and continuation against a longer reference bed. |
| 3 | P1 | Preserve saved stage routing on reconnect. The console menu offers restoration when connection state is unknown; direct re-entry cannot overwrite the saved patch. Exercise the UI toggle with simulated desk replies. |
| 4 | P1 | Reserve session I/O while delete confirmation is open; recheck recording and current session identity at deletion. Verify active capture media survives a delayed delete and switching sessions does not clear the new session. |
| 5 | P2 | Close the stereo mix writer on device stop, retain the interrupted endpoint and queue engine finalization once. Verify media reload, matching mix/take duration and repeated STOP preserving edits. |
| 6 | P2 | Persist capture input gain, initialize reopened live gain to its reference, and never treat zero dB as unknown. Verify +3 dB stays +3 dB after reopening and mid-take updates retain a zero reference. |
| 7 | P2 | Feed silence for any captured channel lacking an input, so either stereo side can continue. Verify missing left input produces zero left plus intact right with the full frame count. |
| 8 | P2 | Merge modern and legacy media per track, preferring modern media consistently. Verify root tracks remain available after adding Audio Files tracks. |
| 9 | P2 | Clear the rejected mono fallback source identity so adjacent pasted clips open their explicit source reader. Verify rendering and consolidation. |
| 10 | P2 | Bound right trims by source length, including cross-track sources. Verify extension stops at EOF and remains renderable. |
| 11 | P2 | Constrain grouped edits to a common delta before moving any peer. Verify a real drag against timeline zero retains the original relative offset. |
| 12 | P2 | Feed live inputs into aux sends, avoid doubling punch input and exclude playback trim compensation from live input gain. Verify audible live aux. |
| 13 | P2 | Include VCA mute in live and offline post-fader send gates; retain independent pre-fader behavior. Verify both render paths. |
| 14 | P2 | Use input-only peaks for auto-arm and arm stereo pairs together. Verify playback alone leaves arms off and right-only input arms both halves. |

Physical desk reconnect and interface removal still require disposable rig validation. Synthetic tests do not certify physical hardware behavior. Both installed apps and the current DMG now include these fixes.


### Follow-up validation (2026-10-04)

- `cmake --build build --config Release` passed. The GUI and embedded capture helper both contain arm64 and x86_64 architectures.
- Full Release suite: **438 test groups, zero failures** on Apple Silicon, including all 14 new regression groups. Two older auto-arm fixtures now feed actual input instead of setting the display meter; the trim/undo fixture now supplies source media and asserts bounded trimming.
- `tools/design_audit.sh`, `tools/invariants_audit.sh`, `git diff --check` and `codesign --verify --deep --strict` passed.
- A 30-second `--isolated-settings` startup smoke reached the normal empty-session UI. Samples at 10/20/30 seconds were 133.7/120.4/120.3 MiB RSS and 6.4/4.0/3.0% CPU. Runtime log: zero bytes. The app exited through its normal Quit confirmation.
- No new Zynforge `.ips` crash report appeared; hashes of both existing production settings files were unchanged.
- The tested bundle was subsequently packaged and installed on both Macs; executable hashes and signatures match. Physical audio-device removal and console reconnect remain rig checks.
