# Android / iOS port

This port keeps the desktop runtime and SDL3/SDL_GPU renderer, and packages the
same recompiled game for Android and iOS.

## Prerequisite: generate the game code on a desktop host

ROM-derived C++ is never committed. Build/recompile once on the host so these exist:

- `build/gen/daytona93/*.cpp`
- `build/gen/daytona93_tgp/tgp_gen.cpp`
- `build/gen/daytona93_snd/snd_gen.cpp`

Normally:

```sh
./setup.sh
python3 scripts/recompile.py
```

The mobile builds read that directory through `M2_GEN_ROOT`.

## Android

GPU startup failures now show a native **Daytona Error** dialog and log the
compiled GPU drivers. Vulkan API version alone does not establish SDL_GPU
compatibility. Startup opts out of unused shader clip distances, indirect
first-instance drawing and anisotropic filtering; depth clamping remains
required by the current game and ImGui pipelines. This is not an OpenGL
fallback, nor a guarantee for every Mali/Adreno driver.

If startup still fails, capture the GPU diagnostics (not only SDL/APP):

```sh
adb logcat -d -s SDL SDL/APP SDL/GPU AndroidRuntime libc
```

Requirements: JDK 17, Android SDK API 37, NDK 28.2.13676358, CMake 3.31.6, and
Gradle compatible with Android Gradle Plugin 9.4.1.

```sh
python3 scripts/setup.py --no-build
cd platform/mobile/android
gradle :app:assembleDebug
```

The APK is under `app/build/outputs/apk/debug/`.

The Android package uses SDL3's `SDLActivity`, builds SDL as `libSDL3.so`, and loads the
game CMake target as `libmain.so`. It is arm64-only for now. SDL handles Bluetooth/USB
gamepads and the SDL file dialog is used by the existing launcher for selecting a user
provided ZIP/7z ROM set.

## iOS

Requirements: macOS, Xcode, CMake, and an Apple signing identity for installation on a
physical device.

```sh
platform/mobile/ios/build.sh
```

That generates an Xcode iOS project and builds `daytona.app`. Open the generated
`daytona_recomp.xcodeproj`, select your Team under Signing, then deploy to the device.
The bundle opts into Files document sharing/open-in-place so the user's ROM archive can
be made available to the launcher.
On iOS, Browse opens the native Files document picker and imports a private copy
before the existing ROM checks. Choose your daytona93 ZIP/7z from On My iPhone,
iCloud Drive, or another Files provider. Each selection keeps a separate copy,
so an invalid selection cannot overwrite an earlier import. The launcher uses
the screen safe area; drag blank space vertically or use its right scrollbar
to reach the remaining options and Start button.

For an unsigned IPA to sign and install through AltStore:

```sh
UNSIGNED=1 platform/mobile/ios/build.sh
```

The output is `build/ios/Daytona-unsigned.ipa`, containing the arm64 iPhoneOS app
inside `Payload/daytona.app`. This mode disables Xcode code signing; AltStore
must sign the IPA before installation. It does not bundle a ROM archive. Set
`M2_GEN_ROOT` if the generated sources are outside `build/gen`, and `BUILD_DIR`
to select a different output directory. Tested on devices (2026-10-06): Android and
iOS play with no issues reported.

## Current input

SDL3 gamepads work on both platforms using the same bindings as desktop. The launcher is
touch/mouse compatible through SDL/ImGui. iOS and Android show a shared touch overlay
during gameplay:

- Slide across STEER at the lower left for analogue steering; the middle is neutral.
- Hold GAS or BRAKE at the lower right. Different fingers can steer and use pedals together.
- Gear -/+ shift sequentially; V1-V4 select the four views.
- Coin and Start operate the arcade buttons. Menu pauses and returns to the launcher.

Controls scale with the safe area. Releasing a finger, changing the layout, pausing,
or backgrounding the app clears held input. Brief button taps are retained until
the next game frame. Touch and physical controls can be used together. Touch
steering uses the existing invert setting; buttons are full-travel pedals.
