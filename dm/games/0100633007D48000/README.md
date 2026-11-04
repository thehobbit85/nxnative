# Hollow Knight (`0100633007D48000`)

Update 1.5.12620 (v589824) over the base game, SDK 20.5.21. Unity 6000.0.61f1 IL2CPP with FMOD (one AudioOut) on NVN. Modules `main`, `subsdk0` (the SDK's movie library, for the MP4 cinematics) and `sdk`. Plays on the Mac: the title, a new game, the prologue text and opening cinematic, then King's Pass: walking, jumping, nail attacks, breaking its walls, killing crawlids, geo, damage, death, the shade, respawning, and a save loaded after a restart. No game-specific rewriter cut and no host change of its own. iPhone and iPad validation remains open.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout with the hardware pool: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the block of the other Unity games with virtual address memory; see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game sets near = 0 at startup (log: `depth mode near=0`), so the pipeline bases made before it runs take the `main0_z` vertex entries; with the default `gl`, the prebuild's bases are for the wrong entry and go unused |
| `shader_groups /Data/=data` | `game.tsv` | everything is under `Data/`: the player's serialized files and the `StreamingAssets` bundles |
| No `pad_layout` (the 2D default) | `game.tsv` | B jumps, Y attacks, A confirms in menus |

## Frame rate

- 60 fps, as the game asks. While the opening cinematic plays it asks for present interval 2 (30 fps, frame time p50 33.3 ms).
- With a window, on the title: frame time p50 16.67 ms, p95 16.70 ms, max about 16.9 ms, with one 189 ms frame early in the title's load.
- Windowless, the fixed 60 Hz timer shows p95 up to 20 ms on the title and after a respawn, against 16.77 ms in other stretches of the same run. The window's numbers show that this does not reach the screen, so compare frame times only between runs of the same kind.

## Memory

- As in [Hollow Knight: Silksong](../010013C00E930000/README.md#memory): `main` never sets a heap and the log shows no StandardAllocator; Unity maps its memory itself in the alias region. See [Unity 6 takes its memory with no StandardAllocator and no heap](../../../docs/reference/memory.md#unity-6-takes-its-memory-with-no-standardallocator-and-no-heap).
- The node area takes the SDK-wide function-matched cut on `sdk+0x145d0c` / `+0x145d78` (`dm/tools/sdk_patches.py` `MATCH_CUTS`).
- Measured on the Mac in King's Pass over an 8-minute run with a death and respawn: the alias region's high-water mark +1.75 GB of 4.38 GB, 1.25 GB mapped; resident about 1.5 to 1.56 GB and 0.83 to 0.86 GB of GPU memory in play. A run from a loaded save stayed in the same range (+1.51 GB, resident 1.55 GB, 0.9 GB of GPU memory). Later areas are not measured.

## Files and shaders

- The merged RomFS has 1,641 files, 3.46 GB: serialized assets, 228 FMOD banks and 16 MP4 movies.
- 1,812 shaders from the `Shader` objects of `data.unity3d` and the UnityFS bundles, read by `unityfs.py`; `main` also holds 11 GLSLC outputs of Unity's player.
- The prebuild makes 858 pipeline bases from 873 shader pairs on the Mac: none fail, 15 are left to the draws.
- `pso_manifest.tsv` has 110 lines, recorded along the route below and through a save load.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` and use its printed command.
- From a fresh save, the title is up by about 25 s. A on Start Game, A on the first slot, then the prologue text and the opening cinematic (about two minutes; skipping was not tried) lead to King's Pass.
- In King's Pass, hold right. Walls of vines block the way until struck with Y (five strikes were always enough). Then there are a pit (B to jump out) and crawlids.
- A save written there loads with A, A from the title and returns to King's Pass.

## Android integration

Current: the shared builder's `--platform android` route builds the registered v589824 update into an ARM64 development APK. All 1,812 shader stages pass SPIR-V validation and resource-sidecar checks. An Android 15 ARM64 emulator with SwiftShader reaches the title, profile selection, save commits, prologue and decoded opening movie; a subsequent launch recognizes and loads the saved King's Pass profile. The original shaders now render the player, HUD, title background and scene effects; scripted movement, jumping and nail attacks are visible in King's Pass. This is an early-area emulator result, not physical-device or whole-game validation.

Resolved: missing player and HUD came from [texture uploads without a sampling dependency](../../../docs/reference/rendering.md#vulkan-texture-uploads-visible-to-sampling). The player atlas is 4096x4096 ASTC 4x4 and the HUD atlas 2048x2048; both were uploaded but sampled as zero. A solid-colour replacement proved the geometry was present, and an isolated atlas replay reproduced the problem. The general image barrier fixes it with the original shaders and depth state. Geometry/runtime parity and later areas remain outside this check.

Save check: Quit to Menu committed the played session; after stopping and restarting the Android process, the profile retained its increased play time and loaded King's Pass again with correct player/HUD rendering. Audio callbacks are observed, but listening and physical controls remain untested.

The separate asset deployment uses the merged RomFS image directly. Synthetic APK checks cover shared fonts, DLC, input, pause/resume, surface recreation and save retention across reinstall; these are general integration checks, not substitutes for this game's gameplay.

## Found on this game

- [Vulkan texture uploads visible to sampling](../../../docs/reference/rendering.md#vulkan-texture-uploads-visible-to-sampling): restored the Android player, HUD, title background and scene effects.

- [Android assets and large RomFS images](../../../docs/reference/packing.md#android-assets-and-large-romfs-images): shared builder and APK review, with synthetic font/DLC and archive checks.
- [Interrupted Android asset updates](../../../docs/reference/packing.md#interrupted-android-asset-updates): staged installation, content checks and recovery qualified with synthetic assets.
- [Android build paths split by Make](../../../docs/reference/packing.md#android-build-paths-split-by-make): the wrapper preserves native output paths and incremental reuse through stable aliases.
- [Incomplete Android package inputs](../../../docs/reference/packing.md#incomplete-android-package-inputs): preflight checks reject missing modules, resource pairs, explicit assets and wrong library ABIs.
- [Incremental APK reuse](../../../docs/reference/packing.md#android-apk-rebuilt-despite-unchanged-inputs): selected-input and product hashes avoid unchanged repackaging; synthetic fixtures cover invalidation and interrupted publication.
- [Separate Android deployment](../../../docs/reference/packing.md#separate-android-deployment-mixes-asset-generations): the installed APK identifies expected data; the shared installer verifies, replaces and recovers managed roots.

- [Vulkan export preserves packaged shader stages](../../../docs/reference/shaders.md#vulkan-export-preserves-packaged-shader-stages): Android export validates all 1,812 stages, including one geometry asset; runtime geometry support remains open.

- [The prepare step inside the shader map's once](../../../docs/reference/shaders.md#the-prepare-step-inside-the-shader-maps-once): the crash at the first program with a window, already found and fixed on other games; this port added the kept prefetch items and the test.
- Existing [Unity 6 Shader objects](../../../docs/reference/shaders.md#unity-6-shader-objects) and the node area's [function-matched cut](../../../docs/reference/memory.md#the-sdks-address-region-node-area) work without another change.

## Open

- iPhone and iPad runtime validation; no IPA has been built.
- Sound is checked only in the log (one AudioOut, no underruns): nobody has listened to it. Input has gone only through `DM_CTL`, not a controller or the keyboard.
- Dirtmouth and everything past King's Pass. Precise platforming through `DM_CTL` was impractical, so no door between rooms has been crossed here, and memory in later areas is not measured.
- The base game alone (v0) was not played or registered; packing it takes a derived profile.
