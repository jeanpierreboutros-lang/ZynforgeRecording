# Local installation and rollback

This procedure installs a locally built macOS app; it does not create a notarized public release. Run commands from the repository root. Do not update software, drivers or firmware during a show.

## Preconditions

1. Stop every take through the app, wait for completion, save the session and quit gracefully. Confirm the capture daemon is idle and has exited too. Never force-kill an unknown daemon: it may be recording after a GUI crash.
2. Build and run the tests described in [testing.md](testing.md). Check the fresh report and process exit status, not an old pass count.
3. Preserve the existing installed app at a unique backup path. Do not overwrite an earlier backup.

## Current two-Mac test DMG — 2026-09-24

[`dist/Zynforge-Recording-3e6104c-macOS-universal.dmg`](dist/Zynforge-Recording-3e6104c-macOS-universal.dmg) packages the tested installed build from source commit `3e6104c`. It runs on macOS 12.0+ on Apple Silicon or Intel and includes the matching protocol-v3 `ZynforgeCapture` helper, an Applications shortcut and a `READ ME.txt`. SHA-256: `3acfe9d93165b4f59d80089f5bcd214fbd19924c2d8be12a6ef5c12fb70c432f`. A matching [checksum file](dist/Zynforge-Recording-3e6104c-macOS-universal.dmg.sha256) is provided for transfer verification.

From the `dist` directory, run `shasum -a 256 -c Zynforge-Recording-3e6104c-macOS-universal.dmg.sha256` on each Mac after copying both files. Stop and save any take, quit the app and capture helper, and back up the existing app. Open the DMG and drag `Zynforge Recording.app` to the Applications shortcut. After replacement, launch the app from Applications and make a disposable recording and playback check with the selected device and backup/mirror paths.

The image checksum verified. A read-only mount contained the expected shortcut, instructions and app; that app matched the tested installed bundle byte for byte and passed deep/strict ad-hoc signature verification. The GUI and helper are both `arm64` + `x86_64`. The installed bundle passed 397 test groups with zero failures before packaging. The DMG is **not Developer ID signed or notarized**. If macOS blocks the transferred app, use Finder's Control-click > Open or System Settings > Privacy & Security > Open Anyway for this app; do not disable system-wide security settings. Real-device capture and long-take navigation still need testing on each target Mac.

## Earlier local DMG — 2026-09-24

`dist/Zynforge-Recording-b2c1991-macOS-universal.dmg` packages source commit `b2c1991` for macOS 12.0+ on Apple Silicon and Intel. It includes the matching protocol-v3 `ZynforgeCapture` helper. The previous `dist/Zynforge-Recording-0.2.0-macOS-universal.dmg` is a separate older artifact and was not replaced.

SHA-256: `a4c926dd59d74a0f8cba9d20c44781e47db976758301784fdb5328b38af9f778`.

That older source Release build passed 387 test groups with zero failures and [GitHub run 36017363562](https://github.com/jeanpierreboutros-lang/ZynforgeRecording/actions/runs/36017363562). The DMG checksum verified; after mounting read-only, the app matched the built bundle byte-for-byte, both executables were `arm64` + `x86_64`, and deep/strict code-sign verification passed. These are packaging checks, **not** a real-device recording test. The DMG was not used for the later installation below and does not contain the long-take or codebase-audit fixes.

To install from this DMG on a Mac: stop every take, wait for report finalization, quit the GUI and capture helper, and copy the existing `/Applications/Zynforge Recording.app` to a uniquely named rollback location. Open the DMG, then drag `Zynforge Recording.app` onto the `Applications` shortcut. Verify the new app and helper before deleting any backup. The DMG and app are ad-hoc signed, **not Developer ID signed or notarized**; macOS Gatekeeper may block a transferred/downloaded copy, and this is not a public distribution or show-readiness approval. Do not disable system-wide security settings to install it. After installation, use a disposable session to test the selected audio device, permissions, capture, punch, backup/mirror and playback before using it for important material.

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

## Installation record — 2026-09-24, long-take EDIT fix

- Installed the local universal Release bundle built from the current working tree based on `b2c1991`, including the long-take EDIT fix and matching protocol-v3 `ZynforgeCapture`. The change is not committed and is not in either existing DMG.
- Confirmed both Zynforge processes were stopped before replacement. Preserved the former installed app at `/Applications/Zynforge Recording.app.backup-20260924-215108-before-live-nav`; its deep/strict signature still verifies.
- The built, staged and installed bundles matched byte-for-byte. Both installed executables are `x86_64` + `arm64`; the installed bundle passed deep/strict ad-hoc signature verification. GUI SHA-256: `3783001b14675985e79ea7f2215f5f638076892853dfb8bbaa52cc3158d693f9`; helper SHA-256: `e8a5929e7ae4bc619315ebc1ddff7633035031ecb58dcecb9922fd5d7aa6fefc`.
- The installed app passed 391 test groups with zero failures. The first ordinary launch ended without a new crash report or an established cause; a second fresh launch remained running idle for over one minute with no capture helper active and no new ZynForge crash report.
- A disposable real-device capture, rolling EDIT navigation beyond 45 minutes/three hours, punch, playback and backup/mirror check remains required before relying on the build for important material. This is not a notarized public release or exact-rig show acceptance.

## Installation record — 2026-09-24, codebase-audit fixes

- Installed the universal Release bundle with the long-take EDIT and thirteen audit fixes plus its matching protocol-v3 helper. The built and installed apps each passed **397 test groups / 0 failures**; both executables contain `x86_64` and `arm64`. The built app launched idle at about 94 MB RSS and 0.3% CPU after 34 seconds, with no new Zynforge crash report. No real-device take was run.
- Verified the signed stage, copied it into `/Applications`, and compared the installed bundle byte for byte with that stage. Both the installed bundle and the prior app pass deep/strict signature verification. Installed GUI SHA-256: `d1044b61516735b7de6f99f3033fe1e2f2e580e2209ae4fc12fbd3deb4141235`; helper SHA-256: `9fc07d5d93ac0adf2afb74fe1ef999d77eea8189154b7aa5ef9aa56fd431ada6`.
- Preserved the prior installed app at `/Applications/Zynforge Recording.app.backup-20260924-before-audit-fixes` and an exact verified copy under `/Users/jeanpierre/Zynforge-App-Backups/before-audit-fixes-bYakSr/`. Neither existing DMG was changed; both contain older code. This installation remains ad-hoc signed and unnotarized.

## Rollback

Stop capture and quit both processes first. Preserve current session data and the current app; then move the named backup back to `/Applications/Zynforge Recording.app`. Verify the restored signature before launch. Restore GUI and bundled daemon together, never just one executable. A prior build may not understand new session metadata: test with a duplicate session, not the only recording copy. Rolling back the app does not undo session-file changes or recover audio overwritten before the fixes.
