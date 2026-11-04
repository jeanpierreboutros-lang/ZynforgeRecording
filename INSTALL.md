# Local installation and rollback

This procedure installs a locally built macOS app; it does not create a notarized public release. Run commands from the repository root. Do not update software, drivers or firmware during a show.

## Development Mac update — 2026-11-04, one waveform shape

The development Mac (MacBook Pro) now has `eca170e`: `577592c` plus the one-shape zoomed-out waveform (*Fixed — after the three-hour take* in [CHANGELOG.md](CHANGELOG.md)). The Mac mini still has `281c6c0`, and no DMG was made for this build.

| Executable | SHA-256 |
| --- | --- |
| GUI | `53baf442b3e1e44f369877fc552de4b319dec3140eda647a3cc1b81251d6c0a3` |
| Capture helper | `7ac2a265f8343adc8d169fe14eae2e4e4eb8ab6063dab647b5e9d8a5f06ae4fc` (unchanged) |

- **Tests before install:** 1588 groups / 0 failures in Release, launched through `open`; ASan/UBSan and TSan were not re-run for this display-only change. CI passed on the same commit. See [testing.md](testing.md).
- **Install:** after the owner answered the app's save prompt and it quit, neither the app nor the helper was running. The stage and the installed copy pass deep/strict verification and match (`diff -qr`). Both executables are universal.
- **Rollback:** `/Applications/Zynforge Recording.app.backup-20261104-104037-before-eca170e` (`577592c`).
- **Signing:** ad-hoc with the hardened runtime and the audio-input entitlement, as `577592c`.
- **After install:** it reopened the three-hour session from its saved waveform cache within about 20 s, with one waveform shape per lane.

## Previous development Mac update — 2026-11-03, after the first long take

The development Mac (MacBook Pro) had `577592c`: `5c7a3e4` plus the two post-STOP changes made after the first long take (*Fixed — after the first long take* in [CHANGELOG.md](CHANGELOG.md)). The Mac mini still has `281c6c0`, and no DMG was made for this build.

| Executable | SHA-256 |
| --- | --- |
| GUI | `b4aed815afc1e8a50d6dad137de136edd8e9d994c8c6b1adf7dda82019dee859` |
| Capture helper | `7ac2a265f8343adc8d169fe14eae2e4e4eb8ab6063dab647b5e9d8a5f06ae4fc` (unchanged) |

- **Tests before install:** 1588 groups / 0 failures in Release, ASan/UBSan and TSan, each launched through `open`, with no sanitizer reports. The built app launched with `--isolated-settings` and quit by itself after 35 s with no crash report. CI on the same commit was still running at install. See [testing.md](testing.md).
- **Install:** neither the app nor the helper was running. The stage and the installed copy pass deep/strict verification and match (`diff -qr`). Both executables are universal.
- **Rollback:** `/Applications/Zynforge Recording.app.backup-20261103-233742-before-577592c` (`5c7a3e4`).
- **Signing:** ad-hoc with the hardened runtime and the audio-input entitlement, as `d4a9a57` had; `5c7a3e4` had neither. Why builds differ was not looked into. macOS may ask for the microphone again.
- **After install:** it reopened the 4-hour session, and the drive still read about 3 GB/s for the app (see [testing.md](testing.md)). No take has been recorded on it.

## Previous development Mac update — 2026-11-03, JUCE 8.0.17-security

The development Mac (MacBook Pro) had `5c7a3e4`: the build below moved to JUCE 8.0.17-security, with one test, some comments and the docs changed since. The Mac mini still has `281c6c0`, described further down, and no DMG was made for this build.

| Executable | SHA-256 |
| --- | --- |
| GUI | `ed03def1d666b920930ca0d3355fe9b888869e080504ae4cdc5c29fd53cc6bc9` |
| Capture helper | `7ac2a265f8343adc8d169fe14eae2e4e4eb8ab6063dab647b5e9d8a5f06ae4fc` |

