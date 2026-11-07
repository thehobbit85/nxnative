// Live Vulkan capture. The relocation, interning and pool-diff algorithm comes
// from ../nvn_capture.inc. Vulkan reflection, resources and host metadata stay
// here so the Apple implementation keeps its own platform interfaces.
#include "../nvn_capture.h"
#include "../nvn_hmap.h"
#include "../dmsh_chain.h"
#include "../../../android/compat/CommonCrypto/CommonDigest.h"
#include "capture_format.h"
#define CAP_MIN(a,b) ((a)<(b) ? (a) : (b))
#define CAP_MAX(a,b) ((a)>(b) ? (a) : (b))
extern int nvn_host_readable(uint64_t address,uint64_t size);

static int g_cap_frame = -1;            // -1: not parsed yet, 0: off
static char g_cap_path[1024];
static int g_cap_active, g_cap_done, g_cap_failed;
static uint32_t g_cap_submit;           // submits captured in the frame so far
static uint64_t g_cap_presents;         // presents seen (the frame number DM_NVN_SHOT uses)
static cap_writer g_cap_w;
static uint64_t g_cap_t0, g_cap_ns, g_cap_unmapped, g_cap_segs, g_cap_tex_bytes, g_cap_tex_n, g_cap_tex_skip;
typedef struct { uint64_t lo, size; uint32_t kind, pending; uint64_t *hash; } cap_region;   // hash: one per 4 KiB block, for the changes before later submits; pending: bytes not written yet
static cap_region *g_cap_reg; static uint32_t g_cap_nreg;
static nvn_hmap g_cap_tex = {.klen = sizeof(tex_t)}, g_cap_prog = {.klen = 16}, g_cap_dpol = {.klen = 8 + sizeof(dpool_t)}, g_cap_sync = {.klen = 8}, g_cap_pool = {.klen = 8 + sizeof(pool_t)};
static nvn_hmap g_cap_seg = {.klen = 8}, g_cap_read = {.klen = 8};
static int g_cap_no_read;   // at the present: the frame is rendered, nothing may be read back as an input any more
static uint32_t g_cap_ntex, g_cap_nprog, g_cap_ndpol, g_cap_nsync, g_cap_npool;
static cap_buf g_cap_tobj, g_cap_pobj, g_cap_progb, g_cap_dpolb, g_cap_kind;   // objects interned since the last flush
static treg_t *g_cap_treg0; static int g_cap_ntreg0; static samp_par *g_cap_sreg0; static uint8_t *g_cap_sreg0_ok; static int g_cap_nsreg0;   // the registries as last written

static int nvn_cap_enabled(void) {
    if (g_cap_frame >= 0) return g_cap_frame > 0 && !g_cap_done;
    g_cap_frame = 0;
    const char *e = getenv("DM_NVN_CAPTURE"); int f = 0, n = 0, used = 0;
    if (!e || !*e) return 0;
    if (sscanf(e, "%d:%d:%n", &f, &n, &used) < 2 || !used || f < 1 || !e[used]) { LOG("[RED] DM_NVN_CAPTURE=%s: expected <frame>:1:<path>; capture off", e); return 0; }
    if (n != 1) LOG("[RED] DM_NVN_CAPTURE: %d frames asked, step 1 captures one; capturing frame %d only", n, f);
    if (strlen(e + used) >= sizeof g_cap_path) { LOG("[RED] DM_NVN_CAPTURE: path is too long"); return 0; }
    snprintf(g_cap_path, sizeof g_cap_path, "%s", e + used); g_cap_frame = f;
    LOG("DM_NVN_CAPTURE: capturing frame %d into %s", f, g_cap_path);
    return 1;
}

static uint64_t cap_hash_block(const uint8_t *p, size_t n) {   // a fast 64-bit hash of a 4 KiB block (only compared with itself between submits)
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n, v; size_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); h = (h ^ v) * 0xff51afd7ed558ccdull; h ^= h >> 32; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static void cap_fail(const char *why) { if (!g_cap_failed++) LOG("[RED] DM_NVN_CAPTURE: %s: the capture is incomplete", why); }

