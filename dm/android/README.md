# ARM64 Android integration

The Android host uses the shared Horizon services, rewriter and loader, and the NVN-to-Vulkan backend in `dm/gfx/nvn/vulkan/`. The native frontend, menu, asset packer and build recipe reuse the Android port from [joaquinsandoval2307/nxnative](https://github.com/joaquinsandoval2307/nxnative/tree/32b0ef2c5408e1e32cb5c0bc49f84d1e63653dc3), adapted to current host interfaces. Existing memory, scheduling, save, media and renderer improvements on this branch are retained.

## Native build

Install an Android NDK with the ARM64 API 30 compiler, then set `ANDROID_NDK_HOME` (or pass `NDK=`). API 30 / Android 11 is the minimum. The build links libvpx and dav1d through the shared codec build. Both binaries use 16 KiB ELF segment alignment; this is not a claim that all runtime paths have been tested on a 16 KiB kernel.

```sh
make -C dm/android GAME=<title-ID> BUILD_ROOT=<outside-build-directory>
make -C dm/android GAME=<title-ID> BUILD_ROOT=<outside-build-directory> dmhost
```

The first builds `<outside-build-directory>/<title-ID>/libdmhost.so`; the second builds the windowless `dmhost` CLI. There is no default game. Derived profiles use `DM_GAMES_DIR` as on Apple. Run the CLI from an Android shell as `dmhost <rewritten-module-directory> [module names]`. All modules must be rewritten with `patch_nso.py --thread-slots android`; Apple rewrites are incompatible.

The app frontend owns the NativeActivity window, input queue, controller/touch overlay, settings environment and pause/resume callbacks. Graphics work drains on pause, and a recreated native window gets a new Vulkan swapchain. Touch controls and physical controllers currently feed one guest player. Text keyboard, native motion/haptic routing, and Apple-specific diagnostics remain parity work.

The complete CLI and native library link without unresolved references using NDK r27d. Synthetic modules have reached SDK initialization and `nnMain` through the production CLI and app, submitted NVN commands and presented a Vulkan image on an ARM64 Android 15 emulator (SwiftShader). Background/resume and surface recreation have been observed. No commercial game dump or physical Android device has been tested on this integration.

See [Android parity](../../docs/android-parity.md) and [NVN-to-Vulkan parity](../../docs/nvn-vulkan-parity.md) for the remaining scope. Guest Vulkan and desktop OpenGL entry points still need separate backends; this target currently connects the NVN path.

## Packaging and verification

Set `ANDROID_SDK_ROOT` to an SDK with build-tools and an Android platform installed, and `JAVA_HOME` to a JDK. Build-tools 35.0.0 / platform 35 were used for this integration. The manifest and dex minimum are API 30; the target SDK remains the donor's API 33. This is a local development APK, not a store release.

The prepared folder contains `patched_android/*.patched.bin` and their rewriter TSV files, `shader/vk/*.spv` and `*.vk.txt`, optional `font/`, and one `extract/*/RomFS`. Export shaders with `dm/android/tools/spv_android.py` after the existing shader translation stage. This milestone does not add Android as a complete NSP pipeline target to `build_ipa_from_nsp.py`.

```sh
python3 scripts/build_android.py <title-ID> <prepared-folder> <output.apk> --bundle
```

This builds the native library under `<prepared-folder>/android-host/` and packages the reused Java menu. `--build-root` selects another output directory. Native make paths currently need to be free of spaces. Pass `--save <folder>` to seed an empty save directory, `--cheats <file>` to bundle build-matched cheats, and `--label <name>` for the menu title. `--test` uses a separate `.test` application ID. An APK without `--bundle` expects files deployed into its external storage; `dm/android/tools/make_apk.py --deploy <title-ID> <prepared-folder> --serial <adb-serial>` explicitly selects the device. Nothing installs automatically during a normal build.

The development signing key stays at `<prepared-folder>/android-debug.keystore` (override with `--keystore`). Keep that key for app updates. The default version code uses seconds since the Unix epoch; increase an explicit `--version-code` for every content update. Saves live in internal app storage and are not overwritten by unpacking game assets. Logs live in the app's external `files/log.txt`, with the preceding run in `log.prev.txt`.

The packer retains the donor's deduplicated asset pieces and separate data ZIP for large games. It validates Android rewrite metadata and requires a new data file to match the package/version. The menu checks file lengths and confines extraction to game asset roots.

A synthetic test needs no game dump:

```sh
DM_GAME=0100633007D48000 python3 dm/android/tests/app_fixture.py <scratch>/fixture
python3 scripts/build_android.py 0100633007D48000 <scratch>/fixture <scratch>/fixture.apk --bundle --test
python3 dm/android/tests/app_smoke.py --serial emulator-5554 --apk <scratch>/fixture.apk --output <scratch>/results
```

The profile selects build configuration only; every module and asset is generated. The smoke test is restricted to an emulator. It checks production SDK startup, save create/read/write/commit, public NVN presentation, native input delivered to guest HID, no frames while paused, resume without guest restart, window recreation, and save retention across a process kill and APK reinstall. It leaves the fixture paused and writes JSON, logs and a screenshot. Physical GPU behavior, commercial gameplay, 16 KiB kernels and sustained audio under load remain separate qualification work.
