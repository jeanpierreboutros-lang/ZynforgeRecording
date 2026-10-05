# Capture-integrity audit fixes — 2026-09-30

> Historical audit record: findings, counts and package paths below describe this dated pass. Current fixes and verification are in [the October 5 follow-up](docs/REVIEW-REPAIRS-2026-10-05.md) and [testing.md](testing.md). [INSTALL.md](INSTALL.md) distinguishes the development Mac, unchanged mini/DMG and retained rollback copies; older removed artifacts are not current install instructions.

This record describes source commit `672456d`, installed at `/Applications/Zynforge Recording.app` and packaged in the `dist/Zynforge-Recording-672456d-macOS-universal.dmg` (historical package; removed locally). It supplements the earlier [recording-reliability audit](AUDIT_FIXES_2026-09-20.md). Automated validation is not a real-device acceptance test.

## Corrected failures

| User-visible failure | Change |
| --- | --- |
| A repeated daemon START during a rolling continuation reset its base position and could clear a latched failure. | Reject START before `armContinue` or `startRecording` changes recorder state. A repeated command preserves the live position and warning. |
| The take verifier could pass a report with a failed primary, backup or punch operation. | Treat capture-failure flags, skipped/failed mirrors and failed hashing as verification failures. |
| A base file plus `_part03` could pass while `_part02` was missing and playback would refuse the take. | Check the physical part-number sequence as well as manifest coverage and hashes. |
| A report could claim more samples than the audio files contain and still pass a matching SHA-256. | Count decoded file frames and compare the sum to each track's reported primary total. |
| A rolled-back punch was missing from show-handoff review warnings. | Include `punchSpliceFailed` in the handoff's manual-review evidence. |
| A slow status subscriber could hold the daemon command lock across a socket write and delay STOP. | Build status under the lock, send outside it, and interrupt a stalled writer when the server stops. |
| The final background hash or report rewrite could fail without reaching the local app. | Record `sha256Failed`, retain the asynchronous failure state and show a local integrity warning after STOP. |
| The unattended stop helper could report success after a failed HTTP request or a single, unconfirmed STOP tap. | Complete the companion's two-tap STOP and require authenticated status to confirm `recording:false`; exit unsuccessfully otherwise. |
| The helper could select an old `Track_01.wav` from another session. | Pin the active session from authenticated status and use reported elapsed samples instead of a file timestamp. |
| A narrow main window could hide fixed recording controls. | Enforce a 1120 × 700 minimum window size. |

The continuation report also now enumerates and hashes all on-disk parts, including earlier passes and tracks not armed in the final pass. Earlier capture warnings and elapsed counters survive a clean continuation.

## Verification and limits

- Universal Release build succeeded. The in-app runner passed **399 test groups with zero failures**. Both static audits and `git diff --check` were clean.
- Disposable verifier fixtures passed valid fresh and continued WAV takes and real AIFF/FLAC files. Failed capture flags, a short file, a missing middle part and a failed punch were rejected.
- A mocked companion confirmed the unattended script sends the required STOP sequence and exits unsuccessfully when the companion is unreachable. No real take was stopped by this test.
- The installed bundle and read-only-mounted DMG matched byte for byte. Both GUI and helper contain `arm64` and `x86_64`, and the app passed deep/strict ad-hoc signature verification. The DMG checksum is in [INSTALL.md](INSTALL.md).
- A disposable long recording on each target Mac must still confirm waveform FOLLOW, real-device continuity, STOP behavior, backups and intended channel mapping. `verify_take.sh` checks primary files against their report; it cannot establish the planned show duration or certify external copies. Follow [FIELD-TEST.md](FIELD-TEST.md) and [SHOW-READINESS.md](SHOW-READINESS.md).