// ---------- Regions and the address encoding ----------
static int cap_reg_find(uint64_t a) {   // the region holding address a (regions are sorted and disjoint)
    int lo = 0, hi = (int)g_cap_nreg - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (a < g_cap_reg[m].lo) hi = m - 1; else if (a >= g_cap_reg[m].lo + g_cap_reg[m].size) lo = m + 1; else return m; }
    return -1;
}
static void cap_reg_hash(cap_region *r, const uint8_t *p) {   // p: the region's bytes (or the copy that was written)
    size_t nb = (size_t)((r->size + 4095) / 4096); r->hash = realloc(r->hash, nb * 8);
    for (size_t b = 0; b < nb; b++) { uint64_t o = (uint64_t)b * 4096; r->hash[b] = cap_hash_block(p + o, (size_t)CAP_MIN(4096, r->size - o)); }
}
// Regions are written once their index is fixed; a later region gets the next index, so the sorted order is kept in a separate index table
static uint32_t *g_cap_rid; static uint32_t g_cap_nrid;   // g_cap_rid[sorted position] = region number in the file
static int cap_reg_add(uint64_t lo, uint64_t size, uint32_t kind) {   // a new disjoint region (its bytes are written by cap_mem); returns its sorted position
    if (lo > UINT64_MAX-size || size >= (1ull << CAP_OFF_BITS) || g_cap_nrid >= (1u << CAP_REGION_BITS)) { cap_fail("region exceeds the capture address encoding"); return -1; }
    int pos = 0; while (pos < (int)g_cap_nreg && g_cap_reg[pos].lo < lo) pos++;
    if ((pos && g_cap_reg[pos-1].lo+g_cap_reg[pos-1].size>lo) || (pos<(int)g_cap_nreg && g_cap_reg[pos].lo<lo+size)) { cap_fail("capture regions overlap"); return -1; }
    g_cap_reg = realloc(g_cap_reg, (g_cap_nreg + 1) * sizeof *g_cap_reg); g_cap_rid = realloc(g_cap_rid, (g_cap_nreg + 1) * sizeof *g_cap_rid);
    memmove(g_cap_reg + pos + 1, g_cap_reg + pos, (g_cap_nreg - pos) * sizeof *g_cap_reg); memmove(g_cap_rid + pos + 1, g_cap_rid + pos, (g_cap_nreg - pos) * sizeof *g_cap_rid);
    g_cap_reg[pos] = (cap_region){lo, size, kind, 1, NULL}; g_cap_rid[pos] = g_cap_nrid++; g_cap_nreg++;
    cap_reg_hash(&g_cap_reg[pos], (const uint8_t *)(uintptr_t)lo);   // the bytes now: cap_mem counts what changed before they were written
    return pos;
}
static uint32_t g_cap_cur_submit;
static char g_cap_where[96]; static const uint8_t *g_cap_where_base;   // what is being encoded, for the red lines below (a record: its op, and the base its field offsets count from)
#define CAP_WHERE(base, ...) (snprintf(g_cap_where, sizeof g_cap_where, __VA_ARGS__), g_cap_where_base = (const uint8_t *)(base))
static const char *cap_where(const uint64_t *field) {
    static char b[128]; uint64_t off = (uint64_t)((const uint8_t *)field - g_cap_where_base);
    if (g_cap_where_base && off < (64u << 20)) snprintf(b, sizeof b, "%s, field +%llu", g_cap_where, (unsigned long long)off); else snprintf(b, sizeof b, "%s", g_cap_where);
    return b;
}
static int cap_enc_addr(void *ctx, uint64_t *field, uint64_t len, int writable) {
    (void)ctx; (void)writable;
    uint64_t v = *field; if (!v) return 1;
    int i = cap_reg_find(v);
    if (i < 0 && len && len <= (1u << 20) && nvn_host_readable(v, len)) {   // memory outside the pools a record reads (deferred handles, event words): a region of its own
        for (uint32_t k = 0; k < g_cap_nreg; k++) if (g_cap_reg[k].lo < v + len && v < g_cap_reg[k].lo + g_cap_reg[k].size) { cap_fail("a record's memory straddles a region");
            LOG("[RED] DM_NVN_CAPTURE: %#llx (%llu bytes) straddles region %#llx (%llu bytes): %s", (unsigned long long)v, (unsigned long long)len, (unsigned long long)g_cap_reg[k].lo, (unsigned long long)g_cap_reg[k].size, cap_where(field)); return 1; }
        i = cap_reg_add(v, len, 1);
    }
    if (i < 0) { cap_fail("a record contains an address outside captured memory"); if (g_cap_unmapped++ < 8) LOG("[RED] DM_NVN_CAPTURE: a record names %#llx (%llu bytes), outside every pool: kept as it is (the replay resolves it as the live run does, to nothing): %s", (unsigned long long)v, (unsigned long long)len, cap_where(field)); return 1; }
    if (len > g_cap_reg[i].size-(v-g_cap_reg[i].lo)) cap_fail("record range extends beyond its captured region");
    *field = cap_addr(g_cap_rid[i], v - g_cap_reg[i].lo);
    return 1;
}

