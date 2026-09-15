# Show readiness — planned SD5 recording

## Decision as of 2026-09-12

Rehearsal-ready; **not yet approved as the sole recorder for this show**. Commit `44a309e` fixes the 32 reported September issues and passed 320 automated test groups. The installed bundle was verified, but those results do not certify physical hardware, clock stability, disk endurance or uninterrupted show-length capture.

## Confirmed plan and open choices

| Item | Status |
| --- | --- |
| Console | DiGiCo SD5 |
| Inputs | 56 |
| Recording sample rate | 48 kHz |
| Expected show | Approximately 2 hours |
| Interface | RME HDSPe AoX-D |
| SD5-to-interface connection | Undecided: MADI or Dante path must be confirmed |
| Mac / PCIe or Thunderbolt chassis | Undecided |
| Primary, backup and independent recorder | Undecided |
| File format / bit depth | Proposed: 24-bit WAV; not yet selected by the user |
| Buffer and capture-daemon mode | Select and validate at rehearsal |

The AoX-D's specified capacity is 512 record/playback channels at 48 kHz; MADI needs optional expansion hardware. This establishes interface capacity, not end-to-end compatibility. [RME specifications](https://www.rme-usa.com/hdspe-aox-d.html). DiGiCo describes 56–64 channels per MADI stream at 48 kHz; verify the actual output format, channel mapping and clock source of the chosen path. [DiGiCo recording guide](https://digico.biz/wp-content/uploads/2020/02/TN296-Recording-with-Digico-Systems.pdf).

## Capacity planning

Calculated uncompressed audio payload for 56 mono inputs at 48,000 samples/s:

| Format | Write rate per copy | 2 hours | 3-hour rehearsal |
| --- | --- | --- | --- |
| 24-bit PCM | 8.064 MB/s | 58.06 GB | 87.09 GB |
| 32-bit float | 10.752 MB/s | 77.41 GB | 116.12 GB |

Decimal GB/MB, excluding headers and other session data. A simultaneous same-format backup doubles aggregate writes and needs the same capacity on its own drive. Plan at least 150 GB free per drive for a single 24-bit rehearsal or show; retaining both on the same drive needs more than their combined 145.15 GB payload plus comfortable headroom. Include soundcheck, pre-roll and overruns.

RF64 limits apply **per file**, not to the session total. At 24-bit/48 kHz one mono file reaches roughly 4 GiB after 8.3 hours (stereo about 4.1 hours). Fifty-six separate two-hour mono files do not each cross that threshold.

## Acceptance rehearsal — not yet performed

1. Record the exact Mac, macOS, RME driver/firmware, chassis, cabling, storage, app commit, capture mode and buffer setting. Avoid untested last-minute changes.
2. Confirm the intended clock source and stable synchronization throughout the chosen MADI/Dante path. Confirm the received audio really is 48 kHz. Do not assume the SD5's internal operating rate is the recording feed's rate.
3. Identify all 56 inputs individually with known signal. Check names, physical input assignments, stereo pairs if used, and saved session settings. Keep virtual-soundcheck returns from changing the live desk patch.
4. Run the app's pre-flight checks before recording. Verify primary and backup destinations are separate and writable. Disable computer sleep and protect power/cables. A 512-sample buffer is only a starting point for rehearsal when monitoring through the console; choose the setting that passes on this rig.
5. Record **three continuous hours** of signal on all 56 inputs with the actual backup/mirror configuration and capture mode. Monitor missed samples, write errors, device sync, free space and application responsiveness.
6. Stop normally; wait for the final report and hashing. Require zero missed samples and no primary/backup/mirror failures. Inspect every file's duration and channel mapping, audition beginning/middle/end, and open the recording in another DAW. Check the backup independently; a successful primary does not prove its backup.
7. Separately test repeated takes, session reopen, playback and export on disposable sessions. If using the daemon, test acknowledgement, routing and GUI reattachment. Run [FIELD-TEST.md](FIELD-TEST.md) and the applicable [hardware delta](FIELD-TEST-AUDIT.md).
8. Test device-loss and crash recovery only on disposable recordings, away from show time, with a hardware recovery plan. Stop testing if the interface becomes unstable.

Record actual results and reviewer acceptance; leave unperformed checks open. Any dropped samples, missing/wrong channels, unstable clock, short/corrupt files or failed redundancy is a no-go until diagnosed and the rehearsal passes again.

## Show-day redundancy

Use an independent recorder for an important show, especially before this validation is complete. A backup drive written by the same app protects against some storage failures, not failure of the app, computer, interface or shared power. The optional capture daemon improves process isolation but is not an independent recorder.

## Verification-helper limitations

`tools/verify_take.sh` is a legacy WAV/single-continuous-take diagnostic, not a show-readiness certificate. It currently treats intentional `_partXX` continuation files as errors, can return success while hashes are pending, and does not prove routing, equal expected durations or all backup copies. A crash survivor may lack a clean-stop report. Install its external prerequisites (`ffprobe`, `jq`, `xxd`, `shasum`, `python3`) explicitly; they are not all supplied by macOS. Use its output alongside manual checks, and retain both output and `session.report.json` with rehearsal evidence.
