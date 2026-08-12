# Kern Android

Android simulator shell for the Kern LVGL UI.

> **Research and development only.** Kern is a research and development
> project, not a product. This repository is a simulator shell for exploring
> Kern and air-gapped Bitcoin transactions on a phone. A phone cannot fully
> isolate keys from the OS, libraries, or peripherals, so do not use it for
> wallets holding real funds or important mnemonics.

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

## Prerequisites

- Android SDK with **NDK `30.0.14904198`** and **CMake `4.1.2`** installed via
  `sdkmanager` (or Android Studio's SDK Manager). Both versions are pinned in
  `app/build.gradle.kts`.
- The build needs to know where the SDK lives. Either:
  - export `ANDROID_HOME` (or `ANDROID_SDK_ROOT`) to point at the SDK, **or**
  - create a `local.properties` at the repo root with:
    ```properties
    sdk.dir=/absolute/path/to/Android/sdk
    ```
    Only add `cmake.dir=...` if your CMake 4.1.2 lives outside the standard
    `$ANDROID_SDK_ROOT/cmake/4.1.2/`.

`local.properties` is per-machine — it's gitignored and must not be committed.

## Build

```bash
./gradlew :app:assembleDebug
```

The debug APK is written to:

```text
app/build/outputs/apk/debug/app-debug.apk
```

### Build a single ABI for faster local iteration

The default build produces both `arm64-v8a` (real phones) and `x86_64`
(emulator). Pass `-Pkern.abi` to limit the native build to one ABI —
roughly halves clean-build time and skips the unused-architecture
recompiles during inner-loop work:

```bash
./gradlew :app:assembleDebug -Pkern.abi=arm64-v8a   # phone only
./gradlew :app:assembleDebug -Pkern.abi=x86_64      # emulator only
```

Multiple ABIs can be comma-separated (`-Pkern.abi=arm64-v8a,x86_64`).
For a persistent per-machine default, add the property to your user
Gradle config — this keeps the repo's defaults untouched:

```bash
echo 'kern.abi=arm64-v8a' >> ~/.gradle/gradle.properties
```

Unset the property (or remove the line) to restore the full release matrix.

## Run On Emulator

```bash
emulator -avd kern_test -no-window -no-audio -no-snapshot -gpu swiftshader_indirect
adb wait-for-device
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.odudex.kern/.MainActivity
```

## Install On USB Device

Enable Developer options and USB debugging on the phone, connect it over USB,
then accept the debugging prompt.

```bash
./gradlew :app:assembleDebug
adb devices
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.odudex.kern/.MainActivity
```

## Notes

- The QR scanner and entropy capture flows run against the phone's real
  rear camera via Camera2; the simulator's mock camera shims are only
  used as a fallback when no camera is available (e.g. emulator without
  a virtual webcam).
- A research-and-development notice is shown on every launch; the Kern
  surface is only initialised once it is acknowledged. Kern shows its own
  R&D disclaimer as well, once per version, so the first run of a newly
  built version shows both.
- The About page shows a version of the form `<kern>-sim-dev`, where
  `<kern>` is read at configure time from `third_party/Kern/version.txt`.
- Kern and LVGL sources are not modified; Android-specific glue lives under `app/src/main/cpp`.
- Android simulates the `wave_5` board at its LVGL logical resolution (`720x1280`) and scales that frame into the phone/emulator `SurfaceView` as large as possible without changing the aspect ratio.

## License

MIT — see [LICENSE](LICENSE). Bundled dependencies keep their own licenses:
Kern (MIT), LVGL (MIT), mbedtls (Apache-2.0).