// Vulkan reflection and image readback are defined in capture_resources.inc.
static void cap_iface(const prog_t *program, uint64_t out[6]);
// ---------- Objects ----------
static int cap_enc_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size);
static const cap_ops g_cap_enc = {cap_enc_addr, cap_enc_obj};
static void cap_tex_read(const tex_t *t, int rec);
static uint64_t cap_pool_obj(const pool_t *p) {
    if (!p) return 0;
    uint8_t k[8 + sizeof(pool_t)]; memcpy(k, &p, 8); memcpy(k + 8, p, sizeof *p);
    void *have = nvn_hm_get(&g_cap_pool, k); if (have) return cap_obj(CAP_K_POOL, (uint64_t)(uintptr_t)have - 1);
    uint32_t idx = g_cap_npool++; nvn_hm_put(&g_cap_pool, k, (void *)(uintptr_t)(idx + 1));
    cap_pobj o = {idx, p->flags, (uint64_t)(uintptr_t)p->mem, p->size, p->vmode, 0};
    if (p->vmode == 2) cap_fail("a sparse virtual pool (step 1 does not capture page tables)");
    cap_enc_addr(NULL, &o.mem, 0, 0); cap_buf_put(&g_cap_pobj, &o, sizeof o);
    return cap_obj(CAP_K_POOL, idx);
}
static void cap_tex_enc(tex_t *t, const char *what) {   // cap_tex_fields, labelled for the red lines (inside a record: the record's label follows)
    char sv[sizeof g_cap_where]; const uint8_t *sb = g_cap_where_base; memcpy(sv, g_cap_where, sizeof sv);
    CAP_WHERE(t, "%s ser=%llu%s%s", what, (unsigned long long)t->serial, sb ? " in " : "", sb ? sv : "");
    cap_tex_fields(t, &g_cap_enc, NULL);
    memcpy(g_cap_where, sv, sizeof sv); g_cap_where_base = sb;
}
static uint64_t cap_tex_obj(const tex_t *src) {   // a texture snapshot (by content), relocated
    tex_t t = *src;
    void *have = nvn_hm_get(&g_cap_tex, &t); if (have) return cap_obj(CAP_K_TEX, (uint64_t)(uintptr_t)have - 1);
    uint32_t idx = g_cap_ntex++; nvn_hm_put(&g_cap_tex, &t, (void *)(uintptr_t)(idx + 1));
    cap_tex_read(&t, 1);
    cap_tex_enc(&t, "texture object");
    uint32_t hd[2] = {idx, 0}; cap_buf_put(&g_cap_tobj, hd, 8); cap_buf_put(&g_cap_tobj, &t, sizeof t);
    return cap_obj(CAP_K_TEX, idx);
}
static const uint8_t *g_cap_orig;   // the record's payload in the game's memory (the copy is walked)
static uint64_t cap_seg_get(uint64_t addr);
static int cap_enc_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size) {
    (void)ctx; uint64_t v = *field; if (!v && kind != CAP_K_SEG) return 1;
    switch (kind) {
        case CAP_K_SEG: *field = cap_obj(CAP_K_SEG, cap_seg_get(v)); return 1;
        case CAP_K_POOL: *field = cap_pool_obj((const pool_t *)(uintptr_t)v); return 1;
        case CAP_K_TEX:
            if (g_cap_orig && v >= (uint64_t)(uintptr_t)g_cap_orig && v + sizeof(tex_t) <= (uint64_t)(uintptr_t)g_cap_orig + size) {   // the snapshot inside the record
                uint64_t off = v - (uint64_t)(uintptr_t)g_cap_orig; tex_t *in = (tex_t *)(payload + off);
                cap_tex_read(in, 1); cap_tex_enc(in, "texture snapshot"); *field = cap_obj(CAP_K_TEXIN, off); return 1;
            }
            *field = cap_tex_obj((const tex_t *)(uintptr_t)v); return 1;
        case CAP_K_PROG: {
            const prog_t *p = (const prog_t *)(uintptr_t)v; uint64_t k[2] = {v, p->serial};
            void *have = nvn_hm_get(&g_cap_prog, k); if (have) { *field = cap_obj(CAP_K_PROG, (uint64_t)(uintptr_t)have - 1); return 1; }
            uint32_t idx = g_cap_nprog++; nvn_hm_put(&g_cap_prog, k, (void *)(uintptr_t)(idx + 1));
            cap_prog o = {idx, p->nstage, p->serial, {{0}}, 0, {0}, {0}};
            for (unsigned s = 0; s < p->nstage && s < 6; s++) if (prog_key(p, s, o.key[s])) o.ok |= (uint8_t)(1u << s);
            cap_iface(p, o.iface);
            cap_buf_put(&g_cap_progb, &o, sizeof o); *field = cap_obj(CAP_K_PROG, idx); return 1;
        }
        case CAP_K_DPOOL: {
            const dpool_t *d = (const dpool_t *)(uintptr_t)v; uint8_t k[8 + sizeof(dpool_t)]; memcpy(k, &v, 8); memcpy(k + 8, d, sizeof *d);
            void *have = nvn_hm_get(&g_cap_dpol, k); if (have) { *field = cap_obj(CAP_K_DPOOL, (uint64_t)(uintptr_t)have - 1); return 1; }
            uint32_t idx = g_cap_ndpol++; nvn_hm_put(&g_cap_dpol, k, (void *)(uintptr_t)(idx + 1));
            cap_dpol o = {idx, d->n, cap_pool_obj(d->pool), d->off, nvn_dpool_base(d, 1), nvn_dpool_base(d, 0)};
            cap_buf_put(&g_cap_dpolb, &o, sizeof o); *field = cap_obj(CAP_K_DPOOL, idx); return 1;
        }
        case CAP_K_SYNC: {
            void *have = nvn_hm_get(&g_cap_sync, &v); uint32_t idx = have ? (uint32_t)(uintptr_t)have - 1 : g_cap_nsync++;
            if (!have) nvn_hm_put(&g_cap_sync, &v, (void *)(uintptr_t)(idx + 1));
            *field = cap_obj(CAP_K_SYNC, idx); return 1;
        }
    }
    return 0;
}

