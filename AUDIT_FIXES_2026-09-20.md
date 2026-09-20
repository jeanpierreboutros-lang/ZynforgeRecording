# Recording reliability fixes — 2026-09-20

## Outcome

The whole-codebase follow-up audit found 16 user-facing failures: 2 critical, 11 high, and 3 medium. All 16 are fixed in the current working tree. A macOS universal Release build succeeds, the in-app runner passes 358 test groups with zero failures, and both the invariant and design audits are clean.

This is software validation, not acceptance of the planned SD5 / RME / 56-input show rig. The exact interface, clock, storage, daemon mode, backup/mirror drives, and a continuous three-hour rehearsal still require physical verification. Keep an independent recorder for important shows.

## Critical fixes

| Finding | Resolution |
| --- | --- |
| Starting a fresh take in an unreadable session could overwrite existing `Track_NN` media. | Record start now scans every supported container before opening writers. Existing media forces explicit continue/punch behavior; unreadable sessions are reported as protected rather than treated as empty. |
| Daemon writer/finalization failures could be hidden and STOP could report success while capture was unhealthy. | Primary, backup, mirror, recovery-marker, report-write, and disk-health failures cross the protocol boundary. STOP distinguishes “stopped with finalization errors” from “may still be recording” and the UI only clears external recording state after acknowledged completion. |

## High fixes

| Finding | Resolution |
| --- | --- |
| Backup or mirror destinations that failed to open were still presented as active. | Destinations are created and tested before capture, skipped copies are counted, and only successfully opened writers are active. |
| Backup/mirror write failures during a take were lost when writers were cleared. | Failure state is latched for the take and remains visible after writer teardown. |
| The selected backup path disappeared after relaunch. | The backup directory now persists in application settings and is restored before audio-device availability matters. |
| StereoMix could record silence when no physical stream output was assigned. | File capture now builds the stream mix independently of physical stream outputs and records live routed inputs. A local StereoMix arm with no stream sends, or daemon StereoMix where unsupported, fails loudly before rolling. |
| Remote/MCU playback could start over a daemon-owned take. | The engine-level playback guard uses local-or-external `isRecording()`, covering UI, OSC, Companion, MCU, and timecode callers. |
| CaptureLink published a `shared_ptr` across threads without synchronization. | Connection state now uses atomic shared-pointer load/store. |
| Strip Silence analyzed the raw file rather than the edited arrangement and mishandled stereo, multipart, cross-track, and all-silent material. | It now analyzes the rendered arrangement with clip gain/fades/source channels, supports multipart and cross-track clips, preserves locked clips, and makes an all-silent selection explicitly empty. |
| Mixer output mute and stream-send state did not persist, reset, or participate in undo. | Both fields round-trip in `session_mix.json`, reset between sessions, and are included in mixer undo snapshots. |
| Mirror disk-time estimates could be wrong, especially when destinations shared a volume. | Byte rates are aggregated per physical volume; healthy independent volumes are evaluated independently and failed/skipped copies are excluded. |
| A failed daemon reconfiguration could leave its audio callback detached. | Failed configuration restores the callback; successful configuration attaches it exactly once. |
| Analysis and destructive edit work could block the message thread, including while a daemon take was active. | Transient detection, Strip Silence, Normalize, and Consolidate run on background workers, reject stale results, and are disabled during local/daemon recording or another session operation. |

## Medium fixes

| Finding | Resolution |
| --- | --- |
| Capture commands were accepted before a compatible protocol handshake. | Protocol v3 requires a successful Hello before commands are delivered; missing or mismatched handshakes are rejected and disconnected. |
| Autosave failures could be silent, delay the next retry, or advance the clean marker after a partial save. | A save is clean only after all live metadata and the backup snapshot succeed. Failures warn immediately and retry after 15 seconds without advancing the saved undo baseline. |
| Consolidate could overwrite an existing `_999` output. | Output numbering is unbounded and temporary renders are installed only into a free final filename. |

## Verification

- Universal Release build: succeeded.
- In-app test runner: 358 groups, 0 failures.
- `tools/invariants_audit.sh`: clean.
- `tools/design_audit.sh`: clean.
- `git diff --check`: clean.
- [GitHub run 35538818050](https://github.com/jeanpierreboutros-lang/ZynforgeRecording/actions/runs/35538818050): clean Debug/Release builds and tests, both audits, bundled-helper verification, and report upload passed.

New regression coverage includes cross-container collision protection, explicit continuation, unavailable backup reporting, backup persistence, arrangement-aware Strip Silence, all-silent deletion, external-record playback refusal, `_1000` consolidation, StereoMix capture without hardware stream outputs, protocol-v3 completion state, and pre-Hello command rejection.

Application code commit `fe5280d` and its documentation-only delivery follow-up were pushed to `origin/main` on 2026-09-21. The installed bundle was built from the application-code commit; the follow-up changes documentation only.

## Manual acceptance still required

1. Record to the planned primary, backup, and mirror volumes; unplug one redundant destination mid-take and verify the warning, surviving files, report, and time-remaining display.
2. Exercise daemon start, failed reconfiguration, STOP, and Stop & Quit on the real interface.
3. Capture StereoMix locally with stream sends but no hardware stream outputs and confirm the file is audible.
4. Reopen an unreadable/corrupt session containing existing take files and verify RECORD cannot replace them.
5. Run Strip Silence, Normalize, Consolidate, and transient navigation on a long edited stereo/multipart session and confirm the UI remains responsive.
6. Complete the three-hour, 56-input rehearsal in [SHOW-READINESS.md](SHOW-READINESS.md).
