// Vulkan applies the view's component mapping to border texels too. Opaque
// black with a nonidentity mapping needs optional border-swizzle support;
// transparent black/white avoid that dependency. Constant output components
// are identical in both samples and therefore need no correction.
static int border_correction(samp_par p,const uint8_t map[4],int *nearest,int *other,float factors[4]) {
    const uint8_t wrap[]={p.ws,p.wt,p.wr};
    int needed=nvn_border(p.border,wrap,p.reduction,nearest,other,factors);
    if (p.cmp_mode || p.reduction==NVN_SAMPLER_REDUCTION_MIN || p.reduction==NVN_SAMPLER_REDUCTION_MAX) return 0;
    if (memcmp(map,(uint8_t[]){2,3,4,5},4) && (needed || *nearest==1) &&
        (p.ws==4 || p.ws==5 || p.wt==4 || p.wt==5 || p.wr==4 || p.wr==5)) {
        for (unsigned ch=0;ch<4;ch++) if (map[ch]>5) return 0;
        *nearest=0;*other=2;
        for (unsigned ch=0;ch<4;ch++) factors[ch]=map[ch]<2 ? 0 : p.border[map[ch]-2];
        return 1;
    }
    return needed;
}