// ---------- Segments ----------
static uint64_t *g_cap_segq; static uint32_t g_cap_nseg, g_cap_segq_cap;
static uint64_t cap_seg_get(uint64_t addr) {   // a segment's index in this submit (queued to be copied)
    void *have = nvn_hm_get(&g_cap_seg, &addr); if (have) return (uint64_t)(uintptr_t)have - 1;
    uint32_t idx = g_cap_nseg++; nvn_hm_put(&g_cap_seg, &addr, (void *)(uintptr_t)(idx + 1));
    if (g_cap_nseg > g_cap_segq_cap) { g_cap_segq_cap = g_cap_segq_cap ? g_cap_segq_cap * 2 : 256; g_cap_segq = realloc(g_cap_segq, g_cap_segq_cap * 8); }
    g_cap_segq[idx] = addr; return idx;
}
static void cap_seg_write(uint32_t idx) {   // copy the records from the segment start to its END or JUMP, then encode them in the copy
    const uint8_t *p = (const uint8_t *)(uintptr_t)g_cap_segq[idx]; size_t n = 0;
    for (;;) {
        op_hdr h; memcpy(&h, p + n, 8);
        if (h.size < 8 || (h.size & 7) || h.size > (64u << 20) || h.op >= OP_COUNT_) { cap_fail("a corrupt record in a submitted segment"); LOG("[RED] DM_NVN_CAPTURE: segment %p offset %zu: op %u size %u", p, n, h.op, h.size); break; }
        n += h.size;
        if (h.op == OP_END || h.op == OP_JUMP) break;
        if (n > (256u << 20)) { cap_fail("a segment longer than 256 MiB without END"); break; }
    }
    uint8_t *b = malloc(8 + n); uint32_t hd[2] = {idx, (uint32_t)n}; memcpy(b, hd, 8); memcpy(b + 8, p, n);
    for (size_t o = 0; o < n;) {
        op_hdr h; memcpy(&h, b + 8 + o, 8);
        g_cap_orig = p + o + 8; CAP_WHERE(b + 8 + o + 8, "op %u (%u bytes) at +%zu of segment %u", h.op, h.size, o, idx);
        if (!cap_rec_fields(h.op, b + 8 + o + 8, h.size, &g_cap_enc, NULL)) { cap_fail("a record the capture walk does not know"); LOG("[RED] DM_NVN_CAPTURE: op %u size %u not walked", h.op, h.size); }
        o += h.size;
    }
    g_cap_orig = NULL; CAP_WHERE(NULL, "-");
    cap_put(&g_cap_w, CAP_SEGM, g_cap_cur_submit, b, 8 + n, 1); free(b); g_cap_segs++;
}

