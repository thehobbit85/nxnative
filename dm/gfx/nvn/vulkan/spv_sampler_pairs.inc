// Resolve the descriptor bindings used by OpSampledImage, independently of
// array subscripts. Unsupported provenance uses every candidate image.
typedef struct { uint64_t mask[(DM_VK_MAX_BINDINGS+63)/64]; } sampler_pairs;

static sampler_pairs *reflect_sampler_pairs(const uint32_t *w, size_t count, const sbind *bindings, int nb) {
    int has_sampler=0;
    for (int i=0; i<nb; ++i) has_sampler |= bindings[i].type==BT_SAMP;
    if (!has_sampler || count<5 || w[0]!=0x07230203 || !w[3] || w[3]>0x3fffff) return NULL;
    uint32_t bound=w[3];
    uint16_t *origin=calloc(bound,sizeof *origin); // binding index plus one
    sampler_pairs *pairs=calloc(nb,sizeof *pairs);
    if (!origin || !pairs) goto unknown;
    enum { Load=61, AccessChain=65, InBoundsAccessChain=66, PtrAccessChain=67, InBoundsPtrAccessChain=70,
           Decorate=71, GroupDecorate=74, CopyObject=83, SampledImage=86, Image=100, Select=169 };
    for (size_t pos=5; pos<count;) {
        const uint32_t *p=w+pos;
        unsigned n=p[0]>>16, op=p[0]&0xffff;
        if (!n || n>count-pos || op==GroupDecorate) goto unknown;
        if (op==Decorate && n==4 && p[2]==33 && p[1]<bound) {
            for (int i=0; i<nb; ++i) if (bindings[i].binding==p[3]) { origin[p[1]]=i+1; break; }
        } else if ((op==Load || op==AccessChain || op==InBoundsAccessChain || op==PtrAccessChain ||
                    op==InBoundsPtrAccessChain || op==CopyObject || op==Image) && n>=4 && p[2]<bound && p[3]<bound) {
            origin[p[2]]=origin[p[3]];
        } else if (op==Select && n==6 && p[2]<bound && p[4]<bound && p[5]<bound && origin[p[4]]==origin[p[5]]) {
            origin[p[2]]=origin[p[4]];
        } else if (op==SampledImage) {
            if (n!=5 || p[2]>=bound || p[3]>=bound || p[4]>=bound) goto unknown;
            int texture=(int)origin[p[3]]-1, sampler=(int)origin[p[4]]-1;
            if (sampler<0 || bindings[sampler].type!=BT_SAMP) goto unknown;
            if (texture<0 || (bindings[texture].type!=BT_TEX && bindings[texture].type!=BT_CIS))
                memset(&pairs[sampler],0xff,sizeof pairs[sampler]);
            else pairs[sampler].mask[texture/64] |= UINT64_C(1)<<(texture%64);
            origin[p[2]]=origin[p[3]];
        }
        pos+=n;
    }
    free(origin);
    return pairs;
unknown:
    free(origin); free(pairs);
    return NULL;
}
