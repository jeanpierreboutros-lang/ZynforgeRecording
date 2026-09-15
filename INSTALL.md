# Local installation and rollback

This procedure installs a locally built macOS app; it does not create a notarized public release. Run commands from the repository root. Do not update software, drivers or firmware during a show.

## Preconditions

1. Stop every take through the app, wait for completion, save the session and quit gracefully. Confirm the capture daemon is idle and has exited too. Never force-kill an unknown daemon: it may be recording after a GUI crash.
2. Build and run the tests described in [testing.md](testing.md). Check the fresh report and process exit status, not an old pass count.
3. Preserve the existing installed app at a unique backup path. Do not overwrite an earlier backup.

## Package both executables

The build produces the GUI bundle and a separate `build/ZynforgeCapture_artefacts/Release/ZynforgeCapture`. `CaptureSupervisor` can discover that sibling in the build tree. The installed layout instead requires:

```text
Zynforge Recording.app/Contents/MacOS/
  Zynforge Recording
  ZynforgeCapture
```

Capture protocol version 2 requires matching builds. After a successful build/test run, stage the bundle in a temporary directory:

```bash
install_stage=$(mktemp -d /private/tmp/zynforge-install.XXXXXX)
ditto "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app" "$install_stage/Zynforge Recording.app"
ditto "build/ZynforgeCapture_artefacts/Release/ZynforgeCapture" "$install_stage/Zynforge Recording.app/Contents/MacOS/ZynforgeCapture"
codesign --force --sign - "$install_stage/Zynforge Recording.app/Contents/MacOS/ZynforgeCapture"
codesign --force --sign - --preserve-metadata=entitlements "$install_stage/Zynforge Recording.app"
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

## Installation record — 2026-09-12

- Code commit: `44a309e`, pushed to `origin/main`.
- Release: universal arm64 + x86_64; 320 test groups, zero failures.
- Installed: `/Applications/Zynforge Recording.app`, with matching daemon.
- Installed bundle matched the signed stage; deep/strict signature verification passed. Launch was requested successfully; full hardware smoke testing remains pending.
- Previous app: `/Applications/Zynforge Recording.app.backup-20260912-44a309e`.

## Rollback

Stop capture and quit both processes first. Preserve current session data and the current app; then move the named backup back to `/Applications/Zynforge Recording.app`. Verify the restored signature before launch. Restore GUI and bundled daemon together, never just one executable. A prior build may not understand new session metadata: test with a duplicate session, not the only recording copy. Rolling back the app does not undo session-file changes or recover audio overwritten before the fixes.