#include "capture_resources.inc"

// ---------- Registries ----------
static void cap_registries(uint32_t submit) {   // the entries that differ from the last written ones (all of them at submit 1)
    cap_buf tb = {0}, sb = {0}; tex_t *reads = NULL; int nreads = 0;
    pthread_mutex_lock(&g_rmu);
    if (g_cap_ntreg0 < g_ntreg) { g_cap_treg0 = realloc(g_cap_treg0, g_ntreg * sizeof *g_cap_treg0); memset(g_cap_treg0 + g_cap_ntreg0, 0, (g_ntreg - g_cap_ntreg0) * sizeof *g_cap_treg0); g_cap_ntreg0 = g_ntreg; }
    reads = malloc((size_t)CAP_MAX(g_ntreg, 1) * sizeof *reads);
    for (int i = 0; i < g_ntreg; i++) {
        treg_t *r = &g_treg[i], *o = &g_cap_treg0[i];
        if (r->ok == o->ok && (!r->ok || (r->has_v == o->has_v && !memcmp(&r->t, &o->t, sizeof r->t) && (!r->has_v || !memcmp(&r->v, &o->v, sizeof r->v))))) continue;
        cap_treg e = {i, (uint8_t)r->ok, (uint8_t)r->has_v, 0, 0, r->t, r->v}; o->ok = r->ok; o->has_v = r->has_v; o->t = r->t; o->v = r->v;
        if (r->ok) reads[nreads++] = r->t;
        cap_buf_put(&tb, &e, sizeof e);
    }
    if (g_cap_nsreg0 < g_nsreg) { g_cap_sreg0 = realloc(g_cap_sreg0, g_nsreg * sizeof *g_cap_sreg0); g_cap_sreg0_ok = realloc(g_cap_sreg0_ok, g_nsreg); memset(g_cap_sreg0_ok + g_cap_nsreg0, 0, g_nsreg - g_cap_nsreg0); g_cap_nsreg0 = g_nsreg; }
    for (int i = 0; i < g_nsreg; i++) {
        if (!g_sreg_ok[i] || (g_cap_sreg0_ok[i] && !memcmp(&g_sreg[i], &g_cap_sreg0[i], sizeof g_sreg[i]))) continue;
        cap_sreg e = {i, 0, g_sreg[i]}; g_cap_sreg0[i] = g_sreg[i]; g_cap_sreg0_ok[i] = 1; cap_buf_put(&sb, &e, sizeof e);
    }
    pthread_mutex_unlock(&g_rmu);
    for (size_t o = 0; o < tb.n; o += sizeof(cap_treg)) { cap_treg *e = (cap_treg *)(tb.p + o); if (e->ok) { char w[32]; snprintf(w, sizeof w, "texture registry entry %d", e->tid); cap_tex_enc(&e->t, w); } }
    for (int i = 0; i < nreads; i++) cap_tex_read(&reads[i], 0);   // outside the registry lock: q_cb and the readback take other locks
    free(reads);
    if (tb.n) cap_put(&g_cap_w, CAP_TREG, submit, tb.p, tb.n, 1);
    if (sb.n) cap_put(&g_cap_w, CAP_SREG, submit, sb.p, sb.n, 1);
    cap_buf_free(&tb); cap_buf_free(&sb);
}

// ---------- The frame ----------
#include "capture_memory.inc"

