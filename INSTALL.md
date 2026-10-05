# Local installation and rollback

This procedure installs a locally built macOS app; it does not create a notarized public release. Run commands from the repository root. Do not update software, drivers or firmware during a show.

## Current two-Mac installation and cleanup — 2026-10-05

Both Macs now have source build `443c769` and its matching protocol-4 capture
helper at `/Applications/Zynforge Recording.app`. The mini is currently at
`192.168.68.81`; its SSH host key matches its previously trusted address.
The development installation already matched the latest tested build exactly;
the mini was upgraded from `c563b00`. Later commits changed documentation only.

The GUI SHA-256 is `c78236ec6b14c3b723979c66171015bb13822d7b74b1e5977fb3a0abe74817ae`;
the helper SHA-256 is `ad6b74588229f9c3f23fb556d7f7a95b1a36a010189b4358de4f9c5ceef570f7`.
Both installed bundles match the tested bundle's files and pass deep/strict
signature verification; both executables contain arm64 and x86_64. Neither app
nor helper was running during replacement/cleanup. Mini settings hashes were
unchanged during installation. Recordings, sessions, source and other ZynForge
products were excluded from cleanup.

The current [universal DMG](dist/Zynforge-Recording-443c769-macOS-universal.dmg)
and [checksum](dist/Zynforge-Recording-443c769-macOS-universal.dmg.sha256) are in
`dist/` and the mini's `~/Downloads/`. DMG SHA-256:
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
found no superseded Recording packages remaining. **No old app rollback bundle
is retained.** Historical backup paths below are no longer available.

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
gates passed. See [the repair report](docs/REVIEW-REPAIRS-2026-10-05.md).

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

Source manifest (253 paths) SHA-256: `1d916b93ee5f3b2dc35f61f497615ee828fabce2d8b11d63cda9621efab98fdd`. The accompanying audit commit contains this source. See [the audit report](docs/AUDIT-2026-10-05.md) for defect/test mapping and reproduction commands.

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

The previous installs and packages (`b2c1991`, `3e6104c`, `8d79ab8`, `672456d`, `d2c5858`, `98a645e`, `5026519` and earlier) have been superseded. Their local app backups and installers were removed during the authorized cleanup. Historical validation and field results remain in [CHANGELOG.md](CHANGELOG.md), the dated audit records, [FIELD-TEST-2026-10-01.md](FIELD-TEST-2026-10-01.md), and Git history. A historical hash or test count is not evidence for the current executable.

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

## Rollback

The October 5 two-Mac cleanup removed all inventoried older app bundles at the user's request. There is currently no local old-version rollback copy. The verified current installer can reinstall `443c769`; a rollback to earlier code requires rebuilding and verifying that source revision. Historical backup names in this document are not usable recovery paths.

Stop capture and quit both processes first. Preserve current session data and the current app; then move the named backup back to `/Applications/Zynforge Recording.app`. Verify the restored signature before launch. Restore GUI and bundled daemon together, never just one executable. A prior build may not understand new session metadata: test with a duplicate session, not the only recording copy. Rolling back the app does not undo session-file changes or recover audio overwritten before the fixes.
