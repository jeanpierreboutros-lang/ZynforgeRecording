# Local installation and rollback

This procedure installs a locally built macOS app; it does not create a notarized public release. Run commands from the repository root. Do not update software, drivers or firmware during a show.

## Preconditions

1. Stop every take through the app, wait for completion, save the session and quit gracefully. Confirm the capture daemon is idle and has exited too. Never force-kill an unknown daemon: it may be recording after a GUI crash.
2. Build and run the tests described in [testing.md](testing.md). Check the fresh report and process exit status, not an old pass count.
3. Preserve the existing installed app at a unique backup path. Do not overwrite an earlier backup.

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

## Installation record — 2026-09-12

- Code commit: `44a309e`, pushed to `origin/main`.
- Release: universal arm64 + x86_64; 320 test groups, zero failures.
- Installed: `/Applications/Zynforge Recording.app`, with matching daemon.
- Installed bundle matched the signed stage; deep/strict signature verification passed. Launch was requested successfully; full hardware smoke testing remains pending.
- Previous app: `/Applications/Zynforge Recording.app.backup-20260912-44a309e`.

## Installation record — 2026-09-15

- Application code: `df5ad36`, pushed to `origin/main`. The later documentation/CI-pin commit does not change the installed executable.
- Fresh macOS-12 universal Release: 350 test groups, zero failures on arm64 and x86_64; ASan+UBSan Debug: 350/0. [GitHub run 34986170067](https://github.com/jeanpierreboutros-lang/ZynforgeRecording/actions/runs/34986170067) passed clean Debug and Release builds/tests plus helper verification.
- Installed: `/Applications/Zynforge Recording.app`, with the matching protocol-v2 `ZynforgeCapture` helper. The installed bundle is byte-for-byte identical to the staged build; both executables are universal arm64+x86_64, require macOS 12.0, and pass deep/strict ad-hoc signature verification.
- The installed binary itself passed 350 test groups / zero failures and launched to the native 48 kHz / 24-bit New Session screen without a crash report.
- Previous app: `/Applications/Zynforge Recording.app.backup-20260915-before-df5ad36`.
- Xcode static analysis has no app-owned diagnostics; 27 invariants and the design audit are clean.
- Developer ID signing/notarization is not configured; Gatekeeper rejects this ad-hoc development bundle for distribution.

## Installation record — 2026-09-20

- Application code commit `fe5280d`, pushed to `origin/main` on 2026-09-21, contains all 16 fixes in [the recording-reliability follow-up](AUDIT_FIXES_2026-09-20.md). The following documentation-only delivery commit does not change the installed executables.
- Universal Release build: succeeded. In-app runner: 358 test groups, zero failures. Invariant audit, design audit and final diff checks: clean. [GitHub run 35538818050](https://github.com/jeanpierreboutros-lang/ZynforgeRecording/actions/runs/35538818050) passed Debug/Release builds and tests plus bundled-helper verification.
- Installed: `/Applications/Zynforge Recording.app`, with the matching protocol-v3 `ZynforgeCapture` helper. The built, staged and installed bundles pass deep/strict signature verification and the installed copy matches the stage.
- Previous installed app retained at `/Applications/Zynforge Recording.app.backup-20260920-before-protocol-v3` for rollback.
- Developer ID signing/notarization and the exact-rig hardware rehearsal remain pending.

## Installation record — 2026-09-23

- Application code commit `ccd755e` was built from `origin/main`. [GitHub run 35841647969](https://github.com/jeanpierreboutros-lang/ZynforgeRecording/actions/runs/35841647969) passed its Debug/Release matrix for that commit.
- The fresh universal Release build and the installed app each passed 371 test groups with zero failures. The GUI and bundled protocol-v3 `ZynforgeCapture` are both arm64+x86_64. Built, staged and installed bundles passed deep/strict signature verification; the installed bundle matched the signed stage exactly.
- Installed at `/Applications/Zynforge Recording.app`. The previous complete app remains at `/Applications/Zynforge Recording.app.backup-20260923-before-ccd755e` for rollback. The older 2026-09-20 backup was not changed.
- The installed app launched to an idle window with no new ZynForge `.ips` crash report (about 120 MB RSS / 3.7% CPU after 57 seconds). AppKit still logged transient negative-view-geometry faults; this native-UI follow-up remains open. No microphone, selected-device, capture-daemon, disposable-recording or exact-rig hardware acceptance test was performed during installation.
- This is a local ad-hoc-signed development install, not a notarized public release or show-readiness approval.

## Rollback

Stop capture and quit both processes first. Preserve current session data and the current app; then move the named backup back to `/Applications/Zynforge Recording.app`. Verify the restored signature before launch. Restore GUI and bundled daemon together, never just one executable. A prior build may not understand new session metadata: test with a duplicate session, not the only recording copy. Rolling back the app does not undo session-file changes or recover audio overwritten before the fixes.
