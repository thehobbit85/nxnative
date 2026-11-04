# NVN-to-Vulkan Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Match the supported observable NVN behavior of the rebased Apple renderer, with independent synthetic tests and commercial regression workloads.

**Architecture:** Keep the shared NVN recorder, object ABI and shader translator. Extend the existing Vulkan executor and reuse the Apple semantics and Android donor implementations where applicable. Build conformance from memory ownership through resources, pipeline execution, queries and presentation; a handler count is not a correctness percentage.

**Tech Stack:** C, Vulkan 1.1 plus negotiated optional features, GLSL/SPIR-V, Python, Android NDK API 30, Apple Metal reference.

**Spec:** [NVN behavior matrix](../../nvn-vulkan-parity.md), [target qualification](../../android-parity.md). Apple baseline: upstream `777a5f6c618a38cd2d7f795f465466fb56920534`; feature baseline after integration: `b96b887f`.

## Global Constraints

- Use only `codex/cross-platform-runtime`, tracking `fork/codex/cross-platform-runtime`; incremental commits, no PR or additional branch.
- Stop when weekly usage has less than 25% remaining. Preserve a precise completed/open ledger at the stopping point.
- Games are regression workloads, not the feature boundary. Current Apple unsupported behavior remains explicitly distinguished from implemented behavior.
- Retain Vulkan 1.1 paths and negotiate optional features. Test both dynamic and legacy rendering where paths differ.
- Do not modify shader-chain tools without updating the chain and rebuilding the local profiles as required by AGENTS.md.
- Keep game data and generated shader products in the corresponding game folder; synthetic fixtures may use `/private/tmp`.

## Review Focus

- A later partial GPU write must override only its bytes, including when an earlier range fully contains a later read.
- Completion of an older submission must not overwrite a newer CPU update or expose freed ring storage.
- GPU-written texture readback must be available to a following GPU consumer before the host waits; row padding stays unchanged.
- Distinct descriptors aliasing the same guest bytes must see coherent writes within one command, including vertex/fragment stages.
- Ring rollover and more than 512 written ranges must retain all live data and respect Vulkan alignment and lifetime rules.

## Task 1: Ordered buffer ownership

**Files:** `dm/gfx/nvn/vulkan/nvn_vk.c`, its `tests/t_replay.c`, new `tests/buffer.comp` and reflection, and `Makefile`.

**Interfaces:** Retain `mtl_sub_begin/commit`, `mtl_exec`, `mtl_sub_copy` and `mtl_wait`. Internally, `gw_add` publishes a guest address range backed by a live submission buffer; `copy_in` resolves a snapshot of CPU bytes plus ordered GPU writes into the current ring. Written ranges live until their fence completes.

- [x] Add a compute reader/writer fixture that checks a broad read spanning two partially overlapping GPU writes, then buffer copy, partial clear, inline update and another GPU consumer. Check exact words and untouched guards before/after a submission split.
- [x] Run the fixture against the existing implementation and retain the failing output.
- [x] Replace stale partial-overlap fallback and fixed-capacity write loss with ordered range composition. Preserve pending GPU writes through copies, clears and updates; record completion copies in stream order.
- [x] Register texture-to-buffer output by the actual copied row ranges, and test its next-command compute consumer with padding canaries.
- [x] Exercise more than 512 live writes and ring rollover; test Vulkan alignment constraints and both rendering paths.
- [x] Run the full replay and public transfer fixture, document the proven slice and remaining alias cases, commit and push.

## Task 2: Graphics storage and simultaneous aliases

**Files:** Vulkan executor and fixture shaders; shared `dm/gfx/tests` public conformance fixture where reusable.

**Interfaces:** Each graphics/compute command gathers writable buffer bindings before encoding. Bindings that overlap within that command share storage and offsets; publication occurs only after successful command encoding.

- [x] Add vertex and fragment SSBO writes followed by compute readers; check three successive atomic draws, guards, raster output and submission splits.
- [x] Implement graphics-stage write ownership and barriers, honoring read-only reflection.
- [x] Add overlapping SSBO descriptors used by one dispatch and both graphics stages; verify words, atomics, guards and runtime-array lengths.
- [x] Unify simultaneous storage bindings, preserving word-offset aliases with private SPIR-V block layouts and aligned descriptors. Validate shared source block types and representative exported shaders.
- [ ] Cover CPU consumers of GPU-produced indices/handles, pool finalization, new pools at reused addresses and outstanding fences.
- [ ] Run synthetic Metal/Vulkan comparisons and commit the validated behavior.

## Remaining parity sequence

Each item expands into exact failing fixtures and implementation tasks before edits; the matrix contains its independently observable cases.

3. Descriptors: separate texture/sampler, typed buffer views, numeric classes, arrays, storage images and descriptor-pool lifetime.
4. Images: supported formats and conversion, typed/aliased views, sparse physical residency, CPU ReadTexels/WriteTexels, subresources, 3D, multisample copies and resolves.
5. Targets and raster state: MRT, masked/scissored clears, depth range/clamp, viewport swizzle, blend/logic/alpha, discard, divisors and sample state.
6. Primitive and shader execution: direct/indirect/counted/multi draws, restart/topology conversions, geometry, transform feedback and shader lowering requirements.
7. Queries and ordering: GPU sample counts and timestamps, conditional rendering, public sync/events, queue ownership and failure paths.
8. Temporal upscaling and presentation: supported fallback and temporal contracts, surface recreation, resize, crop/scale, pacing and resource bounds.
9. Qualification: all matrix rows and public APIs have evidence; run representative generated shaders through export/load, both game regression routes, emulator lifecycle and resource/performance checks. List hardware-only validation separately; do not call it passed without measurements.

Full Android host parity, guest Vulkan/OpenGL APIs, ARM64 Linux and Steam Deck CPU translation remain separate tracks.
