// Replay a Vulkan DM_NVN_CAPTURE archive through the production NVN executor.
// Reuses the shared capture format and Metal replayer's portable relocation.
// Usage: nvn_replay <capture> [--png path]; DM_VK_DIR selects shader products.
// Exit 0: exact presented pixels, 1: pixel mismatch, 2: incompatible/incomplete.
#include "../nvn_vk.c"
#include <sys/mman.h>
#include <time.h>

#define CAP_MIN(a,b) ((a)<(b) ? (a) : (b))
#define CAP_MAX(a,b) ((a)>(b) ? (a) : (b))
#define RFAIL(...) do { fprintf(stderr,"[RED] Vulkan replay: " __VA_ARGS__); fputc('\n',stderr); exit(2); } while (0)
typedef struct { cap_chunk c; uint8_t *d; } chunk;
static chunk *rp_ch; static size_t rp_nch;
typedef struct { uint64_t old_lo,size; uint32_t kind; uint8_t *mem; } region;
static region *rp_rg; static uint32_t rp_nrg;
static pool_t *rp_po; static uint32_t rp_npo;
static tex_t *rp_to; static uint32_t rp_nto;
static prog_t *rp_pr; static cap_prog *rp_prc; static uint32_t rp_npr;
static dpool_t *rp_dp; static uint32_t rp_ndp;
static sync_t *rp_sy; static uint32_t rp_nsy;
static uint8_t **rp_seg; static uint32_t rp_nseg;
static uint64_t rp_bad;
static int rp_ounce;
int dm_sdk_ounce(void) { return rp_ounce; }
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
uint64_t guest_ns(void) { return now_ns(); }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
void crash_install(void);
void nvn_serial_floor(uint64_t);
void nvn_ft_poll(void);
void replay_program(prog_t *,const cap_prog *);
int replay_dpool(dpool_t *,const cap_dpol *);

static int dec_addr(void *ctx, uint64_t *field, uint64_t len, int writable) {
    (void)ctx; (void)writable;
    uint64_t v = *field; if (!v) return 1; if (!cap_is_addr(v)) { rp_bad++; return 0; }
    uint32_t r = cap_addr_region(v); uint64_t off = cap_addr_off(v);
    if (r >= rp_nrg || !rp_rg[r].mem || off > rp_rg[r].size || (len > rp_rg[r].size - off)) { rp_bad++; *field = 0; return 0; }
    *field = (uint64_t)(uintptr_t)rp_rg[r].mem + off; return 1;
}
static int dec_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size);
static const cap_ops rp_dec = {dec_addr, dec_obj};
static int dec_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size) {
    (void)ctx; uint64_t v = *field; if (!v) return 1;
    int k = cap_obj_kind(v); uint64_t i = cap_obj_index(v);
    if (k != kind && !(kind == CAP_K_TEX && k == CAP_K_TEXIN)) { rp_bad++; *field = 0; return 0; }
    void *p = NULL;
    switch (k) {
        case CAP_K_TEX: p = i < rp_nto ? &rp_to[i] : NULL; break;
        case CAP_K_TEXIN: if (payload && size >= sizeof(tex_t) && i <= size - sizeof(tex_t)) { p = payload + i; cap_tex_fields(p, &rp_dec, NULL); } break;
        case CAP_K_PROG: p = i < rp_npr ? &rp_pr[i] : NULL; break;
        case CAP_K_DPOOL: p = i < rp_ndp ? &rp_dp[i] : NULL; break;
        case CAP_K_SYNC: p = i < rp_nsy ? &rp_sy[i] : NULL; break;
        case CAP_K_POOL: p = i < rp_npo ? &rp_po[i] : NULL; break;
        case CAP_K_SEG: p = i < rp_nseg ? rp_seg[i] : NULL; break;
    }
    if (!p) { rp_bad++; *field = 0; return 0; }
    *field = (uint64_t)(uintptr_t)p; return 1;
}

