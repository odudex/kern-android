# Kern Android

Android simulator shell for the Kern LVGL UI.

## What is included

- Kotlin `SurfaceView` Android app at package `com.odudex.kern`
- JNI/native bridge in `app/src/main/cpp`
- Android LVGL display/input driver using `ANativeWindow`
- Kern simulator shims reused from `third_party/Kern/simulator`
- Submodule-style dependencies:
  - `third_party/Kern` from `https://github.com/odudex/Kern`
  - `third_party/lvgl` pinned to LVGL `v9.5.0`
- Additional native crypto dependency:
  - `third_party/mbedtls` pinned to `mbedtls-3.6.2`

## Build

```bash
./gradlew :app:assembleDebug
```

The debug APK is written to:

```text
app/build/outputs/apk/debug/app-debug.apk
```

## Run On Emulator

```bash
emulator -avd kern_test -no-window -no-audio -no-snapshot -gpu swiftshader_indirect
adb wait-for-device
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.odudex.kern/.MainActivity
```

## Notes

- First milestone is UI simulation only. Camera/QR device integration is still mocked through Kern's simulator shims.
- Kern and LVGL sources are not modified; Android-specific glue lives under `app/src/main/cpp`.
- Android simulates the `wave_5` board at its LVGL logical resolution (`720x1280`) and scales that frame into the phone/emulator `SurfaceView` as large as possible without changing the aspect ratio.
