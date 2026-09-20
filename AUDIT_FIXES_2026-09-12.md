# Recording and session-integrity fixes

> Historical verification snapshot. For the current protocol-v3 build and the later reliability fixes, see [AUDIT_FIXES_2026-09-20.md](AUDIT_FIXES_2026-09-20.md).

Scope: the 32 findings in the September code audit. Validation below was completed before committing, pushing and installing the update.

## Changes

| Audit item | Resolution |
| --- | --- |
| 1. Backup/primary collision | Reject overlapping destinations before any primary writer opens; resolve filesystem aliases when checking roots. |
| 2. Mid-take record arms | Freeze capture participation for the take; reject UI/remote arm changes while recording. |
| 3. Import wipes edits | Snapshot and restore existing playlists around same-session import reload. |
| 4. Strip deletion misidentifies audio | Use a shared track-order operation; archive removed takes, reindex surviving audio and state together. |
| 5. Mixed mono/stereo moves | Move complete physical-channel blocks, preserving stereo adjacency. |
| 6. Unsafe file swaps | Journal staged moves, check every rename, roll back failures, recover interrupted operations on open. Never sweep away orphaned recording files. |
| 7. Save As/session switching during recording | Guard entry points and asynchronous session-replacement completion; require recording to stop first. |
| 8. Mirror-driven dangling UI references | Invalidate strips, edit rows and floating meters before remote track-count changes. |
| 9. Editing during bounce | Block menu dispatch during session operations; suspend autosave while files are being processed. |
| 10. Daemon overwrites earlier takes | Start subsequent captures as new continuation parts. |
| 11. Daemon ignores capture configuration | Transfer device state, sample rate, input routing, stereo layout, arm states, pre-roll and backup/mirror configuration; require acknowledgement. |
| 12. STOP reports success while daemon rolls | Await daemon STOP acknowledgement, update recording state and reload completed media. Transport readouts use daemon status. |
| 13. Stale-idle shutdown kill | Force-kill fallback requires an accepted Quit response, never cached idle status. |
| 14. Deleted clips return in exports | Distinguish authoritative empty arrangements from an uninitialised clip list. |
| 15. Edited audio extends past playback end | Recompute playback extent when clips change. |
| 16. Paste on unrecorded tracks is silent | Grow playback track storage and permit explicit media without a destination take file. Restore those tracks from playlist JSON. |
| 17. Clipboard loses media identity | Preserve source file, source channel and fade shape; refuse paste if copied media is missing. |
| 18. Missing media substitutes another take | Missing explicit readers render silence rather than falling back to the track's recording. |
| 19. Track state stays at old indices | Reorder UUIDs, groups, sends, automation and clip/take state; remap cue automation and referenced media. |
| 20. Reorder persists wrong keys | Persist through the actual zero-based settings stores. |
| 21. Cue UUIDs do not travel with sessions | Save and restore strip UUIDs in session_mix.json, including their application-settings representation. Rebind legacy cue snapshots by saved strip order where the old session has no portable strip UUID. |
| 22. Import uses device rather than session rate | Prefer the loaded session's sample rate for conversion. |
| 23. Deleting an earlier take changes active take | Adjust active index when removing a preceding take. |
| 24. Locked clips split/ripple | Respect locks in split, ripple and crop paths. |
| 25. Automation undo merges instead of restoring | Replace automation state when loading a snapshot, including empty snapshots. |
| 26. Wrong marker renamed | Resolve the inserted marker by runtime identity, including after asynchronous naming. |
| 27. Analysis commands swallowed | Move template-default IDs out of the analysis-command range. |
| 28. Format/multipart analysis failures | Resolve supported media and use concatenated take readers for normalization and zero-crossing lookup. Retain the intentional native-stereo contract: normalize once on L using both channels, not independently on R. |
| 29. Console gain snapshot overwritten | Accept gain replies into the snapshot only during an active capture request. |
| 30. Loop-boundary silence | Render successive loop segments within the same device callback. |
| 31. Session settings not restored | Load capture format, pre-roll, loop region and session sample rate when opening. |
| 32. Recovery shows zero tracks | Count takes in Audio Files, with legacy root-layout fallback. |

## Operational notes

- Deleting strips retains their recording files under `Removed Tracks` inside the session. Referenced archived audio remains usable by surviving clips.
- File-order journals and metadata backups remain under `Session File Backups/reorder_*`. Do not manually remove a pending journal when recovering a failed disk operation.
- Track-order changes clear the old index-based undo history and clip clipboard to prevent applying stale actions to different audio.
- Capture protocol version is now 2. Use matching GUI and daemon builds; stop existing takes before upgrading. Local installation must bundle the matching `ZynforgeCapture` executable in `Contents/MacOS` and retain the previous app as a rollback backup.
- This prevents future corruption; it does not reconstruct audio or edits already overwritten by an older build.

## Verification

Release build succeeded. The rebuilt application passed all 320 test groups with 0 failures (exit code 0). The application binary includes both arm64 and x86_64 architectures. `git diff --check` passed.

Test report: `/Users/jeanpierre/Library/Logs/Zynforge/test-report.log`.

## Delivery and remaining acceptance

Code commit `44a309e` was pushed to `origin/main`. The GUI and matching protocol-v2 daemon were installed in `/Applications/Zynforge Recording.app`; the installed copy matched the signed staging bundle and passed deep/strict signature verification. The previous app is retained at `/Applications/Zynforge Recording.app.backup-20260912-44a309e`. See [installation and rollback](INSTALL.md).

The planned SD5 / RME HDSPe AoX-D show is 56 inputs at 48 kHz for approximately two hours. Connection, computer/chassis and drives remain undecided. [Show readiness](SHOW-READINESS.md) defines the pending three-hour rehearsal and independent-backup requirement. The build is rehearsal-ready, not certified as the sole show recorder.

Regression coverage includes backup collisions, live-arm changes, empty arrangements, cross-track media, missing references, take selection, locks, automation snapshots, session UUIDs, reorder persistence, mixed stereo/mono moves, failed and interrupted file transactions, sample-continuous loops, console snapshots, daemon routing and repeat recording.

Physical audio devices, hardware consoles, long-duration recordings and actual power-loss/unplug events were not tested. File-transaction failure and interruption were exercised with temporary test sessions.
