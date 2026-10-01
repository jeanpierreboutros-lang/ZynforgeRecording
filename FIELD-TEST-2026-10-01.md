# Mac mini long-take evidence — 2026-10-01

## Prior-build recording

The user recorded 55 mono channels at 48 kHz / 24-bit for 10:42:37 on the Mac mini using the prior `672456d` installation and the primary drive only. During the long run, the live EDIT waveform became increasingly blocky. After a normal STOP, the file-backed waveform regained its usual shape. This observation isolates a live-preview problem; it does not by itself prove the underlying audio lost detail.

Before the session was moved or removed from the Mac mini's default sessions folder, read-only checks found 55 RF64 track files. Each was 5,552,247,568 bytes and contained 1,850,748,928 frames. The final `session.report.json` reported zero missed samples, no primary/device/hash failures, and completed SHA-256 values. Independent SHA-256 checks of all 55 files matched the report. Five small windows sampled from each channel were not all zero. These checks support the integrity of the primary files and their equal duration. They do not validate listening quality, intended input mapping, an independent backup, or every sample's musical content. The verified take was no longer in the default sessions folder at the later installation check; the app update did not remove it.

## Cause and change

`LivePeakHistory` reduced its bounded whole-take peak vector by pairs as a take grew. After roughly ten hours, one overview point represented about 1.36 seconds of audio, so the live display could only draw broad blocks at the recording edge. Stopping switched EDIT to a fresh file-backed thumbnail, which explains why the normal shape returned.

Source commit `d2c5858` retains a bounded ring of 65,536 original 256-sample peak bins alongside the reduced whole-take overview. At 48 kHz the ring covers about 5.8 minutes near the current head. EDIT asks for peaks by absolute recorder-bin range and uses the detailed ring where available. Older timeline areas still use the bounded overview. A ten-hour history and ring-wrap regression were added.

## Build and installation

The universal Release build passed 401 headless test groups with zero failures and the static invariant audit. The matching [DMG](dist/Zynforge-Recording-d2c5858-macOS-universal.dmg) passed SHA-256, `hdiutil verify`, read-only mount comparison, both-architecture and deep/strict ad-hoc signature checks. The Mac mini at `192.168.68.77` received the exact matching app and helper, with its previous installation retained at `/Applications/Zynforge Recording.app.backup-20261001-before-d2c5858`. The new app launched idle. The development Mac's Applications copy still uses `672456d`.

## Interrupted five-hour run on the updated build

The user began a new 55-track, 48 kHz session, `TEST 5 HOURS NEW`, at 19:13:45 local time on 2026-10-01. Read-only checks during capture found a `recording.session` marker and 55 equal-sized files growing at the expected aggregate rate. At 21:57 the Mac mini restarted unexpectedly. The restarted app opened; the marker was absent at the later read-only check. **This was not a completed five-hour test or a clean STOP.** Preserve the session's originals.

All 55 mono 24-bit WAV files remained present and had the same size, 1,407,486,736 bytes each (77,411,770,480 bytes total). `afinfo` opened all 55 headers; a sampled file reported 9,771.52 seconds (2:42:51.52) of readable audio. Its physical payload extends 2.688 seconds beyond the last flushed `data` header, so those trailing bytes are not counted by ordinary WAV readers without a separate, carefully validated recovery copy. The last file modification was around 21:56:39, about 45 seconds before the reported panic. There is no `session.report.json`, final hash or clean-stop capture warning summary. These checks establish readable headers and equal file lengths, not uninterrupted audio quality, channel mapping or missing-write status.

The 21:57:24 macOS panic report names `apcie[2:lan-1gb]::handleCompletionTimeoutInterrupt` in Apple's PCIe code. Reports from 16:22 the same day and 13:56 the previous day show the same reason. The panicked task was `kernel_task`; no Zynforge defect was identified in this interruption. The reports do not establish which Mac hardware, macOS component or connected device caused the timeout. The user attributes the interrupted test to the Mac mini and does not want app or audio-routing changes in response. Do not interpret the aborted run as evidence that the new waveform fix failed or passed. A completed live waveform/FOLLOW observation and clean-stop five-hour capture remain unverified; defer those checks until the Mac mini is stable.

After reboot, the active `en0` interface was the Mac mini's built-in 1 Gb Ethernet port. DVS 4.5.2, RME AoX Settings 1.3.0, Zynforge Recording, ZynForge Live and Dante Controller were installed or running. The user confirmed that the interrupted Zynforge take used DVS for input and the Samsung monitor for output, with the RME AoX-D also in the setup. This describes the test configuration; it does not identify DVS, the RME interface, the monitor or concurrent applications as the cause. Their post-reboot state does not prove which software was active at the instant of the panic. The same panic on 2026-09-30 predates the `d2c5858` waveform installation. No changes to the app, device selection or routing are planned from this incident.
