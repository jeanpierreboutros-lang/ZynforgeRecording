# ZynForge Recording app icon

The shipped forge-mark icon source is [`AppIcon.png`](AppIcon.png), referenced by `ICON_BIG` in the root `CMakeLists.txt`. JUCE creates the bundle icon from this PNG for the universal macOS app. The current `c563b00` app and DMG contain that built icon; see [installation details](../../INSTALL.md).

To change the icon, replace `Source/Theme/AppIcon.png` with a reviewed square PNG, rebuild the Release app, and inspect the result at small and large sizes in Finder and the Dock. Keep the image change in the same source commit as the new icon and package a fresh DMG from the tested bundle.

An iconset can be introduced later if small-size artwork needs separate treatment. No `brand/app-icon-1024.png` or `brand/ZynForgeRecording.iconset` source is present in this repository, so older copy commands referring to those paths are not part of the current build workflow.
