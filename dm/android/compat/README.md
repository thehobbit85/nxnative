# Android compatibility headers

The shared host now has native bionic implementations for memory, scheduling, filesystem,
saves and media. The Android build does not force-include the donor's Mach compatibility
header or link its failure-returning shims. `TargetConditionals.h` supplies the platform
constants; `CommonCrypto/CommonDigest.h` and `sha256.c` retain the portable SHA-256 implementation.

The app and build integration reuse `joaquinsandoval2307/nxnative` at
`32b0ef2c5408e1e32cb5c0bc49f84d1e63653dc3`. Its renderer is maintained at
`dm/gfx/nvn/vulkan/`, alongside the shared recorder. Existing portable host improvements
supersede obsolete Mach and Darwin filesystem adapters.
