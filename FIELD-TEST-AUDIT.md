# ZynForge Recording — Field-Test Checklist: Audit + Session Changes (2026-06-14)

Turnkey verification of the **hardware-gated audit items** and **everything changed since the 2026-05-24 build** (native stereo capture, console link, the compact/GRID mixer UI, prompt chrome, the design re-tone). Run on the real rig. The general first-launch/recording/takes flow lives in `FIELD-TEST.md` — this file is the delta.

**Severity:** 🟥 data-loss (stop + report now) · 🟧 audio-path (stop if reproducible) · 🟨 stage-readiness · ⬜ cosmetic.
**Turnkey helper:** after any take, `tools/verify_take.sh [session]` checks RF64/split/length/sha/missedSamples in one pass (exit 0 = green).

---

## A. RF64 single-file >4 GiB — the data-loss linchpin 🟥

Goal: one continuous `Track_NN.wav` past 4 GiB that opens full-length in a real DAW, and survives a hard kill.

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ A.1 | Record a take that crosses **4 GiB in one file** (≈8.3 h mono, or ~16 min × 32ch @ 24-bit/48k, or push channel count). | Single `Track_NN.wav` — **no** `Track_NN_partNN` split files. | Any `_part02` file appeared. |
| ☐ A.2 | Run `tools/verify_take.sh` on the session. | Exit 0; reports RF64 + `ds64` header on the >4 GiB file, `missedSamples: 0`, sha manifest match. | Non-zero exit; "crossed 4 GiB but NOT RF64". |
| ☐ A.3 | Open the >4 GiB `Track_NN.wav` in **Pro Tools / Reaper / Logic**. | Imports at full length, plays start-to-end, no truncation/garbage tail. | DAW reports corrupt header or short length. |
| ☐ A.4 | Start a fresh long take; **hard-kill** (`kill -9` / power) mid-take. Re-launch. | Recovery dialog lists the orphan; recovered file opens to ~the last 5-s header flush. | File opens to 0 length / won't open. |
| ☐ A.5 | After A.4 recovery, `verify_take.sh` again. | Green (length ≈ pre-kill, header valid). | — |

> ⚠️ Do **not** force-kill with the HDSPe as the live device unless you can power-cycle it — a hard kill can wedge the CoreAudio device (use a throwaway device for A.4 if possible).

---

## B. Throughput soak — does the disk keep up at scale 🟥

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ B.1 | Arm **64 ch @ 24-bit/48k**, record 20+ min of real signal to the gig SSD. | `missedSamples: 0` in the dashboard + `session.report.json`. "DISK STRUGGLING" never shows. | Any missed samples; struggling warning. |
| ☐ B.2 | Push to **96–128 ch @ 96k** if the interface allows. | Same — 0 missed, ring-fill stays well under 80%. | Ring fill pegs / missed samples climb. |
| ☐ B.3 | Enable a **second mirror** (backup drive). Record. | Both primary + mirror files present, identical length; `verify_take.sh` green on both roots. | Mirror short / missing; `anyMirrorFailed`. |
| ☐ B.4 | (Optional) **Capture daemon ON** (Session ▸ Recording & Sync ▸ Capture daemon), repeat B.1. | Take rolls out-of-process; a forced UI quit mid-take leaves the file intact + rolling. | UI quit stops the take / corrupts the file. |

---

## C. Console link on a real X32 / M32 🟧

Verifies the OSC routing/gain paths + the **new reply-timeout watchdog** (this session). Profile must be X32.

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ C.1 | Session ▸ **Console ▸ connect…**, enter the desk IP. | "Console: connect" flips to "disconnect <host>"; SOUNDCHECK/gain items light up. | Never connects; items stay greyed. |
| ☐ C.2 | **Console: SOUNDCHECK patch (card returns)**. | Status: "repatched to SOUNDCHECK; show patch stashed." Desk inputs now read the card. | Stuck at "Reading console patch…". |
| ☐ C.3 | **Pull the network cable for ~2 s** during C.2's query, re-seat. | Within ~1.5 s: "Console patch read timed out (lost reply) — try again." (no permanent hang). | Sits forever at "Reading console patch…". |
| ☐ C.4 | **Console: back to STAGE patch**. | Desk inputs restored to the stashed show patch exactly. | Wrong/partial restore. |
| ☐ C.5 | **Console: capture head-amp gains**, then change a gain on the desk, then **restore**. | Captured count reported; restore writes the captured values back (gain returns). | Gains not captured / not restored; mapping off (note: assumes head-amp N = strip N — verify AES50 channels). |
| ☐ C.6 | **Trim-Follow** ON, nudge a desk input gain during virtual soundcheck playback. | The recorded track's level shifts with the live preamp move. | No follow / wrong channel. |

---