static const char *head_get(const char *head, size_t n, const char *key, char *out, size_t on) {   // value of "key\tvalue\n" in the header text
    size_t kl = strlen(key);
    for (size_t i = 0; i < n;) {
        const char *e = memchr(head + i, '\n', n - i); size_t ln = e ? (size_t)(e - head - i) : n - i;
        if (ln > kl && !memcmp(head + i, key, kl) && head[i + kl] == '\t') { size_t vl = CAP_MIN(ln - kl - 1, on - 1); memcpy(out, head + i + kl + 1, vl); out[vl] = 0; return out; }
        i += ln + 1;
    }
    return NULL;
}
static void apply_env(const char *head, size_t n) {   // the live run's DM_* settings, so the replay takes the same paths (a setting given to the replayer wins)
    static const char *const skip[] = {"DM_NVN_CAPTURE", "DM_VK_TEST_", "DM_NVN_TEXDUMP", "DM_NVN_SHOT", "DM_NVN_TSV", "DM_KEYS", "DM_CTL", "DM_WINDOW", "DM_METAL_HUD", "DM_PSO_PREBUILD"};
    for (size_t i = 0; i < n;) {
        const char *e = memchr(head + i, '\n', n - i); size_t ln = e ? (size_t)(e - head - i) : n - i;
        if (ln > 4 && !memcmp(head + i, "env\t", 4)) {
            char kv[4096]; size_t l = CAP_MIN(ln - 4, sizeof kv - 1); memcpy(kv, head + i + 4, l); kv[l] = 0;
            char *eq = strchr(kv, '='); if (eq) { *eq = 0; int sk = 0; for (unsigned k = 0; k < sizeof skip / sizeof *skip; k++) sk |= !strncmp(kv, skip[k], strlen(skip[k]));
                if (!sk) setenv(kv, eq + 1, 0); }
        }
        i += ln + 1;
    }
}

