// Keep the GPU real while exercising a smaller advertised limit or an absent
// feature. Check the actual VkSamplerCreateInfo before giving it to the driver.
static VkPhysicalDeviceLimits advertised;
static int has_aniso, checked;
static float request_bias, request_aniso;
VKAPI_ATTR void VKAPI_CALL __real_vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *);
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *out) {
    __real_vkGetPhysicalDeviceProperties(pd, out);
    if (getenv("DM_TEST_SMALL_SAMPLER_LIMITS")) {
        out->limits.maxSamplerAnisotropy = fminf(out->limits.maxSamplerAnisotropy, 2);
        out->limits.maxSamplerLodBias = fminf(out->limits.maxSamplerLodBias, 2);
    }
    advertised = out->limits;
}
VKAPI_ATTR void VKAPI_CALL __real_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *out) {
    __real_vkGetPhysicalDeviceFeatures2(pd, out);
    if (getenv("DM_TEST_DISABLE_ANISOTROPY")) out->features.samplerAnisotropy = VK_FALSE;
    has_aniso = out->features.samplerAnisotropy;
}
VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateSampler(VkDevice, const VkSamplerCreateInfo *, const VkAllocationCallbacks *, VkSampler *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateSampler(VkDevice dev, const VkSamplerCreateInfo *info, const VkAllocationCallbacks *a, VkSampler *out) {
    if (info->addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT && info->addressModeV == VK_SAMPLER_ADDRESS_MODE_REPEAT) {
        float bias = fminf(15.99f, fmaxf(-16, request_bias));
        bias = fminf(advertised.maxSamplerLodBias, fmaxf(-advertised.maxSamplerLodBias, bias));
        float aniso = has_aniso ? fminf(advertised.maxSamplerAnisotropy, floorf(fminf(16,request_aniso))) : 1;
        if (info->mipLodBias != bias || info->minLod != 0 || info->anisotropyEnable != (aniso > 1) ||
            (info->anisotropyEnable && info->maxAnisotropy != aniso))
            die("sampler requested bias %g aniso %g; driver received bias %g minLod %g aniso %d/%g, expected %g/0/%g",
                request_bias, request_aniso, info->mipLodBias, info->minLod, info->anisotropyEnable, info->maxAnisotropy, bias, aniso);
        checked++;
    }
    return __real_vkCreateSampler(dev, info, a, out);
}
static void sampler_limits(void) {
    enum { slot=1461 };
    tex_t tex = {.serial=0x17300,.d={.width=4,.height=4,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32F}};
    nvn_tex_layout(&tex.d, &tex.L);
    mtl_init(); mtl_reg_tex(slot, &tex, NULL);
    const float biases[]={90,-90,1.25f,-.75f}, anisotropy[]={64,8,3.75f,1.75f};
    for (int i=0; i<4; i++) {
        request_bias=biases[i]; request_aniso=anisotropy[i];
        samp_par p={.ws=NVN_WRAP_MODE_MIRRORED_REPEAT,.wt=NVN_WRAP_MODE_REPEAT,.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE,
                    .magf=NVN_MAG_FILTER_LINEAR,.minf=NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,
                    .lod_bias=request_bias,.lod_min=-5,.lod_max=2,.aniso=request_aniso};
        mtl_reg_samp(slot+i, &p);
        list_t list={0};
        struct { uint32_t ints[8]; float rgba[4]; } result={0};
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&tex,.r={.width=4,.height=4,.depth=1},.mask=15,.c.f={.5f});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&typed_border_program[0],.stages=0x20);
        RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=2,.h=((uint64_t)(slot+i)<<20)|slot);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)&result,.size=sizeof result);
        RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        assert(result.rgba[0]==.5f && result.rgba[3]==1);
    }
    assert(checked==4);
    mtl_reg_tex(slot,NULL,NULL); mtl_tex_release(&tex);
    puts("Sampler limits: bias, fractional and optional anisotropy and real filtered readback passed");
}
