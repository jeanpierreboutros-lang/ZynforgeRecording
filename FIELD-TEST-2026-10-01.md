# Mac mini long-take evidence — 2026-10-01

## Prior-build recording

The user recorded 55 mono channels at 48 kHz / 24-bit for 10:42:37 on the Mac mini using the prior `672456d` installation and the primary drive only. During the long run, the live EDIT waveform became increasingly blocky. After a normal STOP, the file-backed waveform regained its usual shape. This observation isolates a live-preview problem; it does not by itself prove the underlying audio lost detail.

Before the session was moved or removed from the Mac mini's default sessions folder, read-only checks found 55 RF64 track files. Each was 5,552,247,568 bytes and contained 1,850,748,928 frames. The final `session.report.json` reported zero missed samples, no primary/device/hash failures, and completed SHA-256 values. Independent SHA-256 checks of all 55 files matched the report. Five small windows sampled from each channel were not all zero. These checks support the integrity of the primary files and their equal duration. They do not validate listening quality, intended input mapping, an independent backup, or every sample's musical content. The verified take was no longer in the default sessions folder at the later installation check; the app update did not remove it.

## Cause and change

`LivePeakHistory` reduced its bounded whole-take peak vector by pairs as a take grew. After roughly ten hours, one overview point represented about 1.36 seconds of audio, so the live display could only draw broad blocks at the recording edge. Stopping switched EDIT to a fresh file-backed thumbnail, which explains why the normal shape returned.

Source commit `d2c5858` retains a bounded ring of 65,536 original 256-sample peak bins alongside the reduced whole-take overview. At 48 kHz the ring covers about 5.8 minutes near the current head. EDIT asks for peaks by absolute recorder-bin range and uses the detailed ring where available. Older timeline areas still use the bounded overview. A ten-hour history and ring-wrap regression were added.

## Build and installation

The universal Release build passed 401 headless test groups with zero failures and the static invariant audit. The matching [DMG](dist/Zynforge-Recording-d2c5858-macOS-universal.dmg) passed SHA-256, `hdiutil verify`, read-only mount comparison, both-architecture and deep/strict ad-hoc signature checks. The Mac mini at `192.168.68.77` received the exact matching app and helper, with its previous installation retained at `/Applications/Zynforge Recording.app.backup-20261001-before-d2c5858`. The new app launched idle. The development Mac's Applications copy still uses `672456d`.

## Pending five-hour run

The user started a five-hour recording test on the updated Mac mini on 2026-10-01. Leave the host and session untouched during capture. After normal STOP and final hashing, record whether FOLLOW and zoom continued to work and the waveform near the live head retained detail, then verify the new session's report, frame counts, hashes and capture warnings. Until those observations are collected, automated tests and an idle launch are the validation of the new build; the prior ten-hour recording validates only the earlier build's primary media.