- **Tests before install:** 1586 groups / 0 failures in Release, ASan/UBSan and TSan at `5c7a3e4`, each launched through `open`, with no sanitizer reports. CI passed Debug and Release on the same commit. The built app launched with `--isolated-settings`, stayed near 109 MiB and 2% CPU, quit by itself after 35 s and left no crash report. See [testing.md](testing.md).
- **Install:** neither the app nor the helper was running. The stage and the installed copy pass deep/strict verification and match (`diff -qr`). Both executables are universal.
- **Rollback:** `/Applications/Zynforge Recording.app.backup-20261103-172758-before-5c7a3e4` (`9a40f54`, JUCE 8.0.4).
- **Signing:** ad-hoc, with no hardened runtime or entitlements, as `9a40f54` was. The smoke launch did not ask for the microphone; the installed copy may still ask once.
- **First launch:** settings carry over from `9a40f54`. If the interface was never picked in DEVICE and applied there, do it once: until then a local RECORD asks first, and a remote or twin RECORD is refused.
- **Not checked:** the installed app was not opened, no recording was made and no interface was tried. Nothing here has run on real hardware. The checks for this update are under *JUCE 8.0.17-security* in [FIELD-TEST.md](FIELD-TEST.md).

## Previous development Mac update — 2026-11-03, the fix round

The development Mac (MacBook Pro) had `9a40f54`: every fix of the round after `d4a9a57`, on top of the build below. The Mac mini still has `281c6c0`, described further down, and no DMG was made for this build.

| Executable | SHA-256 |
| --- | --- |
| GUI | `156cc71a275c57da50393b3de32b0ffada796e246cdcaf8e48b814636ac4c028` |
| Capture helper | `69818ba9957a04896a96dd51f7455faf0cd760662121600e034de5539941deff` |

- **Tests before install:** 1586 groups / 0 failures in Release, ASan/UBSan and TSan, each launched through `open`, with no sanitizer reports. The built app launched, quit by itself and left no crash report. See [testing.md](testing.md).
- **Install:** neither the app nor the helper was running. The stage and the installed copy pass deep/strict verification and match (`diff -qr`). Both executables are universal.
- **Rollback:** `/Applications/Zynforge Recording.app.backup-20261103-032331-before-9a40f54` (`d4a9a57`).
- **Signing:** ad-hoc, with no hardened runtime or entitlements, as `fffab44` was. `d4a9a57` had both; why builds differ was not looked into. macOS asks for the microphone again on this build.
- **First launch:** no device has been saved yet, because this is the first build that saves one. Pick the interface in DEVICE and press Apply once. Until then a local RECORD asks first, and a remote or twin RECORD is refused.
- **Not checked:** no recording was made and no interface was tried. Nothing here has run on real hardware.

## Previous development Mac update — 2026-11-02, design critique fixes

The development Mac (MacBook Pro) had `d4a9a57`, every fix from the design critique on top of the build below. The Mac mini kept `281c6c0`, described further down, and no DMG was made for this build.

| Executable | SHA-256 |
| --- | --- |
| GUI | `7b00d2c053a1854efca5734de1615a83cbf92fd0d3ff219471871c02aef7d505` |
| Capture helper | `f9e8c736c94965d5b3e8fcf2aae8cfdb76119204c3e309f670678eee53cd414f` |

- **Tests before install:** 1236 groups / 0 failures in Release, ASan/UBSan and TSan, each launched through `open`, with no sanitizer reports. CI passed Debug and Release on the same commit. The built app launched, quit by itself and left no crash report.
- **Install:** neither the app nor the helper was running. The stage and the installed copy pass deep/strict verification and match (`diff -qr`). Both executables are universal.
- **Rollback:** `/Applications/Zynforge Recording.app.backup-20261102-134207-before-d4a9a57` (`fffab44`).
- **Signing:** ad-hoc, with hardened runtime and `device.audio-input`. `fffab44` had neither, from the same build steps, and why they differ was not looked into. macOS asked for the microphone again, and it is allowed for this build.
- **Not in this build:** the title bar's ZynForge spelling came right after it. Nothing else in the app differs.
- **Seen running:** it opened, sees the audio device at 48 kHz and 512 samples, with no session loaded. No recording was made. Nothing here has run on real hardware.

