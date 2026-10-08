# Mobile port handoff

## 2026-10-07: GPU startup compatibility and visible failures

Rebased onto original upstream 0435a46. Shared GPU creation drops unused
clip-distance, indirect-first-instance and anisotropy requirements. Depth
clamping remains enabled because the current ImGui/game pipelines require it.
No alternate shaders or OpenGL fallback. Errors now use a native SDL message
box as well as SDL/APP logging; compiled GPU backend names are logged.
Host mocked startup/failure tests and mobile syntax checks pass. This does
not yet establish support on the reported Xiaomi Mali-G715 device. Capture
SDL/GPU messages if it still fails; the original SDL/APP-only log omits the
driver's feature rejection details.
Android arm64 assembleDebug succeeds; APK archive integrity and signing
verification pass. Physical-device startup remains unverified.

## 2026-10-06: shared touch driving

Rebased onto main 10c85cb before finishing input work. Raw SDL touch events
drive a shared mobile overlay with steering, gas/brake, sequential gears,
V1-V4, Coin, Start and Menu. Finger/device pairs retain their original control;
sliding off a pedal releases it without activating a neighbouring button.
Short taps are latched for one sampled game frame, but cancellation discards
the latch. Menu/background/focus loss and resized safe areas clear all fingers.
The existing Controls sampler merges touch with physical controls, including
the existing steering-invert option. Desktop touch values default to zero.

Tests: test_touch_controls.cpp checks simultaneous steering/pedals, duplicate
finger IDs on separate devices, clamping, release, short taps, cancellation,
menu reset and non-overlapping in-bounds layout at 480x272, 844x390, 2400x1080
and 1024x768. Build/run with:
`c++ -std=c++20 -Isrc -Iextern/sdl3/include tests/test_touch_controls.cpp -o /tmp/daytona-touch-test && /tmp/daytona-touch-test`.
The existing 13 Android-path and 2 desktop-path ROM import tests also pass.

Android requires a consistent SDK environment: this host has ANDROID_SDK_ROOT
pointing at /opt/android-sdk, which conflicts with ANDROID_HOME. The working
command unsets ANDROID_SDK_ROOT and sets ANDROID_HOME to the installed user SDK.
Android Gradle/CMake build caches are now ignored, not removed. The Mac was
temporarily unreachable; after it returned, the iOS device build succeeded.
Tested on devices (2026-10-06): Android and iOS play, touch controls
included, with no issues reported. Device performance figures are not
recorded yet.
Final Android assembleDebug and iOS device Release builds pass. APK/IPA ZIP
integrity checks pass; iOS is unsigned arm64 and its copied SHA-256 matches
the Mac artifact. IPA: build/ios/Daytona-touch-unsigned.ipa. APK:
platform/mobile/android/app/build/outputs/apk/debug/app-debug.apk.

## 2026-10-06: iPhone launcher repair

The supplied device photo showed clipped help text and inaccessible lower
options. NoDecoration includes NoScrollbar, and the desktop style also applied
Retina scale to UIKit logical-point coordinates. Use a visible scrollbar,
blank-space drag scrolling, wrapping, point-sized iOS controls and the SDL safe
area. Add UILaunchScreen metadata to opt out of legacy screen sizing.

SDL's pinned iOS build selects its dummy file-dialog backend. Added a small
ARC Objective-C++ Files picker, using import mode and copying the selected URL
to a unique app-private path while any security-scoped access is active. The
existing archive manifest verification still gates Start. Cancellation clears
pending state; failures appear beside Browse. Each import keeps a separate
copy, so repeated selections consume additional storage rather than replacing
an earlier valid archive.

Xcode device Release build, plist lint and unsigned ZIP validation pass.
Simulator Ninja build initially failed because Objective-C++ was enabled after
the SDL subdirectory; moving enable_language before subdirectories fixes it.
Simulator installation/launch succeeds but rendering stops at SDL_CreateGPUDevice
with "Device does not meet the hardware requirements for SDL_GPU Metal".
Its black screenshot is not evidence of working UI. Next: install the updated
device IPA through AltStore; verify fit, scrolling to Start, Files/iCloud import,
cancel/retry and reopening the saved ROM. These interactions remain unverified.

## 2026-10-06: unsigned iOS package

Built the mobile branch on macOS with Xcode 26.6 and the iPhoneOS 26.5 SDK,
using the existing generated daytona93, TGP and sound sources. Added
`UNSIGNED=1` packaging to the iOS script and the required executable/package
keys to the bundle template. The final script was rerun successfully with
relative `BUILD_DIR=build/ios` and `M2_GEN_ROOT=generated` paths. macOS resource
forks and extended attributes are excluded from the IPA.

Validation: Xcode Release build succeeded; plist lint passed; the executable
is arm64 Mach-O with iOS platform 2 and minimum OS 15.0. Bundle identifier is
`com.boucydesigns.daytona`, executable is `daytona`, device families are iPhone
and iPad. Linked dynamic libraries are Apple system libraries only. codesign
reports the app is not signed. ZIP integrity passed and the IPA contains only
Payload/daytona.app with its executable, Info.plist and PkgInfo, no ROM archive.
The retrieved IPA hash matched the Mac copy:
`417835b73b9f0e188f21a7115958b08c81207a387646d5e78b7d74b18e8b5f42`.
Local artifact and logs are under ignored `build/ios/`.

Next: sign/install through AltStore, supply the user's ROM archive, and test
launcher import, Metal rendering, audio and controller input on an actual iOS
device. Compilation/package validation does not establish on-device gameplay
or AltStore installation success. On-screen driving controls are not added.

## 2026-10-02: Android document read/import

Reported failure: Browse returned a Downloads provider `content://` URI,
then the launcher reported `cannot open content://...`. `archive.cpp` used
`std::ifstream` on the URI. This was a missing Android document-reader path,
not evidence of a corrupt ZIP. The earlier assumption that the unmodified
desktop ROM reader would work with the SDL Android picker was wrong.

Added `app::RomFile` in `src/app/rom_file.h`: SDL opens the granted URI with
mode `rb`, streams it to a bounded app-private temporary file, and the launcher
runs its existing manifest checks on that file. Only a successful check is
committed to the persistent import and saved configuration. Failed copying,
validation and rename leave the previous import in place. Ordinary filesystem
paths still go straight to the runtime; iOS behaviour is unchanged.

Design basis: `docs/daytona-usa-recomp-design.md`, Architecture (thin host
platform layer) and Overview/goals (user-supplied ROMs). The runtime, generated
code, archive validation rules, shaders and Android SDK/Gradle versions were
not changed. No ROM bytes or generated game sources are committed.

Validation: 13 Android-path helper cases and 2 desktop-path cases pass with
both GCC and Clang using the narrow host SDL shim. The original launcher
source was checked against blob `ed26528e7bd7af206a1a478137cc170a4712e217`
before editing. No Android SDK/device or ROM set was available here; neither
an APK build nor on-device import/gameplay is claimed.

Next: rebuild/install with `adb install -r`, Browse to the archive again,
confirm all files verify, start the game, then close/reopen and reset to check
that the private copy is reused. See FILE_ACCESS.md for commands and limits.

Do not re-propose decoding the Downloads URI into a raw `/sdcard` path or
adding all-files access as the fix for `ifstream(content://...)`. The system
picker supplies document access, and SDL's Android IO is the matching reader.