## D. Native stereo capture — record-as-one-file 🟧

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ D.1 | Mark a strip **stereo** (mixer link or PATCH), arm + record real stereo. | **One** interleaved 2-ch `Track_NN.wav` — no second mono file. Pair hard-panned L/R. | Two mono files; pair centred. |
| ☐ D.2 | Virtual-soundcheck playback of D.1. | Stereo image correct (L→its out, R→its out); the pair meters as one stereo strip. | Plays mono / wrong outputs / no meter. |
| ☐ D.3 | Bounce the stereo pair (Export). | One interleaved stereo stem, both channels present, opens in the DAW. | R channel silent. |
| ☐ D.4 | **Routing-mapping regression:** create 2 empty mono strips (don't record), then **import** a stereo file. | Stereo plays from **its own strip** + meters; the empty mono strips stay silent; muting them does nothing to the stereo. | Stereo plays out of the mono strips / no meter (the bug fixed `3b0b857`). |
| ☐ D.5 | Open a **legacy** session (two-mono-file stereo pair). | Plays/edits/exports exactly as before. | Anything broken for old sessions. |

---

## E. Mixer UI — compact strips, GRID, presets 🟨

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ E.1 | Mixer with ~24 strips, click **M** preset. | ~12 fit per page without horizontal scroll; dB ruler + fader + meter hug each other (no wide gutter). | Strips too wide / few fit. |
| ☐ E.2 | Click **GRID**. | 12 per row, **2 rows = 24 faders** on one page; scrolls **vertically** for more. | Single row / no second row. |
| ☐ E.3 | Click **L**. | 8 per page, each strip shows the **full channel name on its own row** (long names not truncated). | Name truncated on L. |
| ☐ E.4 | Rename a channel at **M** width (double-click name). | Editor opens; short names fit; long names truncate in display but full name on hover + editable. | Rename field unusable. |
| ☐ E.5 | A **stereo** strip in the compact view. | Renders cleanly with a 2-bar meter; not clipped. | Overlap / clipped meter. |
| ☐ E.6 | EDIT view: **double-click** empty area below last row. | Adds one track. Single click does **not**. | Single click adds a track. |
| ☐ E.7 | EDIT view: the clip-gain cap (bottom-left of a clip). | A clear machined cap; easy to grab + drag; GAIN ±dB pill updates. | Tiny/invisible grip. |

---

## F. Faders, prompts, identity 🟨 / ⬜

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ F.1 | **Scroll-wheel / trackpad over a fader** (channel, master, VCA, bus). | Fader does **not** move; the mixer scrolls instead. Only click+drag on the cap moves it. | Wheel changes the level. |
| ☐ F.2 | Trigger **Delete channel?** (select strip → Track ▸ Delete, or strip menu). | Dialog shows the **ZynForge forge-mark** badge (not a red warning triangle); grey chrome. | Warning triangle / blue chrome. |
| ☐ F.3 | Dock / Finder **app icon**. | New forge-mark + "RECORDING" icon (may need a Dock relaunch for cache). | Old forged-Z. |
| ☐ F.4 | Launch **splash**. | New forge-mark glyph (not the old Z). | Old Z. |
| ☐ F.5 | **Design re-tone:** look at a strip at rest vs **record-armed**. | At rest the orange spine/seam are subdued ember; when **armed** the spine goes bright + glows. A hot meter reads as the brightest orange. | Resting chrome as bright as a hot meter (can't tell armed from idle at a glance). |
| ☐ F.6 | Every other **prompt** (Session menu items, errors). | First-party grey chrome; no JUCE default blue/triangle. | Stock JUCE alert. |

---

## G. Live dashboard + pre-flight on real hardware 🟨

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ G.1 | Session ▸ **Show pre-flight checklist…** with the real interface. | Device, sample rate, free space, armed-track count all correct. | Wrong device / missing checks. |
| ☐ G.2 | Record a 10–15 s armed take; watch the **PerfDashboard**. | Enlarges with a red border; shows `48.0 kHz · <buffer> smp` + live `AUDIO %`. | No grow / no AUDIO row. |
| ☐ G.3 | Reopen the session; confirm the **bound CoreAudio device**. | Binds to the intended interface (RME HDSPe), not a stray Avid/proxy device. | Silently binds to wrong device (`HALC_ProxyIOContext … overload`). |
| ☐ G.4 | Session ▸ **Analyse for noise** with recorded audio. | Sortable report (hum/bumps/noise floor) + `noise_report.json`. On an empty session: a clear "nothing to analyse" prompt. | Does nothing silently. |

---

## Reporting

For any 🟥/🟧: note **gesture, build commit (`git rev-parse --short HEAD`), device, channel count, sample rate**, and attach `session.report.json` + the `verify_take.sh` output. Drop it back here and we triage.

When A–D pass on the rig, the **trust gap from the audit is closed** and this is 1.0-ready for live use.
