# ARM64 Android integration

The Android host uses the shared Horizon services, rewriter and loader, and the NVN-to-Vulkan backend in `dm/gfx/nvn/vulkan/`. The native frontend, menu, asset packer and build recipe reuse the Android port from [joaquinsandoval2307/nxnative](https://github.com/joaquinsandoval2307/nxnative/tree/32b0ef2c5408e1e32cb5c0bc49f84d1e63653dc3), adapted to current host interfaces. Existing memory, scheduling, save, media and renderer improvements on this branch are retained.

## Native build

Install an Android NDK with the ARM64 API 30 compiler, then set `ANDROID_NDK_HOME` (or pass `NDK=`). API 30 / Android 11 is the minimum. The build links libvpx and dav1d through the shared codec build. Both binaries use 16 KiB ELF segment alignment; this is not a claim that all runtime paths have been tested on a 16 KiB kernel.

```sh
make -C dm/android GAME=<title-ID> BUILD_ROOT=<outside-build-directory>
make -C dm/android GAME=<title-ID> BUILD_ROOT=<outside-build-directory> dmhost
```

The first builds `<outside-build-directory>/<title-ID>/libdmhost.so`; the second builds the windowless `dmhost` CLI. There is no default game. Derived profiles use `DM_GAMES_DIR` as on Apple. Run the CLI from an Android shell as `dmhost <rewritten-module-directory> [module names]`. All modules must be rewritten with `patch_nso.py --thread-slots android`; Apple rewrites are incompatible.

The app frontend owns the NativeActivity window, input queue, controller/touch overlay, settings environment and pause/resume callbacks. Graphics work drains on pause, and a recreated native window gets a new Vulkan swapchain. Touch controls and physical controllers currently feed one guest player. Text keyboard and native motion/haptic routing remain parity work.

The complete CLI and native library link without unresolved references using NDK r27d. Synthetic modules have reached SDK initialization and `nnMain` through the production CLI and app, submitted NVN commands and presented a Vulkan image on an ARM64 Android 15 emulator (SwiftShader). Background/resume and surface recreation have been observed. Commercial integration checks are tracked in the [Hollow Knight note](../games/0100633007D48000/README.md); physical Android hardware remains untested.

See [Android parity](../../docs/android-parity.md) and [NVN-to-Vulkan parity](../../docs/nvn-vulkan-parity.md) for the remaining scope. Guest Vulkan and desktop OpenGL entry points still need separate backends; this target currently connects the NVN path.

For a running Vulkan game, `DM_CTL=<external-files-path>/ctl.txt` accepts controller
presses and `passdump N` to log the next N complete frames' passes, shaders and
copies. Screenshot and texture-dump commands remain separate Vulkan diagnostics
(`DM_VK_SHOTS`, `DM_VK_RTDUMP`); Metal's interactive dump commands are not all supported.

`DM_CTL` also supports `webshot <path>` for the offline HTML page. Memory-change
polling (`DM_PROBE_POLL`) supports addresses, heap offsets and value filters;
live thread backtraces are asynchronous. `DM_PROBE_WATCH` intercepts writes to
managed heap, alias, stack or module data; arbitrary host allocations use polling.
See [memory polling](../../docs/reference/memory.md#memory-change-polling-on-android)
and [managed write watches](../../docs/reference/memory.md#managed-write-watches-on-android).
`DM_MEMSTAT=<path>` and `DM_THRCPU=<path>[,seconds]` write target-native memory
and CPU time series; `DM_MEMSTAT_DUMP=<seconds>` adds a region breakdown. See
[Android accounting](../../docs/reference/memory.md#android-memory-and-thread-cpu-accounting)
for units, classification and unavailable-data behavior. Use paths writable by the app.

## Packaging and verification

Set `ANDROID_SDK_ROOT` to an SDK with build-tools and an Android platform installed, and `JAVA_HOME` to a JDK. Build-tools 35.0.0 / platform 35 were used for this integration. The manifest and dex minimum are API 30; the target SDK remains the donor's API 33. This is a local development APK, not a store release.

The complete NSP/XCI pipeline is currently supported on macOS because extraction/probing and shader preparation use Apple tools. Shader preparation can now emit a verified capture for portable SPIR-V export using Python and SPIRV-Tools; see [the preparation/export boundary](../../docs/reference/shaders.md#portable-export-from-prepared-shaders). The prepared-folder native recipe selects macOS or x86-64 Linux NDK tools; Linux preparation and packaging have not received the macOS end-to-end qualification. Windows and ARM64 Linux build hosts are not supported by this wrapper yet. Install GNU Make, the API 30 NDK compiler, a working JDK with Java 11 language support, SDK build-tools (`aapt2`, `d8`, `zipalign`, `apksigner`) and a platform `android.jar`. The SDK tools must support the chosen JDK.

Both builders check Android tools before native compilation; the NSP/XCI builder does so before extraction and shader generation. `scripts/build_android.py <title-ID> <prepared-folder> <output.apk> --check-tools` checks prerequisites without building. SDK/JDK/NDK and prepared/output paths may contain spaces. Keep the repository checkout path free of spaces and shell characters: the native Makefiles still use unquoted relative dependencies, so the wrapper rejects unsupported checkout paths before work starts. APKs and receipts must stay outside collected asset directories (RomFS, modules, shaders, fonts, DLC and seed saves).

The main builder accepts Android for NVN games, including NSP and XCI input folders:

```sh
python3 scripts/build_ipa_from_nsp.py <game-folder> -k <prod.keys> --platform android
```

It shares extraction, profile selection and shader discovery with Apple builds, exports validated SPIR-V, rewrites modules with the Android thread ABI, and builds a development APK. Apple capture/probe modules remain in `patched_<version>/`; Android execution uses `patched_android_<version>/`. These are not interchangeable. Guest Vulkan/OpenGL targets fail explicitly until their Android backends exist. The current shader preparation still requires a Mac, the pinned translator and the shared Metal compiler tools; targeting Android does not yet make every preparation stage portable.

`--romfs separate` builds a small APK with an expected-asset manifest and prints the deployment command. Install it first, then supply an explicit adb serial to that command. Deployment verifies the staged files against the installed APK and uses the menu's recoverable installation transaction. A different build's data is rejected; the menu enables Play only for its matching installed identity. Old APKs without this manifest must be rebuilt before using the current deployment tool. Updating an existing installation needs space for both old and staged assets. Native RomFS images are supported directly; unpacking them into a second directory is unnecessary. Run one deployment per application at a time. See [separate-data deployment](../../docs/reference/packing.md#separate-android-deployment-mixes-asset-generations).

For a prepared folder, the standalone wrapper below remains available. It contains `patched_android/*.patched.bin` and their rewriter TSV files, `shader/vk/*.spv` and `*.vk.txt`, optional `font/`, and one `extract/*/RomFS` directory or `RomFS.img`. `--patched` and `--romfs` select an explicit version. Export shaders with `dm/android/tools/spv_android.py <shader-package> <new-output-directory> --keys-file <keys-file>`; the main builder does this automatically and checks product hashes before incremental reuse.

Before SDK compilation, the packer checks the native library's ELF64/AArch64 shared-library header, the Android `main` module and rewrite metadata, shader/descriptor file pairs, and explicitly supplied asset paths. It rejects an output path that would overwrite an input or signing key. These are packaging checks; they do not qualify every shader stage or dynamic-loader behavior.

Export retries retain compiled Metal libraries in `shader/vk-metallib`, keyed by their contents and compiler settings. They still translate and validate fresh Vulkan SPIR-V: the complete translation cache stays isolated from Apple jobs, whose texture operations can differ.

```sh
python3 scripts/build_android.py <title-ID> <prepared-folder> <output.apk> --bundle
```

This builds the native library under `<prepared-folder>/android-host/` and packages the reused Java menu. `--build-root` selects another output directory; `--profile <profile-ID>` selects a shared build profile while keeping the supplied title ID for the app and its storage. The wrapper supports spaces and shell characters in build and NDK paths through stable, private symlinks under `/tmp`; native objects remain in the requested build folder and are reused. Direct `make` invocations still need paths without spaces or shell characters because Make represents its dependencies as word lists. Pass `--save <folder>` to seed an empty save directory, `--cheats <file>` to bundle build-matched cheats, and `--label <name>` for the menu title. `--test` uses a separate `.test` application ID. An APK without `--bundle` expects files deployed into its external storage; `dm/android/tools/make_apk.py --deploy <title-ID> <prepared-folder> --serial <adb-serial>` explicitly selects the device. Nothing installs automatically during a normal build.

The development signing key stays at `<prepared-folder>/android-debug.keystore` (override with `--keystore`). Keep that key for app updates. The default version code uses seconds since the Unix epoch when rebuilding; unchanged builds retain their APK and version code. Increase an explicit `--version-code` for every content update. The main NSP/XCI builder exposes these as `--android-keystore` and `--android-version-code`. These are development keys with the `nxnative` alias and `android` password. The main builder takes the displayed app version from the selected game's control data; the standalone wrapper accepts `--version-name` (default `1.0`). Display versions do not control Android update ordering. Saves live in internal app storage and are not overwritten by unpacking game assets. Logs live in the app's external `files/log.txt`, with the preceding run in `log.prev.txt`.

Offline HTML documents are collected from `html/` beside each selected RomFS image or directory, matching the shared extractor's layout. They are verified and installed with that program's other assets; `DM_HTML` points to the selected program's installed folder. Android WebView displays them over the native game surface and reuses Apple's browser/controller script. Hidden loading, messages, controller and system Back, callback URLs, script/requested exit, and HOME/resume are exercised by `tests/web_smoke.py` with a synthetic guest APK. Requests remain inside the selected document tree; absent documents retain the shared host's fallback response. This is emulator integration evidence, not qualification of every game's HTML or a physical device.

Every new APK also carries `assets/game/target.tsv`, a portable target contract with application/profile IDs, host architecture and ABI, Android thread slot, shader chain/exporter/schema, profile input hashes, module build IDs and content hashes, and the expected asset inventory. Required modules follow the host’s main-build-ID selection rule. Its host producer record is embedded in `libdmhost.so` by the native build. Nonempty shader exports carry `export-contract.json`, written by the exporter after SPIR-V validation. Packaging rejects stale or missing producer evidence; old native libraries and shader exports need rebuilding. Empty synthetic shader sets are recorded explicitly. The menu and separate-data deployment validate the APK's producer/asset contract before starting or replacing assets. This identifies the NVN backend and its packaging ABI; it does not certify that every NVN feature or shader stage executes correctly. See [Android target compatibility](../../docs/reference/packing.md#android-apk-combines-incompatible-prepared-inputs).

The packer writes `<output.apk>.build.json` beside a completed APK. It reuses the APK only when selected input contents, options, Java sources and SDK/JDK identity match, and the APK and any companion archive still match their recorded hashes. Missing or damaged products rebuild; the receipt is optional for installing or sharing a finished APK. Checking reuse streams the input and product files, including large RomFS images, but skips compilation, compression and signing. Input changes during a rebuild stop publication. See [incremental APK reuse](../../docs/reference/packing.md#android-apk-rebuilt-despite-unchanged-inputs).

The packer retains the donor's deduplicated asset pieces and separate data ZIP for large games. Companion ZIP filenames include a content hash; keep each ZIP with its matching APK. Rebuilds publish only after signature and alignment checks, preserving the previous APK and companion if packaging fails. Before adding a file it reserves its worst-case size below the APK limit; oversized files go into the data ZIP in bounded chunks. The APK uses ordinary ZIP records; the separate data archive permits ZIP64 offsets and is streamed by the menu. The menu validates package/version identity, chunk order, file lengths and SHA-256 digests. It stages every asset before replacing live game roots and recovers interrupted replacements on the next start; updates need storage for the old and new assets together. The content manifest identifies an installed asset set, so changing content with the same version code still triggers installation. Removed assets disappear when the new set is committed. See [Android assets and large RomFS images](../../docs/reference/packing.md#android-assets-and-large-romfs-images) and [interrupted updates](../../docs/reference/packing.md#interrupted-android-asset-updates).

For a profile with `programs`, the shared NSP builder prepares every declared program, then builds one native host and one APK. Program 0 keeps the usual roots; other programs use `programs/<index>/{dm,romfs,vk,font,aoc}`. APK identity, settings and saves remain shared. The build writes `android-programs.json` in its work folder and passes it to the prepared builder and separate-data deployment with `--programs`. A manually prepared catalog has `schema: 1` and a `programs` list; each entry supplies a positive `index`, uppercase `profile` ID, `build` folder and `gfx: "nvn"`, with optional `patched`, `romfs` and `aoc` paths. Relative paths resolve against the catalog's folder. All declared programs must be present and match the native profiles; unsupported guest graphics APIs fail explicitly. Each shader export binds its own program profile, while the native host binds all profiles. See [multi-program packaging](../../docs/reference/packing.md#android-package-omits-secondary-programs). At runtime, `ExecuteProgram` ends the current host and starts the selected program in a fresh Android process, consuming launch parameters once and retaining shared saves/settings. See [program handoff](../../docs/reference/services.md#multi-program-applications-oeexecuteprogram) for behavior and synthetic emulator checks.

Packaging and cheat selection regressions run without a device or game files:

```sh
python3 -m unittest dm.android.tests.test_packaging
mkdir -p /tmp/nx-android-java-tests
javac --release 11 -d /tmp/nx-android-java-tests dm/android/app/java/com/nxnative/CheatSelections.java dm/android/tests/CheatSelectionsTest.java
java -cp /tmp/nx-android-java-tests com.nxnative.CheatSelectionsTest
javac --release 11 -d /tmp/nx-android-java-tests dm/android/app/java/com/nxnative/GameAssets.java dm/android/tests/GameAssetsTest.java
java -cp /tmp/nx-android-java-tests com.nxnative.GameAssetsTest
javac --release 11 -d /tmp/nx-android-java-tests dm/android/app/java/com/nxnative/{GameAssets,AssetInstall}.java dm/android/tests/{GameManifest,AssetInstall}Test.java
java -cp /tmp/nx-android-java-tests com.nxnative.GameManifestTest
java -cp /tmp/nx-android-java-tests com.nxnative.AssetInstallTest
javac --release 11 -d /tmp/nx-android-java-tests dm/android/app/java/com/nxnative/{GameAssets,AssetInstall,AssetDeploy,TargetContract}.java dm/android/tests/{AssetDeploy,TargetContract}Test.java
java -cp /tmp/nx-android-java-tests com.nxnative.AssetDeployTest
java -cp /tmp/nx-android-java-tests com.nxnative.TargetContractTest
```

The cheat menu writes the shared host's stable keys (including duplicate-name suffixes), and deployment with `--test` selects the separate test app's asset directory.

A synthetic test needs no game dump:

```sh
DM_GAME=0100633007D48000 python3 dm/android/tests/app_fixture.py <scratch>/fixture
python3 scripts/build_android.py 0100633007D48000 <scratch>/fixture <scratch>/fixture.apk --bundle --test --aoc <scratch>/fixture/aoc
python3 dm/android/tests/app_smoke.py --serial emulator-5554 --apk <scratch>/fixture.apk --output <scratch>/results
```

The profile selects build configuration only; every module and asset is generated. The smoke test is restricted to an emulator and also accepts a separate-data APK after its matching assets have been deployed. It checks production SDK startup, save create/read/write/commit, DLC enumeration, shared-font loading, public NVN presentation, native input delivered to guest HID, no frames while paused, resume without guest restart, window recreation, and save retention across a process kill and APK reinstall. It leaves the fixture paused and writes JSON, logs and a screenshot. Physical GPU behavior, commercial gameplay, 16 KiB kernels and sustained audio under load remain separate qualification work.