static void map_region(const cap_regn *h,const uint8_t *bytes) {
    if (h->index>=1u<<20 || !h->size || h->size>=1ull<<CAP_OFF_BITS || h->old_lo>UINT64_MAX-h->size)
        RFAIL("invalid region %u",h->index);
    if (h->index>=rp_nrg) {
        rp_rg=realloc(rp_rg,(h->index+1)*sizeof *rp_rg);
        memset(rp_rg+rp_nrg,0,(h->index+1-rp_nrg)*sizeof *rp_rg);rp_nrg=h->index+1;
    }
    if (rp_rg[h->index].mem) RFAIL("duplicate region %u",h->index);
    // A non-fixed hint can retain addresses embedded inside shader buffers. It
    // never replaces mappings belonging to the executable, stack or driver.
    size_t page=(size_t)getpagesize(),lead=h->old_lo&(page-1);
    size_t len=(h->size+lead+page-1)&~(page-1);
    uint8_t *base=mmap((void *)(uintptr_t)(h->old_lo-lead),len,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if (base!=MAP_FAILED && (uintptr_t)base+lead==h->old_lo) base+=lead;
    else {
        if (base!=MAP_FAILED) munmap(base,len);
        lead=h->old_lo&0xffff;len=h->size+lead+0x30000;
        uint8_t *m=mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
        if (m==MAP_FAILED) RFAIL("cannot map region %u",h->index);
        base=(uint8_t *)(((uintptr_t)m+0xffff)&~(uintptr_t)0xffff)+lead;
    }
    memcpy(base,bytes,h->size);rp_rg[h->index]=(region){h->old_lo,h->size,h->kind,base};
}

static void add_pools(const uint8_t *data,size_t size) {
    static nvn_hmap seen={.klen=8};
    for (size_t off=0;off<size;off+=sizeof(cap_padd)) {
        cap_padd p;memcpy(&p,data+off,sizeof p);
        if (!dec_addr(NULL,&p.mem,p.size,0) || !p.mem) RFAIL("pool outside captured regions");
        if (nvn_hm_get(&seen,&p.mem)) continue;
        nvn_hm_put(&seen,&p.mem,(void *)1);
        mtl_pool_add((void *)(uintptr_t)p.mem,p.size);mtl_pool_addressed((void *)(uintptr_t)p.mem,p.size);
        nvn_dirty_add_range((void *)(uintptr_t)p.mem,p.size);
    }
}

static void fill_tex(const uint8_t *data,size_t size) {
    cap_tpix h;
    if (size<sizeof h) RFAIL("short texture chunk");
    memcpy(&h,data,sizeof h);
    if (!h.nsub || h.nsub>(size-sizeof h)/sizeof(cap_tsub) || h.samples!=1) RFAIL("invalid texture subresources");
    cap_tex_fields(&h.t,&rp_dec,NULL);
    if (rp_bad) RFAIL("texture references an absent object");
    mtl_sub_begin();vtex *t=tex_get(&h.t,1);
    if (!t) RFAIL("cannot create texture %llu",(unsigned long long)h.t.serial);
    if (!mtl_wait(mtl_sub_commit(),UINT64_MAX)) RFAIL("texture creation did not finish");
    uint32_t planes=t->aspect==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT) ? 2 : 1;
    uint32_t type=t->target==NVN_TEXTURE_TARGET_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    if (h.pixfmt!=t->f.f || h.type!=type || h.width!=t->w || h.height!=t->h || h.depth!=t->d ||
        h.levels!=t->levels || h.slices!=t->layers || h.samples!=t->samples ||
        h.nsub!=(uint64_t)planes*t->levels*t->layers) RFAIL("texture creation differs from capture");
    size_t offset=sizeof h+(size_t)h.nsub*sizeof(cap_tsub);
    for (uint32_t i=0;i<h.nsub;i++) {
        cap_tsub s;memcpy(&s,data+sizeof h+(size_t)i*sizeof s,sizeof s);
        // Capture orders planes, then levels, then slices. Validate complete
        // coverage as well as the exact packed sizes before the driver sees it.
        uint32_t plane=i/(t->levels*t->layers),level=i/t->layers%t->levels,slice=i%t->layers;
        uint32_t w=CAP_MAX(t->w>>level,1u),ht=CAP_MAX(t->h>>level,1u);
        uint32_t d=type==VK_IMAGE_TYPE_3D ? CAP_MAX(t->d>>level,1u) : 1;
        int bw=1,bh=1,bpp=rt_bpp(t->f.f);VkImageAspectFlags aspect=t->aspect;
        if (plane) { bpp=1;aspect=VK_IMAGE_ASPECT_STENCIL_BIT; }
        else if (aspect&VK_IMAGE_ASPECT_DEPTH_BIT) {
            aspect=VK_IMAGE_ASPECT_DEPTH_BIT;if (t->f.f==VK_FORMAT_D16_UNORM_S8_UINT) bpp=2;
        }
        if (!bpp) bpp=nvn_fmt_bpb(t->nvn_fmt,&bw,&bh);
        uint64_t row=((uint64_t)w+bw-1)/bw*bpp,img=row*((ht+bh-1)/bh);
        if (!bpp || s.level!=level || s.slice!=slice || s.plane!=plane || s.w!=w || s.h!=ht || s.d!=d ||
            s.row!=row || s.img!=img || s.bytes!=img*d || s.bytes>size-offset) RFAIL("invalid texture plane %u",i);
        VkDeviceMemory memory;void *mapped;
        VkBuffer buffer=buf_new(s.bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,&memory,&mapped);
        memcpy(mapped,data+offset,s.bytes);
        VkBufferImageCopy copy={.imageSubresource={aspect,level,slice,1},.imageExtent={w,ht,d}};
        VkCommandBuffer command=once_begin();barrier_all(command);
        vkCmdCopyBufferToImage(command,buffer,t->img,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        barrier_all(command);once_end(command);
        vkDestroyBuffer(g_dev,buffer,NULL);vkFreeMemory(g_dev,memory,NULL);offset+=s.bytes;
    }
    if (offset!=size) RFAIL("extra texture bytes");
    t->gpu=1;
}

static void check_programs(void) {
    for (uint32_t i=0;i<rp_npr;i++) {
        uint64_t now[6];cap_iface(&rp_pr[i],now);
        if (memcmp(now,rp_prc[i].iface,sizeof now)) RFAIL("program %u shader interface differs; select matching DM_VK_DIR products",i);
        for (unsigned stage=0;stage<rp_pr[i].nstage;stage++) {
            uint8_t key[8];if (!prog_key(&rp_pr[i],stage,key)) continue;
            shader_t *s=shader_get(key);if (!s) continue;
            for (int b=0;b<s->nb;b++) if (s->b[b].kind==K_GMEM_MAP)
                for (uint32_t r=0;r<rp_nrg;r++) if (rp_rg[r].mem && (uintptr_t)rp_rg[r].mem!=rp_rg[r].old_lo)
                    RFAIL("raw-address shader requires region %u at its captured address, which is occupied",r);
        }
    }
}

static void fixed_chunks(void) {
    for (size_t i=0;i<rp_nch;i++) {
        cap_chunk *c=&rp_ch[i].c;size_t unit=0;
        switch (c->tag) {
            case CAP_PADD:unit=sizeof(cap_padd);break;
            case CAP_POBJ:unit=sizeof(cap_pobj);break;
            case CAP_TOBJ:unit=8+sizeof(tex_t);break;
            case CAP_PROG:unit=sizeof(cap_prog);break;
            case CAP_DPOL:unit=sizeof(cap_dpol);break;
            case CAP_TREG:unit=sizeof(cap_treg);break;
            case CAP_SREG:unit=sizeof(cap_sreg);break;
            case CAP_TKND:unit=sizeof(cap_tknd);break;
            case CAP_SYNC: if (c->raw!=4) RFAIL("invalid sync count");break;
        }
        if (unit && c->raw%unit) RFAIL("truncated object table");
    }
}

static void apply_chunk(chunk *chunk) {
    cap_chunk *c=&chunk->c;uint8_t *data=chunk->d;
    if (c->tag==CAP_PADD) add_pools(data,c->raw);
    else if (c->tag==CAP_PDIF) {
        for (size_t off=0;off<c->raw;) {
            uint32_t r;uint64_t start,len;
            if (c->raw-off<24) RFAIL("short region diff");
            memcpy(&r,data+off,4);memcpy(&start,data+off+8,8);memcpy(&len,data+off+16,8);
            if (r>=rp_nrg || !rp_rg[r].mem || start>rp_rg[r].size || len>rp_rg[r].size-start || len>c->raw-off-24)
                RFAIL("region diff out of bounds");
            memcpy(rp_rg[r].mem+start,data+off+24,len);off+=24+len;
        }
    } else if (c->tag==CAP_TREG) {
        for (size_t off=0;off<c->raw;off+=sizeof(cap_treg)) {
            cap_treg e;memcpy(&e,data+off,sizeof e);
            if (e.ok) cap_tex_fields(&e.t,&rp_dec,NULL);
            if (e.tid<0 || rp_bad) RFAIL("invalid texture registration");
            mtl_reg_tex(e.tid,e.ok ? &e.t : NULL,e.has_v ? &e.v : NULL);
            if (e.gone) RFAIL("retired Metal registry entry in a Vulkan capture");
        }
    } else if (c->tag==CAP_SREG) {
        for (size_t off=0;off<c->raw;off+=sizeof(cap_sreg)) {
            cap_sreg e;memcpy(&e,data+off,sizeof e);
            if (e.sid<0) RFAIL("invalid sampler registration");mtl_reg_samp(e.sid,&e.p);
        }
    } else if (c->tag==CAP_TKND) {
        for (size_t off=0;off<c->raw;off+=sizeof(cap_tknd)) {
            cap_tknd k;memcpy(&k,data+off,sizeof k);cap_tex_fields(&k.t,&rp_dec,NULL);
            if (rp_bad) RFAIL("invalid texture kind");
            if (k.kind&CAP_TK_LIVE) mtl_tex_init(&k.t);
            if (k.kind&CAP_TK_CPU) mtl_tex_cpu_written(&k.t);
        }
    } else if (c->tag==CAP_TPIX) fill_tex(data,c->raw);
}

static void replay_submit(uint32_t submit) {
    rp_nseg=0;uint32_t nh=0;const uint32_t *top=NULL;
    for (size_t i=0;i<rp_nch;i++) {
        cap_chunk *c=&rp_ch[i].c;uint8_t *d=rp_ch[i].d;if (c->submit!=submit) continue;
        if (c->tag==CAP_SEGM) {
            uint32_t idx,len;if (c->raw<8) RFAIL("short segment");memcpy(&idx,d,4);memcpy(&len,d+4,4);
            if (idx>=65536 || len!=c->raw-8) RFAIL("invalid segment");
            if (idx>=rp_nseg) {
                rp_seg=realloc(rp_seg,(idx+1)*sizeof *rp_seg);memset(rp_seg+rp_nseg,0,(idx+1-rp_nseg)*sizeof *rp_seg);rp_nseg=idx+1;
            }
            if (rp_seg[idx]) RFAIL("duplicate segment");
            rp_seg[idx]=calloc(1,(size_t)len+8);memcpy(rp_seg[idx],d+8,len);
        } else if (c->tag==CAP_SUBM) {
            if (top || c->raw<4) RFAIL("invalid submission");memcpy(&nh,d,4);
            if (nh>INT_MAX || c->raw!=4+(uint64_t)nh*4) RFAIL("invalid submission handles");
            top=(const uint32_t *)(d+4);
        }
    }
    if (!top) RFAIL("missing submission %u",submit);
    for (size_t i=0;i<rp_nch;i++) {
        cap_chunk *c=&rp_ch[i].c;uint8_t *d=rp_ch[i].d;if (c->submit!=submit || c->tag!=CAP_SEGM) continue;
        uint32_t idx,len;memcpy(&idx,d,4);memcpy(&len,d+4,4);
        for (uint32_t off=0;off<len;) {
            op_hdr h;if (len-off<8) RFAIL("short record");memcpy(&h,rp_seg[idx]+off,8);
            if (h.size<8 || h.size>len-off || !cap_rec_fields(h.op,rp_seg[idx]+off+8,h.size,&rp_dec,NULL) || rp_bad)
                RFAIL("submit %u segment %u: invalid record at %u",submit,idx,off);
            off+=h.size;
        }
    }
    uint64_t *handles=calloc(CAP_MAX(nh,1u),sizeof *handles);
    for (uint32_t i=0;i<nh;i++) {
        if (top[i]>=rp_nseg || !rp_seg[top[i]]) RFAIL("missing command segment");handles[i]=(uintptr_t)rp_seg[top[i]];
    }
    mtl_sub_begin();nvn_ft_poll();mtl_sub_index(handles,nh);
    for (uint32_t i=0;i<nh;i++) { mtl_exec_rest(handles+i+1,nh-i-1);mtl_exec((const uint8_t *)(uintptr_t)handles[i]); }
    mtl_exec_rest(NULL,0);
    if (!mtl_wait(mtl_sub_commit(),UINT64_MAX)) RFAIL("submission did not finish");
    free(handles);for (uint32_t i=0;i<rp_nseg;i++) { free(rp_seg[i]);rp_seg[i]=NULL; }
}

static void pool_hashes(void) {
    uint64_t bad=0,total=0;
    for (size_t i=0;i<rp_nch;i++) if (rp_ch[i].c.tag==CAP_PHSH) {
        const uint8_t *data=rp_ch[i].d;size_t size=rp_ch[i].c.raw;
        for (size_t off=0;off<size;) {
            uint32_t r,n;if (size-off<8) RFAIL("short pool hashes");memcpy(&r,data+off,4);memcpy(&n,data+off+4,4);off+=8;
            if (r>=rp_nrg || !rp_rg[r].mem || n!=(rp_rg[r].size+4095)/4096 || n>(size-off)/8) RFAIL("invalid pool hashes");
            for (uint32_t b=0;b<n;b++) {
                uint64_t expected;memcpy(&expected,data+off+(size_t)b*8,8);
                bad+=expected!=cap_hash_block(rp_rg[r].mem+(size_t)b*4096,CAP_MIN(4096,rp_rg[r].size-(size_t)b*4096));total++;
            }
            off+=(size_t)n*8;
        }
    }
    printf("Pool writeback: %llu/%llu blocks differ (reported separately from frame pixels)\n",(unsigned long long)bad,(unsigned long long)total);
}

int main(int argc,char **argv) {
    const char *path=NULL,*png=NULL;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--png") && i+1<argc) png=argv[++i];
        else if (argv[i][0]!='-' && !path) path=argv[i];
        else RFAIL("usage: %s <capture> [--png path]",argv[0]);
    }
    if (!path) RFAIL("usage: %s <capture> [--png path]",argv[0]);
    setvbuf(stdout,NULL,_IOLBF,0);crash_install();nvn_serial_floor(1ull<<61);
    cap_reader reader;if (cap_load(&reader,path)<=0) RFAIL("cannot load %s",path);
    int end=0,complete=0;const char *head=NULL,*stat=NULL;size_t hn=0,sn=0;const cap_pres *pres=NULL;
    uint32_t nsub=0;int layout=0;
    for (;;) {
        cap_chunk c={0};uint8_t *d;int next=cap_next(&reader,&c,&d);
        if (next<0) RFAIL("corrupt archive");
        if (!next) { end=c.tag==CAP_END_ && !c.raw && !c.stored && reader.pos==reader.n;break; }
        rp_ch=realloc(rp_ch,(rp_nch+1)*sizeof *rp_ch);rp_ch[rp_nch++]=(chunk){c,d};
        if (c.submit>1000000) RFAIL("invalid submission index");
        if (c.tag==CAP_HEAD) { head=(char *)d;hn=c.raw; }
        if (c.tag==CAP_STAT) { stat=(char *)d;sn=c.raw; }
        if (c.tag==CAP_LAYT) layout=cap_layout_matches((char *)d,c.raw);
        if (c.tag==CAP_VK_RESULT) { if (c.raw!=4) RFAIL("invalid completion marker");memcpy(&complete,d,4); }
        if (c.tag==CAP_END_) end=1;
        if (c.tag==CAP_PRES) { if (c.raw!=sizeof *pres || pres) RFAIL("invalid present");pres=(cap_pres *)d; }
        if (c.tag==CAP_SUBM) nsub=CAP_MAX(nsub,c.submit);
    }
    cap_unload(&reader);
    if (!end || complete!=1 || !head || !layout || !pres || !nsub) RFAIL("incomplete capture or incompatible record layout");
    char value[128];
    if (!head_get(head,hn,"backend",value,sizeof value) || strcmp(value,"vulkan") ||
        !head_get(head,hn,"iface",value,sizeof value) || atoi(value)!=CAP_VK_IFACE) RFAIL("incompatible capture backend/interface");
    if (!head_get(head,hn,"title",value,sizeof value) || strcmp(value,DM_GAME_ID)) RFAIL("capture belongs to another game");
    if (head_get(head,hn,"ounce",value,sizeof value)) rp_ounce=atoi(value)!=0;
    apply_env(head,hn);unsetenv("DM_NVN_CAPTURE");fixed_chunks();
#define GROW(n,idx) do { if ((idx)>=1u<<20) RFAIL("object index too large"); (n)=CAP_MAX((n),(idx)+1); } while (0)
    for (size_t i=0;i<rp_nch;i++) {
        cap_chunk *c=&rp_ch[i].c;uint8_t *d=rp_ch[i].d;size_t unit=0;uint32_t *count=NULL;
        if (c->tag==CAP_REGN) {
            cap_regn h;if (c->raw<sizeof h) RFAIL("short region");memcpy(&h,d,sizeof h);
            if (h.size!=c->raw-sizeof h) RFAIL("invalid region length");map_region(&h,d+sizeof h);
        }
        if (c->tag==CAP_POBJ) { unit=sizeof(cap_pobj);count=&rp_npo; }
        if (c->tag==CAP_TOBJ) { unit=8+sizeof(tex_t);count=&rp_nto; }
        if (c->tag==CAP_PROG) { unit=sizeof(cap_prog);count=&rp_npr; }
        if (c->tag==CAP_DPOL) { unit=sizeof(cap_dpol);count=&rp_ndp; }
        if (count) for (size_t off=0;off<c->raw;off+=unit) { uint32_t idx;memcpy(&idx,d+off,4);GROW(*count,idx); }
        if (c->tag==CAP_SYNC) { uint32_t count;memcpy(&count,d,4);if (count>1u<<20) RFAIL("too many syncs");rp_nsy=CAP_MAX(rp_nsy,count); }
    }
#undef GROW
    rp_po=calloc(rp_npo+1,sizeof *rp_po);rp_to=calloc(rp_nto+1,sizeof *rp_to);rp_pr=calloc(rp_npr+1,sizeof *rp_pr);
    rp_prc=calloc(rp_npr+1,sizeof *rp_prc);rp_dp=calloc(rp_ndp+1,sizeof *rp_dp);rp_sy=calloc(rp_nsy+1,sizeof *rp_sy);
    for (size_t i=0;i<rp_nch;i++) {
        cap_chunk *c=&rp_ch[i].c;uint8_t *d=rp_ch[i].d;
        if (c->tag==CAP_POBJ) for (size_t off=0;off<c->raw;off+=sizeof(cap_pobj)) {
            cap_pobj p;memcpy(&p,d+off,sizeof p);dec_addr(NULL,&p.mem,p.size,0);
            if (p.vmode==2) RFAIL("sparse pool replay requires captured page mappings");
            rp_po[p.index]=(pool_t){.magic=MAGIC('P','O','O','L'),.flags=p.flags,.mem=(uint8_t *)(uintptr_t)p.mem,.size=p.size,.vmode=p.vmode};
        }
        if (c->tag==CAP_TOBJ) for (size_t off=0;off<c->raw;off+=8+sizeof(tex_t)) { uint32_t idx;memcpy(&idx,d+off,4);memcpy(&rp_to[idx],d+off+8,sizeof(tex_t)); }
        if (c->tag==CAP_PROG) for (size_t off=0;off<c->raw;off+=sizeof(cap_prog)) {
            cap_prog p;memcpy(&p,d+off,sizeof p);if (p.nstage>6) RFAIL("invalid program");
            rp_prc[p.index]=p;rp_pr[p.index]=(prog_t){.magic=MAGIC('P','R','O','G'),.nstage=p.nstage,.serial=p.serial};
            replay_program(&rp_pr[p.index],&p);
        }
        if (c->tag==CAP_DPOL) for (size_t off=0;off<c->raw;off+=sizeof(cap_dpol)) {
            cap_dpol p;memcpy(&p,d+off,sizeof p);dec_obj(NULL,CAP_K_POOL,&p.pool,NULL,0);
            rp_dp[p.index]=(dpool_t){(const pool_t *)(uintptr_t)p.pool,p.off,p.n};
            if (!replay_dpool(&rp_dp[p.index],&p)) RFAIL("too many descriptor pools");
        }
    }
    for (uint32_t i=0;i<rp_nto;i++) cap_tex_fields(&rp_to[i],&rp_dec,NULL);
    if (rp_bad) RFAIL("captured pointers name absent objects or regions");
    mtl_init();mtl_queue_activate(1);
    if (head_get(stat,sn,"tex_pool_n",value,sizeof value)) mtl_set_tex_pool_n(atoi(value));
    if (head_get(stat,sn,"origin_ll",value,sizeof value)) mtl_set_origin_lower_left(atoi(value));
    if (head_get(stat,sn,"depth_zero",value,sizeof value)) mtl_set_depth_zero(atoi(value));
    int qll=head_get(stat,sn,"queue_origin_ll",value,sizeof value) ? atoi(value) : 1;
    check_programs();
    for (uint32_t submit=0;submit<=nsub;submit++) {
        for (size_t i=0;i<rp_nch;i++) if (rp_ch[i].c.submit==submit) apply_chunk(&rp_ch[i]);
        if (submit) { mtl_set_queue_origin_lower_left(qll);mtl_set_frame(pres->frame);replay_submit(submit); }
    }
    uint64_t ptr=pres->tex;if (!dec_obj(NULL,CAP_K_TEX,&ptr,NULL,0) || !ptr) RFAIL("missing presented texture");
    uint8_t digest[32];uint64_t bytes;uint32_t w,h,format;cap_buf pixels={0};
    if (!cap_digest((const tex_t *)(uintptr_t)ptr,digest,&bytes,&w,&h,&format,png,&pixels)) RFAIL("cannot read presented texture");
    int same=bytes==pres->bytes && w==pres->width && h==pres->height && format==pres->pixfmt && !memcmp(digest,pres->sha256,32);
    printf("Vulkan replay: %s; %u submissions, %u textures, %ux%u, %llu bytes\n",same ? "exact frame match" : "FRAME MISMATCH",nsub,rp_nto,w,h,(unsigned long long)bytes);
    pool_hashes();cap_buf_free(&pixels);
    return same ? 0 : 1;
}
