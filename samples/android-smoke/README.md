# android-smoke — on-device smoke harness (ADR-0023 G1)

Minimal Android app that proves the SDK renders on a real phone. It
initializes `tiles_renderer` on the `SurfaceView`'s `ANativeWindow`, loads
the bundled **p3** fixture tileset, renders until the tile pipeline settles
(the same 20-frame streak rule as the desktop demo's `--until-loaded`),
then shows **PASS/FAIL** plus the key numbers on screen and writes
`smoke_result.txt` to the app's private files dir.

## For the tester (li)

1. Download **`app-debug.apk`** from the CI run's Artifacts
   (`android-smoke-apk`).
2. Install it on the phone (tap the APK; allow "install unknown apps"
   once). Debug-signed, `arm64-v8a` only, minSdk 28 (the SDK's native
   code links against the android-28 platform: cesium-native's vcpkg
   archives need API 28+ libc symbols).
3. Open **TilesSmoke**, keep the screen on, wait ~30 seconds.
4. The top overlay will show `PASS` (or `FAIL` + reason) with
   `loaded/failed/rendered` tile counts, frame count and settle time.
5. Take a screenshot and send it back. Done — no other interaction needed.

## How the APK is built (CI)

The JNI library is **not** built by Gradle. The repo's top-level CMake
android preset (NDK r27d, real toolchain) builds `libsmoke_jni.so`
linking the real static `tiles_renderer` SDK, exactly like a host app
would. CI then:

1. `cmake --preset android` + `cmake --build --preset android`
   (existing SDK steps, unchanged),
2. stages `build/android/libsmoke_jni.so` **and** the NDK's
   `libc++_shared.so` (the preset uses `c++_shared`) into
   `app/src/main/jniLibs/arm64-v8a/`,
3. `gradle assembleDebug` in this directory (Android SDK platform 34 +
   build-tools 34.0.0 installed on the runner),
4. uploads `app-debug.apk` as the `android-smoke-apk` artifact.

## Local build

```sh
# 1. JNI lib via the repo's CMake (needs ANDROID_NDK_HOME=r27d):
cmake --preset android -DTILES_WITH_FILAMENT=ON
cmake --build --preset android --target smoke_jni

# 2. Stage the .so files:
mkdir -p samples/android-smoke/app/src/main/jniLibs/arm64-v8a
cp build/android/libsmoke_jni.so \
   samples/android-smoke/app/src/main/jniLibs/arm64-v8a/
cp "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" \
   samples/android-smoke/app/src/main/jniLibs/arm64-v8a/

# 3. APK (needs Android SDK platform android-34 + build-tools 34.0.0,
#    Gradle 8.7, JDK 17):
cd samples/android-smoke && gradle assembleDebug
```

## Honest boundaries

- The APK has only ever been **built** by CI; whether it actually starts
  and renders on a phone is exactly what the manual test above answers.
- Single ABI (`arm64-v8a`), portrait/landscape both render at the
  initial surface size — rotation mid-run is not handled (minimal
  harness).
- `setMaxCachedBytes(64MB)` is set for the phone; IBL stays on (default).