// The pool bytes, last in a submit's step: the texture readbacks and the records before it take seconds, the game's other threads run meanwhile, and the live replay of the
// submit reads the pools as they are once the step ends. A new region is written whole (its bytes once, as they are then); of the others, the 4 KiB blocks that changed since
// they were last written. The bytes of new regions that changed after the region was found are counted in the log (0 in Breath of the Wild's and Pokemon Scarlet's frames)
static uint64_t cap_reg_write(uint32_t submit) {   // the new regions, whole; → their bytes that changed after they were found (what a copy at that point would have missed)
    uint64_t moved = 0;
    for (uint32_t i = 0; i < g_cap_nreg; i++) {
        cap_region *r = &g_cap_reg[i]; if (!r->pending) continue;
        size_t nb = (size_t)((r->size + 4095) / 4096);
        for (size_t k = 0; k < nb; k++) { uint64_t o = (uint64_t)k * 4096; size_t len = (size_t)CAP_MIN(4096, r->size - o); if (cap_hash_block((const uint8_t *)(uintptr_t)r->lo + o, len) != r->hash[k]) moved += len; }
        cap_regn hh = {g_cap_rid[i], r->kind, r->lo, r->size}; r->pending = 0;
        uint8_t *b = malloc(sizeof hh + r->size); if (!b) { cap_fail("out of memory for a region"); continue; }
        memcpy(b, &hh, sizeof hh); memcpy(b + sizeof hh, (const void *)(uintptr_t)r->lo, (size_t)r->size);
        cap_reg_hash(r, b + sizeof hh);   // of the copy: a write after it is a change for the next diff
        cap_put(&g_cap_w, CAP_REGN, submit, b, sizeof hh + r->size, 1); free(b);
    }
    return moved;
}
static void cap_mem(uint32_t submit) {
    uint64_t moved = cap_reg_write(submit);
    if (moved) LOG("DM_NVN_CAPTURE: submit %u: %llu bytes of the pools changed while it was captured (written as they are at its end)", submit, (unsigned long long)moved);
    cap_buf b = {0};
    for (uint32_t i = 0; i < g_cap_nreg; i++) {
        cap_region *r = &g_cap_reg[i]; size_t nb = (size_t)((r->size + 4095) / 4096);
        for (size_t k = 0; k < nb;) {
            uint64_t o = (uint64_t)k * 4096; size_t len = (size_t)CAP_MIN(4096, r->size - o);
            uint64_t h = cap_hash_block((const uint8_t *)(uintptr_t)r->lo + o, len);
            if (h == r->hash[k]) { k++; continue; }
            size_t k1 = k; uint64_t end = o;
            while (k1 < nb) { uint64_t o1 = (uint64_t)k1 * 4096; size_t l1 = (size_t)CAP_MIN(4096, r->size - o1); uint64_t h1 = cap_hash_block((const uint8_t *)(uintptr_t)r->lo + o1, l1);
                if (h1 == r->hash[k1]) break; r->hash[k1] = h1; end = o1 + l1; k1++; }
            struct { uint32_t region, pad; uint64_t off, len; } d = {g_cap_rid[i], 0, o, end - o};
            cap_buf_put(&b, &d, sizeof d); cap_buf_put(&b, (const uint8_t *)(uintptr_t)r->lo + o, (size_t)(end - o));
            k = k1;
        }
    }
    if (b.n) cap_put(&g_cap_w, CAP_PDIF, submit, b.p, b.n, 1);
    cap_buf_free(&b);
}
static void cap_flush_objs(uint32_t submit) {
    if (g_cap_pobj.n) cap_put(&g_cap_w, CAP_POBJ, submit, g_cap_pobj.p, g_cap_pobj.n, 1);
    if (g_cap_tobj.n) cap_put(&g_cap_w, CAP_TOBJ, submit, g_cap_tobj.p, g_cap_tobj.n, 1);
    if (g_cap_progb.n) cap_put(&g_cap_w, CAP_PROG, submit, g_cap_progb.p, g_cap_progb.n, 1);
    if (g_cap_dpolb.n) cap_put(&g_cap_w, CAP_DPOL, submit, g_cap_dpolb.p, g_cap_dpolb.n, 1);
    if (g_cap_kind.n) cap_put(&g_cap_w, CAP_TKND, submit, g_cap_kind.p, g_cap_kind.n, 1);
    g_cap_pobj.n = g_cap_tobj.n = g_cap_progb.n = g_cap_dpolb.n = g_cap_kind.n = 0;
}
static void cap_submit_step(int n, const uint64_t *h);
static void cap_present_step(const tex_t *t, const int crop[4]);
static void cap_release(void) {
    for (uint32_t i=0;i<g_cap_nreg;i++) free(g_cap_reg[i].hash);
    free(g_cap_reg);free(g_cap_rid);free(g_cap_segq);
    free(g_cap_treg0);free(g_cap_sreg0);free(g_cap_sreg0_ok);
    nvn_hmap *maps[]={&g_cap_tex,&g_cap_prog,&g_cap_dpol,&g_cap_sync,&g_cap_pool,&g_cap_seg,&g_cap_read};
    for (unsigned i=0;i<sizeof maps/sizeof *maps;i++) {
        free(maps[i]->keys);free(maps[i]->vals);
        maps[i]->keys=NULL;maps[i]->vals=NULL;maps[i]->n=maps[i]->cap=0;
    }
    cap_buf_free(&g_cap_tobj);cap_buf_free(&g_cap_pobj);cap_buf_free(&g_cap_progb);
    cap_buf_free(&g_cap_dpolb);cap_buf_free(&g_cap_kind);
}
// Hook: nvnQueueSubmitCommands, under the submit lock, before the submission is replayed
static void capture_submit(int n, const uint64_t *h) {
    if (!nvn_cap_enabled() || g_cap_presents + 1 != (uint64_t)g_cap_frame) return;
    cap_submit_step(n, h);
}
static void cap_submit_step(int n, const uint64_t *h) {
    uint64_t t0 = now_ns();
    mtl_finish();   // what the previous submissions wrote is in memory now: the pools and textures are read as this submit will see them
    g_cap_cur_submit = ++g_cap_submit;
    if (!g_cap_active) {
        if (!cap_open(&g_cap_w, g_cap_path)) { LOG("[RED] DM_NVN_CAPTURE: cannot write %s: capture off", g_cap_path); g_cap_done = 1; return; }
        g_cap_active = 1; g_cap_t0 = t0;
        cap_head();
    }
    cap_pools(g_cap_cur_submit);
    uint64_t t1 = now_ns(); cap_registries(g_cap_cur_submit);
    uint64_t t2 = now_ns(); nvn_hm_clear(&g_cap_seg, NULL); g_cap_nseg = 0;
    uint32_t *top = malloc((size_t)CAP_MAX(n, 1) * 4);
    for (int i = 0; i < n; i++) top[i] = (uint32_t)cap_seg_get(h[i]);
    for (uint32_t i = 0; i < g_cap_nseg; i++) cap_seg_write(i);   // segments called or jumped to are queued while the earlier ones are written
    cap_flush_objs(g_cap_cur_submit);
    uint64_t t3 = now_ns(); cap_mem(g_cap_cur_submit);
    LOG("DM_NVN_CAPTURE: submit %u: registries and their textures %.1f s, records and their textures %.1f s, pool bytes %.1f s", g_cap_cur_submit, (t2 - t1) / 1e9, (t3 - t2) / 1e9, (now_ns() - t3) / 1e9);
    if (g_cap_nsync) { uint32_t ns = g_cap_nsync; cap_put(&g_cap_w, CAP_SYNC, g_cap_cur_submit, &ns, 4, 0); }
    cap_buf s = {0}; uint32_t nn = (uint32_t)n; cap_buf_put(&s, &nn, 4); cap_buf_put(&s, top, (size_t)n * 4);
    cap_put(&g_cap_w, CAP_SUBM, g_cap_cur_submit, s.p, s.n, 0); cap_buf_free(&s); free(top);
    g_cap_ns += now_ns() - t0;
}

