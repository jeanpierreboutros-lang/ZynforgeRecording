# ZynForge Recording — app icon

Forge-mark glyph with **RECORDING** beneath it, on a forged-steel squircle.

## Easiest (matches your current CMake) ✅
Your `CMakeLists.txt` already has:
```
ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Source/Theme/AppIcon.png"
```
JUCE builds the `.icns` from that PNG. So just **overwrite it**:
```
cp brand/app-icon-1024.png  Source/Theme/AppIcon.png
cmake --build build --config Debug --clean-first
```
Done — the new icon shows in the dock / Finder. (JUCE downscales the one PNG for
every size; the hex reads fine small, the RECORDING text softens to a texture at
16–32 px, which is normal for wordmark icons.)

## Crisp small sizes (optional, premium)
For pin-sharp dock icons with a **glyph-only** small variant (no blurry text at
16–32 px), use the prebuilt iconset instead of JUCE's auto-generation:
```
iconutil -c icns brand/ZynForgeRecording.iconset -o ZynForgeRecording.icns
```
Then drop `ICON_BIG` from `juce_add_gui_app` and reference the `.icns` in your
bundle (`Info.plist` `CFBundleIconFile`, or copy it into `Resources/`). The
iconset already pairs glyph-only art ≤64 px with the full lockup ≥128 px.

## Files
- `app-icon-1024.png` — 1024² master (forge-mark + RECORDING).
- `ZynForgeRecording.iconset/` — full macOS iconset, glyph-only smalls + lockup larges.