## Previous development Mac update — 2026-11-02

The development Mac (MacBook Pro) had `fffab44`: the recorder and virtual soundcheck after the cut, every 2026-11-01 fix and the design pass. The Mac mini still has `281c6c0`, described below, and no DMG was made for this build.

| Executable | SHA-256 |
| --- | --- |
| GUI | `91f6ff578a748ce7323c4eb4842d7d85766bb613cdb5b4a582f4fde3fe8bbcba` |
| Capture helper | `6335a7af98b34227c6401ced532f16461e941b0dc60c657b957249bacde1b3a5` |

- **Tests before install:** 1219 groups / 0 failures in Release, ASan/UBSan and TSan, each launched through `open`, with no sanitizer reports. CI passed Debug and Release on the same commit. The built app launched, quit by itself and left no crash report.
- **Install:** neither the app nor the helper was running. The stage and the installed copy pass deep/strict verification and match (`diff -qr`). Both executables are universal. The stage was copied with `ditto --norsrc --noextattr --noqtn`, because the build folder is on exFAT.
- **Rollback:** `/Applications/Zynforge Recording.app.backup-20261102-094838-before-fffab44` (`281c6c0`).
- **Signing:** ad-hoc, with no entitlements and no hardened runtime, the same as `281c6c0`. Only a `tools/notarize.sh` package carries `device.audio-input`. macOS shows the microphone prompt again, since a new ad-hoc build counts as a new app.
- **Not in this build:** the fix that limits the crash notice to the recorder and its helper came right after it. `fffab44` can still count a crash of the tests app as the recorder's. Nothing else in the app differs.
- **Not checked:** no recording was made, and no audio device was selected. Nothing here has run on real hardware.

## Two-Mac installation — 2026-10-29

Everything in this section describes `281c6c0`, which still carries the features listed under *Removed* in [CHANGELOG.md](CHANGELOG.md). It is now on the Mac mini only.

Both Macs now have source build `281c6c0` (the October bug-hunt fixes, deferred
lead-ins and exact split fades; code identical to `abb83bf`) and its matching
protocol-4 capture helper at `/Applications/Zynforge Recording.app`.

| Executable | SHA-256 |
| --- | --- |
| GUI | `66f4389acee0b264c0b44505fbec66641cb629ca3814582d946d72649d8184e7` |
| Capture helper | `38ec8a06d845ddde62005d010cb2561b465ff929a18f83e78538a5ce7acd62ed` |

- **Tests before install:** a fresh Release run of the tests app reported
  689 groups / 0 failures. ASan/UBSan, TSan and CI passed the same 689 groups
  on `abb83bf`; see [testing.md](testing.md).
- **Development Mac (MacBook Pro, now `192.168.68.81`):** the built bundle and a
  signed stage passed deep/strict verification. The installed copy matches the
  stage (`diff -qr`) and passes verification, and both executables are
  universal (arm64 + x86_64). Rollback:
  `/Applications/Zynforge Recording.app.backup-20261029-220944-before-281c6c0`
  (`443c769`).
- **Mac mini (now `192.168.68.72`, `jeanpierre.local`):** its SSH host key
  matches the key previously trusted for it at `.75`/`.81`. It was installed
  from the DMG after checking the checksum and the mounted app's signature. The
  installed copy matches the DMG, passes verification and has the same hashes.
  Rollback:
  `/Applications/Zynforge Recording.app.backup-20261029-221655-before-281c6c0`
  (`443c769`). The mini had restarted about 15 minutes before installation.
- **DMG:** [universal DMG](dist/Zynforge-Recording-281c6c0-macOS-universal.dmg)
  and [checksum](dist/Zynforge-Recording-281c6c0-macOS-universal.dmg.sha256),
  also in the mini's `~/Downloads/`. DMG SHA-256: `232a28c8e8f4132dc89ac286bb0150204777b9aad88f158220ab40e47e535d9d`. `hdiutil verify`
  passes, and the mounted app matches the installed development bundle exactly
  and passes signature verification.
