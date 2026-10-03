# Local installation and rollback

This procedure installs a locally built macOS app; it does not create a notarized public release. Run commands from the repository root. Do not update software, drivers or firmware during a show.

## Current installation and package — 2026-10-04

Both the development Mac and Mac mini (`192.168.68.75`) now have source build `c563b00` at `/Applications/Zynforge Recording.app`, including the matching protocol-v3 `ZynforgeCapture` helper. Both executables are universal arm64 + x86_64. The build passed 438 test groups with zero failures, both audit gates and a 30-second isolated startup smoke before deployment.

The installed bundles pass deep/strict signature verification on both Macs, and their executable hashes match the tested build:

| Executable | SHA-256 |
| --- | --- |
| GUI | `be66a25be240d4c603a37c6795de6d8fe7f9d1bc5cd776ebab3086aae733c098` |
| Capture helper | `afbcb3cf1a741ef7101550e9cc6e82b39c90a4b9ca12108a1eace0a3e68ca1be` |

The development Mac reached Welcome / New Session with its saved device selections and remained running through a 51-second observation (about 116–117 MiB RSS). The mini launched twice; the user confirmed manually closing it both times. No new crash report appeared on either Mac. The mini was left closed. These startup checks do not replace disposable real-device capture or the outstanding exact-rig rehearsal.

The current [universal DMG](dist/Zynforge-Recording-c563b00-macOS-universal.dmg) and [checksum file](dist/Zynforge-Recording-c563b00-macOS-universal.dmg.sha256) are in `dist/` and the mini's `~/Downloads/`. DMG SHA-256: `d55720c4c291ef207e74b553333b60fd3db7154d43a264aaf9d0bf354bb7fdf5`. Image verification passed on both Macs; the read-only mounted app matched the tested build exactly. From the folder containing both files, run:

```bash
shasum -a 256 -c Zynforge-Recording-c563b00-macOS-universal.dmg.sha256
```

The app is ad-hoc signed, not Developer ID notarized, and supports macOS 12.0+.

At the user's request, after installation verification, old Recording app bundles, backup copies, installers and checksum files were deleted: 31 items on the development Mac (15 apps, 10 DMGs, 6 checksums), and 10 on the mini (3 apps, 4 DMGs, 3 checksums). Recordings, session backups, settings, source and other ZynForge products were preserved. Final scans of Applications and the user's Applications, Desktop, Downloads, Documents and Zynforge-App-Backups locations found no remaining old versions. The current development Release build remains available.

**Retention notice:** Older installation/package records elsewhere in the repository describe historical state. Their backup and installer paths are no longer available locally after cleanup; Git history is retained. The current installation above supersedes earlier paused-installation and retained-backup statements.

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

Capture protocol version 3 requires matching builds and a compatible Hello before commands are accepted. After a successful build/test run, first verify that the built bundle already contains and seals both executables, then stage it in a temporary directory:

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

No local old-version rollback copies remain after the requested 2026-10-04 cleanup. Obtain or rebuild the required revision before using the procedure below.

Stop capture and quit both processes first. Preserve current session data and the current app; then move the named backup back to `/Applications/Zynforge Recording.app`. Verify the restored signature before launch. Restore GUI and bundled daemon together, never just one executable. A prior build may not understand new session metadata: test with a duplicate session, not the only recording copy. Rolling back the app does not undo session-file changes or recover audio overwritten before the fixes.
