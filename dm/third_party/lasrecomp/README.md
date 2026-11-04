# LAS Recomp depth transfers

The Vulkan depth/stencil format selection and integer-image fallback for nearest
scaled depth copies in `dm/gfx/nvn/vulkan/nvn_vk.c` adapt the implementation in
[LAS Recomp](https://github.com/peiikai/LASRecomp/tree/a6b4f03844ecfb4012f18deff96da8b410aeafcd),
`runtime/nvn/vulkan/textures.c` (`depth24_stencil8`) and `exec.c`
(`depth_blit_via_color`). Copyright (c) 2026 Peiikai; MIT license in [LICENSE](LICENSE).

The adaptation uses nxnative's submission rings and image retirement instead of
VMA, requires sampling/attachment/transfers when selecting D24, and carries
stencil through a separate R8_UINT transfer. Blit capability is checked at copy
time; unsupported nearest depth blits use the fallback. Linear depth resampling
and host/guest packed-depth conversion are separate, unfinished capabilities.