- **Processes during replacement:** no Recording GUI or capture helper was
  running on either Mac.
- **Startup on the development Mac:**
  - It was opened normally once and exited 10–20 s later with no crash report.
    The engineer was working in another app at the time; a manual close is the
    likely reason, but this was not confirmed.
  - The binary was then run directly. It reopened the last session and showed
    the Welcome window, and was still running 25 s later.
  - A quit request raised that session's save prompt, which was answered
    "Don't Save" (session files left exactly as on disk). It then exited with
    code 0.
- **Not checked:** the app was not launched on the mini. No recording was made
  on either Mac. Microphone permission, device selection and a disposable
  recording remain to be checked after this installation (see the October
  bug-hunt checks in [FIELD-TEST.md](FIELD-TEST.md)).

## Previous two-Mac installation and cleanup — 2026-10-05

Historical: this describes the state on 2026-10-05. At that installation both
Macs had source build `443c769` and its matching protocol-4 capture helper at
`/Applications/Zynforge Recording.app`. The mini was then at `192.168.68.81`; its
SSH host key matched its previously trusted address.
The development installation already matched the latest tested build exactly;
the mini was upgraded from `c563b00`. Later commits changed documentation only.

The GUI SHA-256 is `c78236ec6b14c3b723979c66171015bb13822d7b74b1e5977fb3a0abe74817ae`;
the helper SHA-256 is `ad6b74588229f9c3f23fb556d7f7a95b1a36a010189b4358de4f9c5ceef570f7`.
Both installed bundles match the tested bundle's files and pass deep/strict
signature verification; both executables contain arm64 and x86_64. Neither app
nor helper was running during replacement/cleanup. Mini settings hashes were
unchanged during installation. Recordings, sessions, source and other ZynForge
products were excluded from cleanup.

That installation's universal DMG `Zynforge-Recording-443c769-macOS-universal.dmg`
and its checksum were in `dist/` and the mini's `~/Downloads/`; they are no longer
in `dist/`. DMG SHA-256:
`01d61b56213a196fec40115ba886cc0729cb387f02f9807d1408e34f5d8fc2be`.
Image verification passed on both Macs; the read-only mounted app matched the
installed development bundle exactly and passed signature verification.

```bash
shasum -a 256 -c Zynforge-Recording-443c769-macOS-universal.dmg.sha256
```

After verification, the user's cleanup request removed 12 superseded items on
the development Mac (six old app/rollback bundles, three DMGs, three checksums)
and three on the mini (its prior app, old DMG and checksum). A final temporary-
directory sweep removed five additional old staging artifacts on the development
Mac (four app bundles and one ZIP); the mini had only current staging copies.
The current Release
and current sanitizer test builds remain available. Scans of Applications and
the user's Applications, Desktop, Downloads, Documents and Zynforge-App-Backups
found no superseded Recording packages remaining. No old app rollback bundle was
retained then; the bundles kept since 2026-10-29 are listed under *Rollback*.
Historical backup paths below are no longer available.

Both installed apps were launched with normal settings and remained running at
the 40-second observation, with no new crash reports. The mini's subsequent
sample was waiting inside CoreAudio device initialization; microphone consent
or device readiness still needs checking on its screen. This is not completed
mini startup acceptance or a physical recording test. No recording was started
by this installation. The mini's Xcode license was not changed; built-in `file`
inspection verified architectures because its developer-tool `lipo` was blocked.

Evidence is retained in
`../ZynforgeRecording-review-evidence-20261005/two-mac-installation/`.

## Earlier development Mac follow-up — 2026-10-05

At the user's request, source repair `443c769` is installed at
`/Applications/Zynforge Recording.app`, with its matching protocol-v4 capture
helper. This is the tested follow-up for all 21 reviewed findings: full Release,
ThreadSanitizer and ASan/UBSan/float-cast-overflow each passed 563 groups with
zero failures and no sanitizer reports. Six auto-stop tests and both static
gates passed. See [the repair report](docs/history/REVIEW-REPAIRS-2026-10-05.md).

