# Android and iOS port

> **Unofficial and vibe coded.** This port was written largely by an AI assistant and has not
> been played through on real phones. Expect crashes, missing features and performance problems.
> Do not report its bugs to the PC port's authors.

The phone apps run the same runtime as the PC port (Aurora on Dawn: Vulkan on Android, Metal on
iOS; SDL3 for the window, input and audio). What a phone cannot do is compile the game: the
game module that setup builds from your disc needs clang and lld, and iOS does not allow
loading code generated on the device at all. So the module is built on a PC and copied over.

## What you need

- Your own Luigi's Mansion disc image (ISO or RVZ), USA (GLME01) and/or Europe (GLMP01).
- A PC with Luigi's Mansion Recompiled **of the same version** as the phone app.
- Android: Android 10 or newer, arm64, Vulkan 1.1. iOS / iPadOS: 15 or newer, sideloaded.

## 1. Export the game module on the PC

Open Luigi's Mansion Recompiled, choose your disc(s) on the setup screen (`--setup` opens it
again), and under **Play on a phone** press **Export for Android** or **Export for iPhone / iPad**.
It writes `lm_game_GLME01.so` / `lm_game_GLMP01.so` (Android) or `.dylib` (iOS) into
`<data folder>/mobile/<android-arm64|ios-arm64>/` and shows the folder. No disc data is in the
file; it is the game code translated for the phone's processor, with a key that ties it to that
disc and that version of the program.

The export uses the bundled clang and lld with `--target=aarch64-linux-android29` or
`--target=arm64-apple-ios15.0`; the module is freestanding, so no Android NDK or Xcode is needed.

## 2a. Android

1. Install `LuigisMansionRecompiled-<version>-android-arm64.apk`.
2. Open the app and use **Import a file...** to pick the disc image and the `lm_game_*.so`
   (they are copied into the app), or copy them over USB into
   `Android/data/io.github.lmrecompiled.mobile/files/` and press **Look in the folder again**.
3. **Install and play**. The module is copied into the app's private storage (Android only runs
   code from there) and checked against the disc.

## 2b. iPhone / iPad

1. Put the module into the unsigned IPA:
   `python3 tools/mobile/inject_ios_module.py LuigisMansionRecompiled-<version>-ios-arm64-unsigned.ipa lm_game_GLME01.dylib`
   (or use your sideloading tool's "inject dylib" option; it must end up inside the app).
2. Sideload the resulting IPA (AltStore, SideStore, Sideloadly, ...). The signing tool signs the
   module along with the app, which is what lets iOS load it.
3. Copy the disc image into the app's folder in the Files app (On My iPhone > Luigi's Mansion),
   or use **Import a file...** in the app.

## Controls

On-screen GameCube controller (left stick, C stick, A/B/X/Y, L/R, Z, D-pad, START). It hides
itself while a real controller is connected, which then drives port 1. **MENU** (or Android's
back button) opens the settings menu; F1 > Visuals > Touch controls sets their size and opacity.

## Files

- Android: settings, caches, saves and the imported module in the app's private storage; imports
  and `last_run.log` in `Android/data/io.github.lmrecompiled.mobile/files/`.
- iOS: settings, caches and saves in the app's Library; imports and `last_run.log` in Documents.

After an app update, export the module again with the PC version that matches.

## Building

- Android: `ANDROID_NDK_ROOT=<ndk> tools/mobile/build_android.sh` (NDK r27+, JDK 17, Gradle 8.12,
  `rustup target add aarch64-linux-android` for nod). Makes `build/*-android-arm64.apk`, signed
  with the debug key unless `LM_ANDROID_KEYSTORE` (+ `_PASSWORD`, `LM_ANDROID_KEY_ALIAS`,
  `LM_ANDROID_KEY_PASSWORD`) is set.
- iOS (on a Mac with Xcode): `tools/mobile/build_ios.sh` (`rustup target add aarch64-apple-ios`).
  Makes `build/*-ios-arm64-unsigned.ipa`.
- GitHub Actions builds both (`.github/workflows/build.yml`, jobs `android` and `ios`).

The phone builds define `LM_MOBILE`: no recompiler, no developer build, no updater; the setup
screen imports instead of compiling (`src/setup/module_builder.cpp`, `src/setup/setup.cpp`),
paths come from `src/port/mobile.cpp`, touch controls from `src/recomp/touch.cpp` (an SDL
virtual gamepad, so bindings work as for any pad). `LM_TOUCH=1` shows the touch controls on a PC.

## Known gaps

- Never run on a device by the author of this port: the whole thing is untested.
- Shader preparation on first start can take minutes on a phone.
- High-end options (Enhanced lighting, high resolutions, high frame rates) will be too heavy for
  most phones; start with the defaults.
- iOS without JIT/sideloading entitlements may limit memory; large settings can get the app killed.
