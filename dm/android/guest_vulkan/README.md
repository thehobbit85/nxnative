# Android guest Vulkan

This frontend serves games that call Vulkan themselves. It is separate from
`dm/gfx/nvn/vulkan`, which executes recorded NVN commands.

`hle_vk_android.c` registers guest imports and procedures through the shared host
tables. Vulkan structures and handles use the common ARM64 layout, so the native
Android loader/driver supplies device discovery and execution. Guest procedures
always return our typed wrappers, which enter the existing guarded host stack;
native driver pointers and Android-only surface entry points are not exported to
the guest. Procedure scope and native extension availability are both checked.

The frontend currently covers instance/device discovery, memory, buffers/images,
descriptors, render passes and SPIR-V graphics pipeline creation. Extension
enumeration intersects the native driver with implemented families: physical
device properties2, memory requirements2, dedicated allocation, bind memory2 and
descriptor update templates. Commands and Nintendo VI presentation are still
open. The Android builder refuses guest Vulkan games until presentation and
packaging are qualified.

Allocation callbacks use the host allocator, matching the Apple frontend's
policy. Instance creation chains and device chains other than core
`VkPhysicalDeviceFeatures2` are currently rejected explicitly: guest callbacks
must not be invoked on native driver threads without a host-to-guest bridge.
This is an open implementation boundary, not a claim of full Vulkan support.

The Apple reference API inventory is `dm/gfx/vk/hle_vk.m`. Procedure lookup follows
the Khronos [instance](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetInstanceProcAddr.html)
and [device](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceProcAddr.html)
scope rules. The separate OpenGL/EGL frontend remains unimplemented on Android.

Build the public driver test with the installed NDK:

```sh
make -C dm/android/guest_vulkan/tests GAME=<title-id> B=<scratch-directory>
```

Run `t_dispatch` on an ARM64 Android emulator. It uses real registered guest
entry points and the native Vulkan driver, including two independent devices,
extension rejection/enumeration, KHR aliases, and a guarded 8 KiB guest stack.

The `resources` target adds `t_resources`, which checks mapped-memory round trips,
descriptor writes/templates, image/framebuffer creation, native SPIR-V graphics
pipeline compilation and pipeline-cache export/reload on those same devices and
stacks. Pixel execution is a separate next check, not implied by pipeline creation.

`generate.py` builds the typed resource wrappers from an explicit command list
and the repository's pinned Vulkan headers. Run `generate.py --check` to verify
the checked-in source; unsupported commands are not auto-exported. The compiler
also verifies every wrapper against its Vulkan function-pointer type.