| Executable | SHA-256 |
| --- | --- |
| GUI | `c78236ec6b14c3b723979c66171015bb13822d7b74b1e5977fb3a0abe74817ae` |
| Capture helper | `ad6b74588229f9c3f23fb556d7f7a95b1a36a010189b4358de4f9c5ceef570f7` |

Both installed executables match the tested Release bundle byte-for-byte, contain
arm64 and x86_64, and pass deep/strict signature verification. A complete staged
bundle was verified before replacement and compared again after installation.
No Recording GUI or capture helper was running at replacement. Production
preference hashes were unchanged by installation; recordings were untouched.

Rollback: `/Applications/Zynforge Recording.app.backup-20261005-112530-before-443c769`.
Earlier rollback bundles were not overwritten. The Mac mini and DMGs were not
updated. Physical 27-track capture/STOP acceptance remains pending.

The installed app was launched with normal preferences and left open. A
30-second observation produced no new crash report; the targeted process sample
showed main-component rendering and timer dispatch. During startup, CPU was
roughly 59–81% of one core and RSS roughly 522–525 MiB after the first five
seconds. This confirms startup, not interactive latency or physical recording
performance. No recording was started by the installer.

Installation hashes, rollback path, process sample and observation log are
retained in `../ZynforgeRecording-review-evidence-20261005/installation/`.


## Previous development Mac S6 audit repair — 2026-10-05

At the earlier S6 checkpoint, the development Mac had the audit repair at `/Applications/Zynforge Recording.app`, including its matching protocol-v4 helper. Both executables are universal arm64 + x86_64, match the tested Release bundle, and pass deep/strict signature verification. The full Release, ThreadSanitizer and AddressSanitizer/UndefinedBehaviorSanitizer/float-cast-overflow suites each passed 519 groups with zero failures; no sanitizer reports were emitted. Final isolated TERM/INT and disposable ENOSPC probes also passed.

| Executable | SHA-256 |
| --- | --- |
| GUI | `cf84bfe554d7916e60251d86e9f82d54b451d3a903da2c85d2b57b632e8faa98` |
| Capture helper | `015a3706a066238b57092fc683299657fe49b0e06c694ac83b943100739c5787` |

Source manifest (253 paths) SHA-256: `1d916b93ee5f3b2dc35f61f497615ee828fabce2d8b11d63cda9621efab98fdd`. The accompanying audit commit contains this source. See [the audit report](docs/history/AUDIT-2026-10-05.md) for defect/test mapping and reproduction commands.

Rollback: `/Applications/Zynforge Recording.app.backup-20261005-031802-before-audit`. Installation checked that no Recording GUI or capture helper was running, staged and verified the complete bundle, then replaced it with rollback on failure. Recordings and production preferences were preserved. The Mac mini and existing DMG remain unchanged.

**S6 startup observation was blocked on macOS microphone consent.** That local code signature caused TCC to request fresh consent; the isolated smoke launch waited in CoreAudio before creating its main window. Only that owned, non-recording smoke process was terminated after normal quit failed. No audio service or privacy setting was changed. At that checkpoint the app was left closed. The later `443c769` startup observation above reached rendering. A physical 27-track/27-insert long-take check remains required. Apple `leaks` reports the same small AppIntents connection cycles as the baseline; no leak-free claim is made.

## Previous development Mac responsiveness update — 2026-10-04

The previous development installation ran source build `55d62eb` at `/Applications/Zynforge Recording.app`, with its matching protocol-v3 helper. Both binaries are universal arm64 + x86_64, match the tested Release bundle byte-for-byte, and pass deep/strict signature verification. All 440 test groups pass. The affected 58-track session reopened and view switching/save/quit completed without a new crash report; repeat a fresh capture/STOP on the physical setup to confirm the full symptom is resolved.

| Executable | SHA-256 |
| --- | --- |
| GUI | `0a08743f6448d46068f087465721f55df5e6316b80517d6290cf6af649df9475` |
| Capture helper | `405d815d1005e224dc6294fdf2c5fa7e109de81c10620ed0c17feaf82e3b8384` |