static void cap_hex(const uint8_t *md, char out[65]) { for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", md[i]); }

// Hook: nvnQueuePresentTexture, before the present is queued
static void capture_present(const tex_t *t, const int crop[4]) {
    if (!nvn_cap_enabled()) return;
    g_cap_presents++;
    if (!g_cap_active || g_cap_presents != (uint64_t)g_cap_frame) {
        if (g_cap_presents >= (uint64_t)g_cap_frame && !g_cap_active) { LOG("[RED] DM_NVN_CAPTURE: frame %d presented without a submit: nothing captured", g_cap_frame); g_cap_done = 1; }
        return;
    }
    cap_present_step(t, crop);
}
static void cap_present_step(const tex_t *t, const int crop[4]) {
    uint64_t t0 = now_ns();
    cap_pres p = {0}; cap_buf px = {0}; char png[1100]; snprintf(png, sizeof png, "%s.live.png", g_cap_path);
    g_cap_cur_submit = g_cap_submit + 1; g_cap_no_read = 1;
    if (t) { p.tex = cap_tex_obj(t); cap_flush_objs(g_cap_cur_submit); cap_reg_write(g_cap_cur_submit); }
    if (crop) memcpy(p.crop, crop, sizeof p.crop);
    p.frame = (uint32_t)g_cap_frame;
    if (!t || !cap_digest(t, p.sha256, &p.bytes, &p.width, &p.height, &p.pixfmt, png, &px)) cap_fail("the presented texture could not be read back");
    g_cap_ns += now_ns() - t0; p.capture_ms = g_cap_ns / 1e6;
    cap_put(&g_cap_w, CAP_PRES, g_cap_cur_submit, &p, sizeof p, 0);
    if (px.n) cap_put(&g_cap_w, CAP_LPIX, g_cap_cur_submit, px.p, px.n, 1);
    cap_buf_free(&px);
    {   // the pools as the frame left them (after the GPU finished): the replayer compares its own, which tells pool writes it did not make (the CPU writing during the frame) from ones it made differently
        cap_buf hb = {0};
        for (uint32_t i = 0; i < g_cap_nreg; i++) {
            cap_region *r = &g_cap_reg[i]; uint32_t nb = (uint32_t)((r->size + 4095) / 4096), hd[2] = {g_cap_rid[i], nb}; cap_buf_put(&hb, hd, 8);
            for (uint32_t k = 0; k < nb; k++) { uint64_t o = (uint64_t)k * 4096, h = cap_hash_block((const uint8_t *)(uintptr_t)r->lo + o, (size_t)CAP_MIN(4096, r->size - o)); cap_buf_put(&hb, &h, 8); }
        }
        cap_put(&g_cap_w, CAP_PHSH, g_cap_cur_submit, hb.p, hb.n, 1); cap_buf_free(&hb);
    }
    uint32_t success = !g_cap_failed;
    cap_put(&g_cap_w, CAP_VK_RESULT, g_cap_cur_submit, &success, sizeof success, 0);
    uint64_t raw = g_cap_w.raw, stored = g_cap_w.stored;
    int ok = cap_close(&g_cap_w); char hex[65]; cap_hex(p.sha256, hex);
    LOG("%sDM_NVN_CAPTURE: frame %d: %u submits, %llu segments, %u regions, %u textures (%llu read back, %.1f MB; %llu not captured), %llu addresses outside the pools; "
         "%.1f MB (%.1f MB raw) in %.0f ms → %s%s; live digest %s (%ux%u format %u)",
         g_cap_failed || !ok ? "[RED] " : "", g_cap_frame, g_cap_submit, (unsigned long long)g_cap_segs, g_cap_nreg, g_cap_ntex, (unsigned long long)g_cap_tex_n, g_cap_tex_bytes / 1048576.0,
         (unsigned long long)g_cap_tex_skip, (unsigned long long)g_cap_unmapped, stored / 1048576.0, raw / 1048576.0, p.capture_ms, g_cap_path, ok ? "" : " (write failed)", hex, p.width, p.height, p.pixfmt);
    g_cap_active = 0; g_cap_done = 1; cap_release();
}

// Submit and present can be called by different guest threads.
static pthread_mutex_t g_capture_mu=PTHREAD_MUTEX_INITIALIZER;
void nvn_cap_submit(int n,const uint64_t *handles) {
    pthread_mutex_lock(&g_capture_mu);
    capture_submit(n,handles);
    pthread_mutex_unlock(&g_capture_mu);
}
void nvn_cap_present(const tex_t *texture,const int crop[4]) {
    pthread_mutex_lock(&g_capture_mu);
    capture_present(texture,crop);
    pthread_mutex_unlock(&g_capture_mu);
}

#undef CAP_MIN
#undef CAP_MAX
