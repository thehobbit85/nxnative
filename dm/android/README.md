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