Rollback copy: `/Applications/Zynforge Recording.app.backup-20261004-194817-before-responsiveness`. The Mac mini and existing DMG remain on `c563b00`; no new installer was packaged in this update. The installed development app was relaunched with normal preferences and left open on the stopped session.

## Earlier two-Mac installation and superseded DMG — 2026-10-04

At the earlier installation, both the development Mac and Mac mini (`192.168.68.75`) received source build `c563b00` at `/Applications/Zynforge Recording.app`, including the matching protocol-v3 `ZynforgeCapture` helper. Both executables are universal arm64 + x86_64. The build passed 438 test groups with zero failures, both audit gates and a 30-second isolated startup smoke before deployment.

The installed bundles pass deep/strict signature verification on both Macs, and their executable hashes match the tested build:

| Executable | SHA-256 |
| --- | --- |
| GUI | `be66a25be240d4c603a37c6795de6d8fe7f9d1bc5cd776ebab3086aae733c098` |
| Capture helper | `afbcb3cf1a741ef7101550e9cc6e82b39c90a4b9ca12108a1eace0a3e68ca1be` |

The development Mac reached Welcome / New Session with its saved device selections and remained running through a 51-second observation (about 116–117 MiB RSS). The mini launched twice; the user confirmed manually closing it both times. No new crash report appeared on either Mac. The mini was left closed. These startup checks do not replace disposable real-device capture or the outstanding exact-rig rehearsal.

The historical `Zynforge-Recording-c563b00-macOS-universal.dmg` and checksum were in `dist/` and the mini's `~/Downloads/`; both were removed during the October 5 cleanup. DMG SHA-256: `d55720c4c291ef207e74b553333b60fd3db7154d43a264aaf9d0bf354bb7fdf5`. Image verification passed on both Macs; the read-only mounted app matched the tested build exactly. From the folder containing both files, run:

```bash
shasum -a 256 -c Zynforge-Recording-c563b00-macOS-universal.dmg.sha256
```

The app is ad-hoc signed, not Developer ID notarized, and supports macOS 12.0+.

At the user's request, after installation verification, old Recording app bundles, backup copies, installers and checksum files were deleted: 31 items on the development Mac (15 apps, 10 DMGs, 6 checksums), and 10 on the mini (3 apps, 4 DMGs, 3 checksums). Recordings, session backups, settings, source and other ZynForge products were preserved. Final scans of Applications and the user's Applications, Desktop, Downloads, Documents and Zynforge-App-Backups locations found no remaining old versions. The current development Release build remains available.

**Retention notice:** Older installation/package records elsewhere in the repository describe historical state. Their backup and installer paths are no longer available locally after cleanup; Git history is retained. The current `443c769` section above supersedes earlier installation state. Later October 5 rollback bundles were also removed by the subsequent two-Mac cleanup above.

## Preconditions

1. Stop every take through the app, wait for completion, save the session and quit gracefully. Confirm the capture daemon is idle and has exited too. Never force-kill an unknown daemon: it may be recording after a GUI crash.
2. Build and run the tests described in [testing.md](testing.md). Check the fresh report and process exit status, not an old pass count.
3. For future updates, preserve the existing installed app at a unique backup path until replacement verification succeeds. Do not overwrite an earlier backup. The explicit 2026-10-04 cleanup request removed those verified old copies afterward.

## Earlier installations

The previous installs and packages (`b2c1991`, `3e6104c`, `8d79ab8`, `672456d`, `d2c5858`, `98a645e`, `5026519` and earlier) have been superseded. Their local app backups and installers were removed during the authorized cleanup. Historical validation and field results remain in [CHANGELOG.md](CHANGELOG.md), the dated audit records, [FIELD-TEST-2026-10-01.md](docs/history/FIELD-TEST-2026-10-01.md), and Git history. A historical hash or test count is not evidence for the current executable.

## Package both executables

The build produces the GUI bundle and a separate `ZynforgeCapture` artefact, then automatically embeds the matching helper and seals the local app after the copy. The installed layout requires:

```text
Zynforge Recording.app/Contents/MacOS/
  Zynforge Recording
  ZynforgeCapture
```

Capture protocol version 4 requires matching builds and a mutually authenticated Hello before commands are accepted. Version 3 peers must be upgraded as a GUI/helper pair. After a successful build/test run, first verify that the built bundle already contains and seals both executables, then stage it in a temporary directory:

```bash
install_stage=$(mktemp -d /private/tmp/zynforge-install.XXXXXX)
test -x "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app/Contents/MacOS/ZynforgeCapture"
codesign --verify --deep --strict "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app"
ditto "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app" "$install_stage/Zynforge Recording.app"
codesign --verify --deep --strict "$install_stage/Zynforge Recording.app"
```

Stop if any command fails. These are local ad-hoc signatures, not Developer ID notarization. Re-signing changes executable hashes; compare the installed copy with this signed stage, not the unsigned/original build output.

With both processes stopped, move the existing `/Applications/Zynforge Recording.app` to a unique, explicitly chosen backup name. Then:

```bash
ditto "$install_stage/Zynforge Recording.app" "/Applications/Zynforge Recording.app"
diff -qr "$install_stage/Zynforge Recording.app" "/Applications/Zynforge Recording.app"
codesign --verify --deep --strict "/Applications/Zynforge Recording.app"
open "/Applications/Zynforge Recording.app"
```

If copying or verification fails, do not launch the partial installation. Preserve it separately and restore the complete previous bundle. Test microphone permission, selected device, daemon startup and a disposable recording after installation. A verified signature alone is not an audio-path test.

## Developer ID build

Not run yet: it needs the owner's Developer ID. Local installs above stay ad-hoc signed.

`tools/notarize.sh` signs a private copy of the built app (the helper first, then the app), submits it, staples it and packages a DMG. The build tree keeps its ad-hoc seal. `--identity` takes the name or SHA-1 hash that `security find-identity -v -p codesigning` lists; `--profile` is a keychain profile made once with `xcrun notarytool store-credentials`.

```bash
app="build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app"
identity="Developer ID Application: <name> (<team id>)"
tools/notarize.sh --app "$app" --identity "$identity" --profile <profile> --check   # validates everything, changes nothing
tools/notarize.sh --app "$app" --identity "$identity" --profile <profile>           # sign, notarize, staple, DMG into build/release
```

## Rollback

The October 5 two-Mac cleanup removed every app bundle older than `443c769` at the user's request. Since then each install has kept the build it replaced:

| Mac | Build | Path in `/Applications/` |
| --- | --- | --- |
| Development | `443c769` | `Zynforge Recording.app.backup-20261029-220944-before-281c6c0` |
| Development | `281c6c0` | `Zynforge Recording.app.backup-20261102-094838-before-fffab44` |
| Development | `fffab44` | `Zynforge Recording.app.backup-20261102-134207-before-d4a9a57` |
| Development | `d4a9a57` | `Zynforge Recording.app.backup-20261103-032331-before-9a40f54` |
| Development | `9a40f54` | `Zynforge Recording.app.backup-20261103-172758-before-5c7a3e4` |
| Development | `5c7a3e4` | `Zynforge Recording.app.backup-20261103-233742-before-577592c` |
| Development | `577592c` | `Zynforge Recording.app.backup-20261104-104037-before-eca170e` |
| Mac mini | `443c769` | `Zynforge Recording.app.backup-20261029-221655-before-281c6c0` |

The development Mac's list was checked on 2026-11-03; the mini's is from its installation record. Earlier code requires rebuilding and verifying that source revision. Historical backup names in this document are not usable recovery paths.

Stop capture and quit both processes first. Preserve current session data and the current app; then move the named backup back to `/Applications/Zynforge Recording.app`. Verify the restored signature before launch. Restore GUI and bundled daemon together, never just one executable. A prior build may not understand new session metadata: test with a duplicate session, not the only recording copy. Rolling back the app does not undo session-file changes or recover audio overwritten before the fixes.
