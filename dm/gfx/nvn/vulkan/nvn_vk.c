// NVN Vulkan graphics backend (offscreen or an Android window): the nvn_mtl.h interface the NVN layer calls (on Apple it is nvn_mtl.m), replaying each submission's command list into a Vulkan command buffer.
// Shaders are the SPIR-V the Metal chain translates (dm/android/tools/spv_android.py: <key>.spv + <key>.vk.txt in DM_VK_DIR), every resource in set 0 with a binding of its own; here the vertex
// stage keeps set 0 and the fragment stage is moved to set 1 (its DescriptorSet decorations rewritten at load), so one pipeline layout holds both.
// Memory: Vulkan cannot use the game's memory pools in place (no VK_EXT_external_memory_host on Mali), so what a draw reads from them -- uniform buffers, vertex and index data, the shader's
// own constants -- is copied into the submission's ring buffer when the draw is recorded. GPU-written ranges, copies, fills and inline updates retain ordered ring backing;
// later consumers compose overlapping writes on the GPU, and completion copies publish the final bytes to guest memory. Textures are Vulkan images: packaged ones
// (block-linear in the pool) are deswizzled and uploaded at first use; render targets, copies and clears stay on the GPU. Every image lives in VK_IMAGE_LAYOUT_GENERAL, and passes and copies
// are separated by full memory barriers: simple and correct first, faster later.
// Event values, counter reports and texture-to-buffer copies reach guest memory when the GPU has finished the submission (the completion thread), as on the Metal backend.
// With a window (the app, app/front_android.c) presented frames are scaled onto a swapchain, keeping the game's aspect unless DM_SCREEN_FIT=stretch; DM_SHOW_FPS=1 draws a frame-rate
// counter, DM_FPS=30 holds every frame two vsyncs. Without one (the adb front end) nothing is shown: DM_VK_SHOTS=<frame>,<frame>,... writes those frames to DM_VK_SHOT_DIR as BMP files.
#include "../nvn_dirty.h"
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_timestamp.h"
#include "render_condition.h"
#include "dmsh_abi.h"
#include "spv_vertex_fetch.h"
#include "spv_output_defaults.h"
#include "spv_alpha.h"
#include "spv_integer_outputs.h"
#include "spv_draw_parameters.h"
#include "spv_buffer_bias.h"
#include "spv_texel_bias.h"
#include "spv_wrapped_buffer.h"
#include "spv_descriptor_table.h"
#include "spv_one_row.h"
#include "spv_sample_reduction.h"
#include "feedback_gather.h"
#include "../nvn_geometry.h"
#include "uniform_address.h"
#include "present_rect.h"
#include "split_handle.h"
#include "driver_c2.h"
#include "driver_c0.h"
#include "../../tex/nvn_deswizzle.h"
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#endif
#include <vulkan/vulkan.h>
#include <math.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) die("Vulkan: %s failed (%d)", #x, (int)r_); } while (0)
#define RED_ONCE(id, ...) do { static int once_; if (!once_++) LOG("[RED] vk: " __VA_ARGS__); } while (0)

static VkInstance g_inst; static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf;
static VkPhysicalDeviceMemoryProperties g_memp; static VkPhysicalDeviceProperties g_props;
static uint64_t g_timestamp_mask;
static int g_segment_timestamps, g_cut_draws;
static PFN_vkGetCalibratedTimestampsEXT p_calibrated_timestamps;
static int g_draw_parameters, g_descriptor_tables, g_texel_descriptor_tables;
static int g_sampled_indexing,g_texel_indexing;
static int g_buffer_address;
static PFN_vkGetBufferDeviceAddress p_buffer_address;
static uint32_t g_vertex_divisor_limit = 1;
static VkPhysicalDeviceTexelBufferAlignmentProperties g_texel_limits;
static VkPhysicalDeviceSubgroupProperties g_subgroup;
static VkPhysicalDeviceSubgroupSizeControlProperties g_subgroup_sizes;
static VkShaderStageFlags g_selectable_subgroup_stages;
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;   // the queue (submissions, the present readback)
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;   // texture / sampler registrations and the image table (game threads register while the submit thread reads)

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if ((bits >> i & 1) && (g_memp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if (bits >> i & 1) return i;
    die("Vulkan: no memory type for %#x", bits);
}
static VkDeviceMemory mem_alloc(VkMemoryRequirements r, VkMemoryPropertyFlags want) {
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, want)};
    VkDeviceMemory m; VKCHECK(vkAllocateMemory(g_dev, &ai, NULL, &m)); return m;
}
static VkBuffer buf_new(VkDeviceSize n, VkBufferUsageFlags u, VkDeviceMemory *m, void **map) {
    if (g_buffer_address && (u&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) u|=VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b; VKCHECK(vkCreateBuffer(g_dev, &bi, NULL, &b));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(g_dev, b, &r);
    if (u&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkMemoryAllocateFlagsInfo flags={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,NULL,VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,0};
        VkMemoryAllocateInfo info={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,&flags,r.size,
            mem_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKCHECK(vkAllocateMemory(g_dev,&info,NULL,m));
    } else *m = mem_alloc(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkBindBufferMemory(g_dev, b, *m, 0));
    if (map) VKCHECK(vkMapMemory(g_dev, *m, 0, VK_WHOLE_SIZE, 0, map));
    return b;
}
#define ALL_BUF_USAGE (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | \
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)

static _Atomic uint64_t g_big_bytes, g_extra_ring_bytes;   // memory of the big images and of the extra rings now held (the frame stats line)
// ---------- Image memory: 64 MB blocks, each with a list of its free ranges (first fit; a freed range joins its neighbours; a block with no image left is freed) ----------
// Thousands of textures would exceed the allocation count (Vulkan promises only 4096). Blocks used to be handed out in order and freed only when their last image was: a game
// streaming its textures in and out kept every block alive with a few images in it, and Super Mario Odyssey's opening reached 4 GB in 64 blocks and was killed for memory
typedef struct { VkDeviceSize off, size; } mfree;
typedef struct { VkDeviceMemory m; VkDeviceSize size; int live; uint32_t type; mfree *fr; int nfr, cfr; } mblock;
enum { NBLK = 256 };
static mblock g_blk[NBLK]; static int g_nblk; static pthread_mutex_t g_blkmu = PTHREAD_MUTEX_INITIALIZER;   // images are made on the submit thread and freed on the completion thread
static void fr_put(mblock *b, VkDeviceSize off, VkDeviceSize size) {   // return a range, joined with the free ranges next to it (the list is kept sorted)
    int k = 0; while (k < b->nfr && b->fr[k].off < off) k++;
    if (k > 0 && b->fr[k - 1].off + b->fr[k - 1].size == off) { b->fr[k - 1].size += size; if (k < b->nfr && off + size == b->fr[k].off) { b->fr[k - 1].size += b->fr[k].size; memmove(b->fr + k, b->fr + k + 1, (b->nfr - k - 1) * sizeof *b->fr); b->nfr--; } return; }
    if (k < b->nfr && off + size == b->fr[k].off) { b->fr[k].off = off; b->fr[k].size += size; return; }
    if (b->nfr == b->cfr) { b->cfr = b->cfr ? 2 * b->cfr : 16; b->fr = realloc(b->fr, b->cfr * sizeof *b->fr); }
    memmove(b->fr + k + 1, b->fr + k, (b->nfr - k) * sizeof *b->fr); b->fr[k] = (mfree){off, size}; b->nfr++;
}
static int fr_take(mblock *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off) {   // first fit
    for (int k = 0; k < b->nfr; k++) {
        VkDeviceSize o = (b->fr[k].off + align - 1) / align * align, end = b->fr[k].off + b->fr[k].size;
        if (o + size > end) continue;
        VkDeviceSize lo = b->fr[k].off, hi = o + size;
        memmove(b->fr + k, b->fr + k + 1, (b->nfr - k - 1) * sizeof *b->fr); b->nfr--;
        if (o > lo) fr_put(b, lo, o - lo);
        if (end > hi) fr_put(b, hi, end - hi);
        *off = o; return 1;
    }
    return 0;
}
static int img_mem_(VkMemoryRequirements r, VkDeviceMemory *m, VkDeviceSize *off);
static int img_mem(VkMemoryRequirements r, VkDeviceMemory *m, VkDeviceSize *off) { pthread_mutex_lock(&g_blkmu); int b = img_mem_(r, m, off); pthread_mutex_unlock(&g_blkmu); return b; }
static int img_mem_(VkMemoryRequirements r, VkDeviceMemory *m, VkDeviceSize *off) {
    uint32_t type = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (r.size > (32u << 20)) { *m = mem_alloc(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); *off = 0; atomic_fetch_add(&g_big_bytes, r.size); return -1; }   // big images get memory of their own
    for (int i = 0; i < g_nblk; i++) { mblock *b = &g_blk[i]; if (b->m && b->type == type && fr_take(b, r.size, r.alignment, off)) { b->live++; *m = b->m; return i; } }
    int i = 0; while (i < g_nblk && g_blk[i].m) i++;
    if (i == NBLK) die("Vulkan: image memory blocks exhausted");
    if (i == g_nblk) g_nblk++;
    mblock *b = &g_blk[i];
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 64u << 20, type};
    VKCHECK(vkAllocateMemory(g_dev, &ai, NULL, &b->m));
    b->size = 64u << 20; b->live = 0; b->type = type; b->nfr = 0; fr_put(b, 0, b->size);
    if (!fr_take(b, r.size, r.alignment, off)) die("Vulkan: an image of %llu bytes does not fit a new memory block", (unsigned long long)r.size);
    b->live++; *m = b->m; return i;
}
static void img_mem_free(int blk, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size) {
    if (blk < 0) { vkFreeMemory(g_dev, m, NULL); atomic_fetch_sub(&g_big_bytes, size); return; }
    pthread_mutex_lock(&g_blkmu);
    mblock *b = &g_blk[blk]; fr_put(b, off, size);
    if (!--b->live) { vkFreeMemory(g_dev, b->m, NULL); b->m = VK_NULL_HANDLE; b->nfr = 0; }
    pthread_mutex_unlock(&g_blkmu);
}

// ---------- Submission contexts: command buffers, ring buffer, descriptor pool, the writes that wait for completion ----------
#define NCTX 3
#define RING (64u << 20)
// Replay fixtures use small real driver pools to exercise exhaustion and reuse.
#ifndef DM_VK_DESCRIPTOR_POOL_DIVISOR
#define DM_VK_DESCRIPTOR_POOL_DIVISOR 1
#endif
static VkDescriptorPool descriptor_pool_new(const uint32_t *need) {
    VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192},
                                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLER, 4096},
                                 {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 4096}};
    for (unsigned i = 0; i < sizeof ps / sizeof *ps; i++) {
        uint32_t minimum = need && need[ps[i].type] ? need[ps[i].type] : 1;
        ps[i].descriptorCount /= DM_VK_DESCRIPTOR_POOL_DIVISOR;
        if (ps[i].descriptorCount < minimum) ps[i].descriptorCount = minimum;
    }
    uint32_t sets = 32768 / DM_VK_DESCRIPTOR_POOL_DIVISOR;
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, sets ? sets : 1, sizeof ps / sizeof *ps, ps};
    VkDescriptorPool pool; VKCHECK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &pool)); return pool;
}
#include "visibility.h"
// Each NVN queue owns its feedback cursors. A queued submission keeps the
// buffer alive if the guest finalizes its queue before the GPU has finished.
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint64_t last_submission;
    _Atomic unsigned refs;
} feedback_control;
enum { FEEDBACK_BYTES = 8 * sizeof(uint32_t) };
static void feedback_release(feedback_control *control) {
    if (control && atomic_fetch_sub(&control->refs, 1) == 1) {
        vkDestroyBuffer(g_dev, control->buffer, NULL);
        vkFreeMemory(g_dev, control->memory, NULL);
        free(control);
    }
}
enum { POST_VISIBILITY_ADD = 4, POST_VISIBILITY_RESET, POST_FEEDBACK_RELEASE, POST_TIME_BOUNDARY, POST_GMEM_COPY };
typedef struct { int kind; void *dst; uint32_t mode, v; uint64_t addr; uint32_t type; const uint8_t *src; size_t n;
                 visibility_counter *visibility; const uint64_t *samples; } post_t;   // kind 1 event, 2 counter, 3 copy ring → guest
typedef struct vtex vtex;
typedef struct {
    VkCommandPool pool; VkCommandBuffer cb, up; VkFence fence; VkDescriptorPool dp;
    VkDescriptorPool dp0, *extra_dp; size_t nextra_dp, cextra_dp; // older sets remain valid until this context's fence
    VkBufferView *bv; size_t nbv, cbv; // typed views retain their submission's backing
    VkBuffer ring; VkDeviceMemory ringm; uint8_t *ringp; VkDeviceSize used, capacity;
    post_t *post; int npost, cpost;
    uint64_t fence_val; int up_used;
    VkQueryPool qp;   // GPU timestamps at the start and the end of the submission
    int timed_reports; uint64_t submitted_ns;
    visibility_queries visibility;
    query_pages time_boundaries;
    uint64_t raster_draws;
    int cut_copy;
    VkFramebuffer *fb; int nfb, cfb;   // render passes' framebuffers (no dynamic rendering), destroyed when the submission is done
    VkBuffer ring0; VkDeviceMemory ringm0; uint8_t *ringp0;   // the context's own ring; ring / ringp move to extra rings when a submission outgrows it (loading screens upload hundreds of MB)
    VkBuffer typed0[2]; // single-usage handles on the base ring's memory, created on first use
    struct { VkBuffer b; VkDeviceMemory m; VkDeviceSize size; VkBuffer typed[2]; } *extra; int nextra;
} ctx_t;
static _Atomic uint64_t g_gpu_ns;
static ctx_t g_ctx[NCTX]; static ctx_t *C; static int g_ci;
static _Atomic uint64_t g_fence;
static uint64_t g_done;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv;
static clockid_t g_cv_clock = CLOCK_REALTIME;
static int g_pend[NCTX], g_npend;   // contexts submitted and not yet completed, in order (under g_mu)

static uint64_t g_wgen;   // bumped by every in-order write to guest memory (and when the ring changes): the per-submission copies are reused only within one generation
static VkDeviceSize ring_alloc(VkDeviceSize n, VkDeviceSize align) {
    VkDeviceSize o = (C->used + align - 1) / align * align;
    if (o > C->capacity || n > C->capacity - o) {   // older backing stays live through this submission; a large single range gets its own sized allocation
        C->capacity = n > RING ? n : RING;
        void *next = realloc(C->extra, (C->nextra + 1) * sizeof *C->extra);
        if (!next) die("Vulkan: cannot track submission buffers");
        C->extra = next;
        memset(&C->extra[C->nextra],0,sizeof *C->extra);
        C->extra[C->nextra].size = C->capacity;
        C->extra[C->nextra].b = buf_new(C->capacity, ALL_BUF_USAGE, &C->extra[C->nextra].m, (void **)&C->ringp); C->ring = C->extra[C->nextra++].b; atomic_fetch_add(&g_extra_ring_bytes, C->capacity);
        C->used = 0; o = 0; g_wgen++;   // copies cached in the old ring are not reused from the new one
    }
    C->used = o + n; return o;
}
static VkDeviceSize ring_put(const void *src, VkDeviceSize n, VkDeviceSize align) {
    VkDeviceSize o = ring_alloc(n, align);
    if (o != VK_WHOLE_SIZE) memcpy(C->ringp + o, src, n);
    return o;
}
static void post_add(post_t p) { if (C->npost == C->cpost) { C->cpost = C->cpost ? 2 * C->cpost : 256; C->post = realloc(C->post, C->cpost * sizeof *C->post); } C->post[C->npost++] = p; }
static uint64_t guest_time_ns(void) {
    uint64_t ns = guest_ns();
    return ns ? ns : now_ns();
}
static uint64_t elapsed_gpu_ns(uint64_t from, uint64_t to) {
    double ns = (double)((to - from) & g_timestamp_mask) * g_props.limits.timestampPeriod;
    return ns >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)ns;
}
// Match Metal's segment mode: TIMESTAMP_TOP names the segment's start;
// TIMESTAMP names its end. Only the elapsed time is in the device time domain.
static void segment_ticks(const ctx_t *c, const uint64_t gpu[2], uint64_t duration, uint64_t ticks[2]) {
    uint64_t before = guest_time_ns(), end = before;
    if (p_calibrated_timestamps && c->qp) {
        VkCalibratedTimestampInfoEXT info = {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_DEVICE_EXT};
        uint64_t sampled, deviation;
        if (p_calibrated_timestamps(g_dev, 1, &info, &sampled, &deviation) == VK_SUCCESS) {
            uint64_t after = guest_time_ns();
            uint64_t mid = before + (after >= before ? (after-before)/2 : 0);
            uint64_t ago = elapsed_gpu_ns(gpu[1], sampled);
            if (mid >= ago && mid-ago >= c->submitted_ns) end = mid-ago;
        }
    }
    // Without clock calibration, anchor the measured duration at completion.
    // Bound both times to the guest submission interval, including a freeze.
    uint64_t earliest = c->submitted_ns < end ? c->submitted_ns : end;
    uint64_t begin = c->qp && duration < end-earliest ? end-duration : earliest;
    ticks[0] = nvn_timestamp_from_ns(begin);
    ticks[1] = nvn_timestamp_from_ns(end);
}
// ---------- What the GPU writes: storage buffers of compute dispatches ----------
// Draws and dispatches read guest memory through copies made when they are recorded, so what a dispatch writes into its storage buffers (particles simulated on the GPU,
// the exposure an auto-exposure pass computes) lives only in the ring until its submission completes and the copy goes back to guest memory (post kind 3). Until then
// later work recorded from the same range is bound to that ring range itself instead of a fresh copy of the guest's stale bytes (gw_find); the entry dies with the ring
// (ctx_complete, when the bytes are back in guest memory)
typedef struct { uint64_t addr, size; VkBuffer buf; VkDeviceSize off; uint8_t *p; uint64_t fence; int cpu_known; } gw_t;
static gw_t *g_gw; static size_t g_ngw, g_cgw; static pthread_mutex_t g_gwmu = PTHREAD_MUTEX_INITIALIZER;
static int overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) {
    return an && bn && (a < b ? b - a < an : a - b < bn);
}
static void gw_add(uint64_t addr, uint64_t size, VkBuffer buf, VkDeviceSize off, uint8_t *p, uint64_t fence, int cpu_known) {
    pthread_mutex_lock(&g_gwmu);
    for (size_t i = 0; i < g_ngw; i++) if (g_gw[i].addr == addr && g_gw[i].size == size) {
        --g_ngw;
        memmove(g_gw + i, g_gw + i + 1, (size_t)(g_ngw - i) * sizeof *g_gw);
        break;
    }   // keep overlapping writes in submission order
    if (g_ngw == g_cgw) {
        if (g_cgw > SIZE_MAX / 2 / sizeof *g_gw) die("Vulkan: GPU write tracking size overflow");
        size_t capacity = g_cgw ? g_cgw * 2 : 512;
        gw_t *next = realloc(g_gw, (size_t)capacity * sizeof *next);
        if (!next) die("Vulkan: cannot track live GPU buffer writes");
        g_gw = next; g_cgw = capacity;
    }
    g_gw[g_ngw++] = (gw_t){addr, size, buf, off, p, fence, cpu_known};
    pthread_mutex_unlock(&g_gwmu);
    g_wgen++;
}
// The ring range holding what the GPU wrote at [addr, addr + size), when an entry of the submission being recorded covers it and the offset in it keeps `align`. An entry
// of an earlier submission still in flight is waited for instead (its ring may be reused before this submission runs; once it completes the bytes are in guest memory and
// the caller copies them from there): 0 then, as for no entry
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static int gw_find(uint64_t addr, uint64_t size, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off, uint8_t **p) {
    int hit = 0; uint64_t wait = 0;
    pthread_mutex_lock(&g_gwmu);
    for (size_t i = g_ngw; i-- > 0 && !hit && !wait;) {
        const gw_t *e = &g_gw[i];
        if (!overlaps(addr, size, e->addr, e->size)) continue;
        if (e->fence != g_fence + 1) { wait = e->fence; break; }
        if (addr < e->addr || size > e->size - (addr - e->addr) || (e->off + (addr - e->addr)) % align) break; // copy_in composes partial overlaps
        *buf = e->buf; *off = e->off + (addr - e->addr); if (p) *p = e->p + (addr - e->addr); hit = 1;
    }
    pthread_mutex_unlock(&g_gwmu);
    if (wait) { static _Atomic uint64_t n; if (!atomic_fetch_add(&n, 1)) LOG("vk: a GPU-written buffer of an earlier submission is read: waiting for that submission (logged once)"); mtl_wait(wait, UINT64_MAX); }
    return hit;
}
// Prior submissions must finish before their CPU bytes are snapshotted or
// overwritten. Their ring storage is recycled independently of this submission.
static void gw_wait_prior(uint64_t addr, uint64_t size) {
    uint64_t wait = 0;
    pthread_mutex_lock(&g_gwmu);
    for (size_t i = 0; i < g_ngw; i++) if (g_gw[i].fence != g_fence + 1 &&
            overlaps(addr, size, g_gw[i].addr, g_gw[i].size) && g_gw[i].fence > wait) wait = g_gw[i].fence;
    pthread_mutex_unlock(&g_gwmu);
    if (wait) mtl_wait(wait, UINT64_MAX);
}
static void buffer_publish(uint64_t addr, uint64_t size, VkBuffer buffer, VkDeviceSize offset, uint8_t *mapped, int cpu_known) {
    gw_add(addr, size, buffer, offset, mapped, g_fence + 1, cpu_known);
    post_add((post_t){3, (void *)(uintptr_t)addr, 0, 0, 0, 0, mapped, size});
}
static void buffer_written(uint64_t addr, uint64_t size, VkBuffer buffer, VkDeviceSize offset, uint8_t *mapped) {
    buffer_publish(addr, size, buffer, offset, mapped, 0);
}
static void gw_expire(uint64_t fence) {   // completion: the submission's writes are in guest memory now, and its rings go
    pthread_mutex_lock(&g_gwmu);
    size_t keep = 0;
    for (size_t i = 0; i < g_ngw; ++i) if (g_gw[i].fence > fence) g_gw[keep++] = g_gw[i];
    g_ngw = keep;
    pthread_mutex_unlock(&g_gwmu);
}
// Find the end of the next logical segment. Map its device timestamp onto
// the same calibrated guest interval as the submission's start and end.
static uint64_t next_boundary(const ctx_t *c, int *cursor, uint64_t gpu_end, uint64_t lo, uint64_t hi) {
    while (*cursor<c->npost) {
        const post_t *p=&c->post[(*cursor)++];
        if (p->kind!=POST_TIME_BOUNDARY) continue;
        uint64_t ago=nvn_timestamp_from_ns(elapsed_gpu_ns(*p->samples,gpu_end));
        return ago<hi-lo ? hi-ago : lo;
    }
    return hi;
}
static void ctx_complete(ctx_t *c) {
    uint64_t ts[2] = {0}, ticks[2] = {0}, duration = 0;
    query_read(&c->visibility.pages); query_read(&c->time_boundaries);
    if (c->qp) {
        VKCHECK(vkGetQueryPoolResults(g_dev, c->qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT));
        duration = elapsed_gpu_ns(ts[0], ts[1]);
        atomic_fetch_add(&g_gpu_ns, duration);
    }
    if (c->timed_reports) segment_ticks(c, ts, duration, ticks);
    else if (c->npost) ticks[0] = ticks[1] = nvn_timestamp_from_ns(guest_time_ns());
    int next_cut=0;
    uint64_t lo=ticks[0], hi=c->timed_reports ? next_boundary(c,&next_cut,ts[1],lo,ticks[1]) : ticks[1];
    for (int i = 0; i < c->npost; i++) {
        post_t *p = &c->post[i];
        if (p->kind == 1) {
            dm_guest_write_prepare(p->dst, sizeof(uint32_t));
            volatile uint32_t *v = p->dst; if (p->mode == 1) *v += p->v; else *v = p->v;
        }
        else if (p->kind == 2) {
            uint64_t t = p->type==NVN_COUNTER_TYPE_TIMESTAMP_TOP ? lo : hi;
            int timestamp = p->type == NVN_COUNTER_TYPE_TIMESTAMP || p->type == NVN_COUNTER_TYPE_TIMESTAMP_TOP;
            uint64_t rec[2] = {timestamp && !DM_GAME_RENDER_COND ? t : 0, t};
            if (p->visibility) rec[0] = p->visibility->samples;
            dm_guest_write_prepare((void *)p->addr, sizeof rec);
            memcpy((void *)p->addr, rec, sizeof rec);
        }
        else if (p->kind == 3) {
            dm_guest_write_prepare(p->dst, p->n);
            memcpy(p->dst, p->src, p->n);
        }
        else if (p->kind==POST_GMEM_COPY && (((const uint32_t *)(uintptr_t)p->addr)[p->mode]&p->v)) {
            dm_guest_write_prepare(p->dst,p->n);
            memcpy(p->dst,p->src,p->n);
        }
        else if (p->kind == POST_VISIBILITY_ADD) p->visibility->samples += *p->samples;
        else if (p->kind == POST_VISIBILITY_RESET) p->visibility->samples = 0;
        else if (p->kind == POST_FEEDBACK_RELEASE) feedback_release(p->dst);
        else if (p->kind==POST_TIME_BOUNDARY && c->timed_reports) {
            lo=hi; hi=next_boundary(c,&next_cut,ts[1],lo,ticks[1]);
        }
        visibility_release(p->visibility);
    }
    c->npost = 0;
    c->timed_reports = 0;
    gw_expire(c->fence_val);
    for (size_t i = 0; i < c->nbv; i++) vkDestroyBufferView(g_dev, c->bv[i], NULL);
    c->nbv = 0;
    for (int i=0;i<c->nextra;i++) {
        for (int j=0;j<2;j++) vkDestroyBuffer(g_dev,c->extra[i].typed[j],NULL);
        vkDestroyBuffer(g_dev,c->extra[i].b,NULL); vkFreeMemory(g_dev,c->extra[i].m,NULL);
        atomic_fetch_sub(&g_extra_ring_bytes,c->extra[i].size);
    }
    c->nextra = 0;
    for (int i = 0; i < c->nfb; i++) vkDestroyFramebuffer(g_dev, c->fb[i], NULL);
    c->nfb = 0;
    for (size_t i = 0; i < c->nextra_dp; i++) vkDestroyDescriptorPool(g_dev, c->extra_dp[i], NULL);
    c->nextra_dp = 0;
}
static void garbage_free(uint64_t done);
static void *completion_thread(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_npend) pthread_cond_wait(&g_cv, &g_mu);
        ctx_t *c = &g_ctx[g_pend[0]];
        pthread_mutex_unlock(&g_mu);
        VkResult r = vkWaitForFences(g_dev, 1, &c->fence, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS) die("Vulkan: waiting for a submission failed (%d)", (int)r);
        ctx_complete(c);
        pthread_mutex_lock(&g_mu);
        uint64_t completed = c->fence_val;
        g_done = completed; memmove(g_pend, g_pend + 1, --g_npend * sizeof *g_pend);
        pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
        garbage_free(completed);   // c can now belong to a later submission
    }
    return NULL;
}

// ---------- Device ----------
static int g_vertex_stores, g_has_u8, g_storage_ms, g_precise_visibility, g_depth_clamp, g_depth_bias_clamp;
static int g_logic_op, g_non_solid_fill, g_layer_output;
static int g_minmax_filter, g_minmax_swizzle;
static uint32_t g_custom_border_limit, g_custom_border_count;
static int g_border_from_image, g_border_without_format;
static float g_max_anisotropy;
enum { ZERO_BUFFER_BYTES=64<<10 };
static VkSampler g_dummy_samp; static VkImage g_dummy_img; static VkImageView g_dummy_view; static VkBuffer g_zero_buf, g_support_buf; static VkDeviceMemory g_zero_mem, g_support_mem;
static void dummies(void);
static int g_has_surface;
static _Atomic uint64_t g_vbuf_copies, g_vbuf_copy_bytes;
// Render passes: dynamic rendering where the GPU has it (Vulkan 1.3, or 1.2 / 1.1 with VK_KHR_dynamic_rendering), else classic render passes and framebuffers made per pass
// Every submission, the game's and the presents, waits on GPU for the one before and signals the next value of one timeline semaphore (g_tlv, under g_qmu). Submission order alone with a
// barrier at each start should do; the Mali-G57 (Unisoc T820) did not keep it for a present's blit, whose batch also waits on the swapchain acquire: two thirds of Metroid Dread's menu
// frames showed black. GPUs without timeline semaphores (Vulkan 1.1 without 1.2) wait for the GPU on the CPU before each present instead
static VkSemaphore g_tl; static uint64_t g_tlv;
// Pipelines compiled on earlier runs: the driver's pipeline cache, kept in <DM_VK_DIR>/../pipeline_cache.bin (written every 1800 frames when pipelines were added). A Mali-G57
// compiles one in about 40 ms, and a scene meeting a dozen new effects stalled its frame for 150-240 ms (Super Mario Odyssey); data from another GPU or driver is ignored by it
static VkPipelineCache g_pcache; static char g_pcache_path[1024]; static int g_pcache_saved;
static void pcache_load(void) {
    const char *d = getenv("DM_VK_DIR"); size_t n = 0; void *data = NULL;
    if (d) { snprintf(g_pcache_path, sizeof g_pcache_path, "%s/../pipeline_cache.bin", d); FILE *f = fopen(g_pcache_path, "rb");
             if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); if (sz > 0 && (data = malloc((size_t)sz)) && fread(data, 1, (size_t)sz, f) == (size_t)sz) n = (size_t)sz; fclose(f); } }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, n, data};
    if (vkCreatePipelineCache(g_dev, &ci, NULL, &g_pcache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; if (vkCreatePipelineCache(g_dev, &ci, NULL, &g_pcache) != VK_SUCCESS) g_pcache = VK_NULL_HANDLE; }
    if (n) LOG("vk: pipeline cache of %zu KB loaded from an earlier run", n >> 10);
    free(data);
}
static int g_npso;
static void pcache_save(void) {
    if (!g_pcache || !g_pcache_path[0] || g_npso == g_pcache_saved) return;
    size_t n = 0; if (vkGetPipelineCacheData(g_dev, g_pcache, &n, NULL) != VK_SUCCESS || !n) return;
    void *data = malloc(n); if (!data) return;
    if (vkGetPipelineCacheData(g_dev, g_pcache, &n, data) == VK_SUCCESS) {
        char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", g_pcache_path); FILE *f = fopen(tmp, "wb");
        if (f) { int ok = fwrite(data, 1, n, f) == n; ok &= fclose(f) == 0; if (ok) rename(tmp, g_pcache_path); }
        g_pcache_saved = g_npso;
    }
    free(data);
}
static int g_dynrend; static PFN_vkCmdBeginRenderingKHR p_begin_rendering; static PFN_vkCmdEndRenderingKHR p_end_rendering;
static void vk_init_(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "nxnative", 1, "dmhost", 1, VK_API_VERSION_1_3};
#ifdef __ANDROID__
    const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    uint32_t nie = 0; vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    VkExtensionProperties *iep = calloc(nie ? nie : 1, sizeof *iep); vkEnumerateInstanceExtensionProperties(NULL, &nie, iep);
    int nsurf = 0; for (uint32_t i = 0; i < nie; i++) for (int k = 0; k < 2; k++) nsurf += !strcmp(iep[i].extensionName, iext[k]);
    free(iep); g_has_surface = nsurf == 2;
#else
    const char **iext = NULL;   // offscreen Vulkan replay, without a window system
#endif
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, g_has_surface ? 2 : 0, iext};
    VKCHECK(vkCreateInstance(&ii, NULL, &g_inst));
    uint32_t n = 1; VkResult r = vkEnumeratePhysicalDevices(g_inst, &n, &g_pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) die("Vulkan: no GPU");
    vkGetPhysicalDeviceProperties(g_pd, &g_props); vkGetPhysicalDeviceMemoryProperties(g_pd, &g_memp);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL);
    VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
    for (g_qf = 0; g_qf < nq && !(qp[g_qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); g_qf++) {}
    if (g_qf == nq) die("Vulkan: no graphics queue");
    uint32_t bits = getenv("DM_VK_NO_GPU_TIMESTAMPS") ? 0 : qp[g_qf].timestampValidBits;
    g_timestamp_mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    const char *cut=getenv("DM_NVN_CUT"); g_cut_draws=cut ? atoi(cut) : 64;
    const char *timer = getenv("DM_NVN_GPU_TS");
    g_segment_timestamps = timer && *timer ? !strcmp(timer,"segment") : DM_GAME_GPU_TS_SEGMENT;
    if (g_segment_timestamps && !bits) LOG("[RED] vk: graphics queue has no GPU timestamps; segment reports use the guest submission interval");
    // Every feature the GPU has is enabled (the translated shaders may use any of them), except the costly robustness beyond robustBufferAccess. The feature structures
    // chained are only those of the GPU's Vulkan version and extensions (1.1 phones have neither the 1.2 / 1.3 structures nor dynamic rendering)
    uint32_t dv = g_props.apiVersion;
    int minmax_ext=0, driver_properties_ext=0, address_ext=0;
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep); vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, ep);
    int has_sc = 0, has_u8 = 0, has_dr = 0, has_subgroup_sizes = 0, has_calibrated = 0;
    int divisor_ext = 0, divisor_khr = 0, descriptor_indexing = 0;
    int border_ext = 0, border_swizzle_ext = 0, texel_ext=0, layer_ext=0;
    for (uint32_t i = 0; i < ne; i++) {
        has_sc |= !strcmp(ep[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME); has_u8 |= !strcmp(ep[i].extensionName, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
        has_dr |= !strcmp(ep[i].extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        has_subgroup_sizes |= !strcmp(ep[i].extensionName, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
        has_calibrated |= !strcmp(ep[i].extensionName, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        texel_ext |= !strcmp(ep[i].extensionName,VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME);
        layer_ext |= !strcmp(ep[i].extensionName,VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
        descriptor_indexing |= !strcmp(ep[i].extensionName, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        border_ext |= !strcmp(ep[i].extensionName, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        border_swizzle_ext |= !strcmp(ep[i].extensionName, VK_EXT_BORDER_COLOR_SWIZZLE_EXTENSION_NAME);
        divisor_ext |= ep[i].specVersion >= 3 && !strcmp(ep[i].extensionName,VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        divisor_khr |= !strcmp(ep[i].extensionName,VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        minmax_ext |= !strcmp(ep[i].extensionName,VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
        address_ext |= !strcmp(ep[i].extensionName,VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        driver_properties_ext |= !strcmp(ep[i].extensionName,VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    }
    free(ep);
    g_subgroup_sizes = (VkPhysicalDeviceSubgroupSizeControlProperties){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    g_subgroup = (VkPhysicalDeviceSubgroupProperties){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    if (has_subgroup_sizes || dv >= VK_API_VERSION_1_3) g_subgroup.pNext = &g_subgroup_sizes;
    VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &g_subgroup};
    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT div_ext_props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT, props2.pNext};
    VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR div_khr_props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR, props2.pNext};
    if (divisor_ext) props2.pNext=&div_ext_props;
    else if (divisor_khr) props2.pNext=&div_khr_props;
    VkPhysicalDeviceCustomBorderColorPropertiesEXT border_props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT, props2.pNext};
    if (border_ext) props2.pNext = &border_props;
    VkPhysicalDeviceSamplerFilterMinmaxProperties minmax_props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES,props2.pNext};
    if (dv>=VK_API_VERSION_1_2 || minmax_ext) props2.pNext=&minmax_props;
    VkPhysicalDeviceDriverProperties driver_props={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,props2.pNext};
    if (dv>=VK_API_VERSION_1_2 || driver_properties_ext) props2.pNext=&driver_props;
    int relaxed_texels=!getenv("DM_VK_STRICT_TEXEL_ALIGNMENT") && (dv>=VK_API_VERSION_1_3 || texel_ext);
    if (relaxed_texels) {
        g_texel_limits=(VkPhysicalDeviceTexelBufferAlignmentProperties){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES,props2.pNext};
        props2.pNext=&g_texel_limits;
    }
    vkGetPhysicalDeviceProperties2(g_pd, &props2);
    g_texel_limits.pNext=NULL;
    VkPhysicalDeviceSubgroupSizeControlFeatures size_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT u8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceShaderDrawParametersFeatures sdp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceDescriptorIndexingFeatures indexing = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    void *chain = dv < VK_API_VERSION_1_2 && descriptor_indexing ? &indexing : NULL;
    VkPhysicalDeviceBufferDeviceAddressFeatures address_features={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,chain};
    if (dv<VK_API_VERSION_1_2 && address_ext) chain=&address_features;
    VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT texel_features={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT,chain};
    if (relaxed_texels && dv<VK_API_VERSION_1_3) chain=&texel_features;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, chain};
    if (border_ext) chain = &border_features;
    VkPhysicalDeviceBorderColorSwizzleFeaturesEXT border_swizzle = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT, chain};
    if (border_swizzle_ext && border_ext) chain = &border_swizzle;
    // EXT rev 3 guarantees firstInstance + localInstance/divisor. Its KHR
    // successor allows implementations without nonzero firstInstance support.
    const char *div_extension = divisor_ext ? VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME :
        div_khr_props.supportsNonZeroFirstInstance ? VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME : NULL;
    if (getenv("DM_VK_NO_VERTEX_DIVISOR")) div_extension=NULL;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    if (div_extension) { div_features.pNext=chain; chain=&div_features; }
    if (has_subgroup_sizes && dv < VK_API_VERSION_1_3) { size_features.pNext = chain; chain = &size_features; }
    if (has_u8) { u8.pNext = chain; chain = &u8; }
    if (dv >= VK_API_VERSION_1_3) { f13.pNext = chain; chain = &f13; } else if (has_dr) { dr.pNext = chain; chain = &dr; }
    if (dv >= VK_API_VERSION_1_2) { f12.pNext = chain; chain = &f12; f11.pNext = chain; chain = &f11; } else { sdp.pNext = chain; chain = &sdp; }
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain};
    vkGetPhysicalDeviceFeatures2(g_pd, &f2);
    g_max_anisotropy = f2.features.samplerAnisotropy ? fminf(16, g_props.limits.maxSamplerAnisotropy) : 1;
    g_draw_parameters = dv >= VK_API_VERSION_1_2 ? f11.shaderDrawParameters : sdp.shaderDrawParameters;
    g_layer_output = dv >= VK_API_VERSION_1_2 ? f12.shaderOutputLayer : layer_ext;
    g_vertex_stores = f2.features.vertexPipelineStoresAndAtomics;
    g_storage_ms = f2.features.shaderStorageImageMultisample;
    g_precise_visibility = f2.features.occlusionQueryPrecise;
    g_depth_clamp = f2.features.depthClamp && !getenv("DM_VK_NO_DEPTH_CLAMP");
    if (getenv("DM_VK_NO_DEPTH_BIAS_CLAMP")) f2.features.depthBiasClamp = VK_FALSE;
    g_depth_bias_clamp = f2.features.depthBiasClamp;
    if (getenv("DM_VK_NO_LOGIC_OP")) f2.features.logicOp = VK_FALSE;
    g_logic_op = f2.features.logicOp;
    if (getenv("DM_VK_NO_WIREFRAME")) f2.features.fillModeNonSolid = VK_FALSE;
    g_non_solid_fill = f2.features.fillModeNonSolid;
    f13.robustImageAccess = VK_FALSE;
    const char *ext[16]; uint32_t next = 0;
    g_buffer_address=(dv>=VK_API_VERSION_1_2 ? f12.bufferDeviceAddress : address_features.bufferDeviceAddress);
    if (g_buffer_address && dv<VK_API_VERSION_1_2) ext[next++]=VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
    g_minmax_filter=(dv>=VK_API_VERSION_1_2 ? f12.samplerFilterMinmax : minmax_ext) && !getenv("DM_VK_NO_SAMPLER_MINMAX");
    // Qualification found weighted min/max results on emulator software
    // drivers, including a fractional-mip blend on desktop Lavapipe.
    if (driver_props.driverID==VK_DRIVER_ID_MESA_LLVMPIPE || driver_props.driverID==VK_DRIVER_ID_GOOGLE_SWIFTSHADER)
        g_minmax_filter=0;
    g_minmax_swizzle=minmax_props.filterMinmaxImageComponentMapping;
    if (g_minmax_filter && dv<VK_API_VERSION_1_2) ext[next++]=VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME;
    if (dv<VK_API_VERSION_1_3) {
        if (texel_features.texelBufferAlignment) ext[next++]=VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME;
        else relaxed_texels=0;
    }
    if (!relaxed_texels) g_texel_limits=(VkPhysicalDeviceTexelBufferAlignmentProperties){0};
    if (border_features.customBorderColors && !getenv("DM_VK_NO_CUSTOM_BORDER")) {
        ext[next++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
        g_custom_border_limit = border_props.maxCustomBorderColorSamplers;
        if (getenv("DM_VK_NO_FORMATLESS_BORDER")) border_features.customBorderColorWithoutFormat = VK_FALSE;
        g_border_without_format = border_features.customBorderColorWithoutFormat;
    } else {
        border_features.customBorderColors = border_features.customBorderColorWithoutFormat = VK_FALSE;
    }
    if (g_custom_border_limit && border_swizzle.borderColorSwizzleFromImage) {
        ext[next++] = VK_EXT_BORDER_COLOR_SWIZZLE_EXTENSION_NAME;
        g_border_from_image = 1;
    } else {
        border_swizzle.borderColorSwizzle = border_swizzle.borderColorSwizzleFromImage = VK_FALSE;
    }
    g_sampled_indexing=dv>=VK_API_VERSION_1_2 ? f12.shaderSampledImageArrayNonUniformIndexing : indexing.shaderSampledImageArrayNonUniformIndexing;
    g_texel_indexing=dv>=VK_API_VERSION_1_2 ? f12.shaderUniformTexelBufferArrayNonUniformIndexing : indexing.shaderUniformTexelBufferArrayNonUniformIndexing;
    int runtime_arrays=dv>=VK_API_VERSION_1_2 ? f12.runtimeDescriptorArray : indexing.runtimeDescriptorArray;
    g_descriptor_tables=runtime_arrays && g_sampled_indexing;
    g_texel_descriptor_tables=runtime_arrays && g_texel_indexing;
    if (dv < VK_API_VERSION_1_2 && descriptor_indexing) ext[next++] = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
    if (div_extension) {
        ext[next++]=div_extension;
        if (div_features.vertexAttributeInstanceRateDivisor)
            g_vertex_divisor_limit=divisor_ext ? div_ext_props.maxVertexAttribDivisor : div_khr_props.maxVertexAttribDivisor;
    }
    LOG("vk: native vertex instance divisor limit %u",g_vertex_divisor_limit);
    // A guest draw may need several native instance ranges on these devices.
    // Keep its original BaseInstance in the existing draw-parameter push block.
    if (g_vertex_divisor_limit<UINT32_MAX || getenv("DM_VK_NO_DRAW_PARAMETERS")) g_draw_parameters=0;
    if (!g_draw_parameters) LOG("vk: shader draw parameters supplied by host push constants");
    if (dv < VK_API_VERSION_1_2 && layer_ext) ext[next++] = VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME;
    has_calibrated=has_calibrated && bits && !getenv("DM_VK_NO_CALIBRATED_TIMESTAMPS");
    if (g_segment_timestamps && has_calibrated) ext[next++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    g_selectable_subgroup_stages = 0;
    if (f13.subgroupSizeControl || size_features.subgroupSizeControl) {
        g_selectable_subgroup_stages = g_subgroup_sizes.requiredSubgroupSizeStages;
        if (dv < VK_API_VERSION_1_3) ext[next++] = VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME;
    }
    g_has_surface = g_has_surface && has_sc;
    if (g_has_surface) ext[next++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (has_u8 && u8.indexTypeUint8 && !getenv("DM_VK_NO_INDEX_UINT8")) { ext[next++] = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME; g_has_u8 = 1; }
    g_dynrend = dv >= VK_API_VERSION_1_3 ? f13.dynamicRendering : has_dr && dr.dynamicRendering;
    if (getenv("DM_VK_NO_DYNREND")) g_dynrend = 0;   // diagnostics: the render pass path of GPUs without dynamic rendering, on one that has it
    if (g_dynrend && dv < VK_API_VERSION_1_3) ext[next++] = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g_qf, 1, &prio};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qi, 0, NULL, next, ext, NULL};
    VKCHECK(vkCreateDevice(g_pd, &di, NULL, &g_dev));
    if (g_segment_timestamps && has_calibrated)
        p_calibrated_timestamps = (PFN_vkGetCalibratedTimestampsEXT)vkGetDeviceProcAddr(g_dev, "vkGetCalibratedTimestampsEXT");
    if (g_buffer_address) {
        p_buffer_address=(PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(g_dev,dv>=VK_API_VERSION_1_2 ? "vkGetBufferDeviceAddress" : "vkGetBufferDeviceAddressKHR");
        if (!p_buffer_address) g_buffer_address=0;
    }
    vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);
    pcache_load();
    if (dv >= VK_API_VERSION_1_2 && f12.timelineSemaphore && !getenv("DM_VK_NO_TIMELINE")) {
        VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti, 0};
        if (vkCreateSemaphore(g_dev, &si, NULL, &g_tl) != VK_SUCCESS) g_tl = VK_NULL_HANDLE;
    }
    if (g_dynrend) {
        p_begin_rendering = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(g_dev, dv >= VK_API_VERSION_1_3 ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR");
        p_end_rendering = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(g_dev, dv >= VK_API_VERSION_1_3 ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR");
        if (!p_begin_rendering || !p_end_rendering) g_dynrend = 0;
    }
    for (int i = 0; i < NCTX; i++) {
        ctx_t *c = &g_ctx[i];
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qf};
        VKCHECK(vkCreateCommandPool(g_dev, &pi, NULL, &c->pool));
        VkCommandBuffer cbs[2]; VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
        VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, cbs)); c->cb = cbs[0]; c->up = cbs[1];
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        VKCHECK(vkCreateFence(g_dev, &fi, NULL, &c->fence));
        c->ring = c->ring0 = buf_new(RING, ALL_BUF_USAGE, &c->ringm, (void **)&c->ringp); c->ringm0 = c->ringm; c->ringp0 = c->ringp;
        c->dp = c->dp0 = descriptor_pool_new(NULL);
        VkQueryPoolCreateInfo qi2 = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2};
        if (g_timestamp_mask) VKCHECK(vkCreateQueryPool(g_dev, &qi2, NULL, &c->qp));
    }
    dummies();
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr)) die("Vulkan: cannot initialize completion condition attributes");
#if defined(_POSIX_CLOCK_SELECTION) && _POSIX_CLOCK_SELECTION >= 0
    if (!pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) g_cv_clock = CLOCK_MONOTONIC;
#endif
    if (pthread_cond_init(&g_cv, &attr)) die("Vulkan: cannot initialize the completion condition");
    pthread_condattr_destroy(&attr);
    pthread_t th; if (pthread_create(&th, NULL, completion_thread, NULL)) die("Vulkan: cannot start the completion thread"); pthread_detach(th);
    LOG("vk: %s, Vulkan %u.%u.%u, driver %#x; uint8 indices %s, %s, %s", g_props.deviceName, VK_API_VERSION_MAJOR(g_props.apiVersion), VK_API_VERSION_MINOR(g_props.apiVersion),
        VK_API_VERSION_PATCH(g_props.apiVersion), g_props.driverVersion, g_has_u8 ? "yes" : "no", g_dynrend ? "dynamic rendering" : "render passes (no dynamic rendering)",
        g_tl ? "submissions chained by a timeline semaphore" : "no timeline semaphore (presents wait for the GPU)");
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void vk_init(void) { pthread_once(&g_once, vk_init_); }
void mtl_init(void) { vk_init(); }

// One-time commands outside the submissions (dummy contents, the present readback): their own pool, the queue lock held, waited for
static VkCommandPool g_opool;
static VkCommandBuffer once_begin(void) {
    pthread_mutex_lock(&g_qmu);
    if (!g_opool) { VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, g_qf}; VKCHECK(vkCreateCommandPool(g_dev, &pi, NULL, &g_opool)); }
    VkCommandBuffer cb; VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_opool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VKCHECK(vkBeginCommandBuffer(cb, &bi)); return cb;
}
static void once_end(VkCommandBuffer cb) {
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    VKCHECK(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE)); VKCHECK(vkQueueWaitIdle(g_q));
    vkFreeCommandBuffers(g_dev, g_opool, 1, &cb);
    pthread_mutex_unlock(&g_qmu);
}
static void barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void to_general(VkCommandBuffer cb, VkImage img, VkImageAspectFlags asp) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {asp, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void dummies(void) {
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.maxLod = 0;
    VKCHECK(vkCreateSampler(g_dev, &si, NULL, &g_dummy_samp));
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VKCHECK(vkCreateImage(g_dev, &ii, NULL, &g_dummy_img));
    VkMemoryRequirements r; vkGetImageMemoryRequirements(g_dev, g_dummy_img, &r); VkDeviceMemory m = mem_alloc(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkBindImageMemory(g_dev, g_dummy_img, m, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_dummy_img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VKCHECK(vkCreateImageView(g_dev, &vi, NULL, &g_dummy_view));
    void *zp, *sp;
    g_zero_buf = buf_new(ZERO_BUFFER_BYTES, ALL_BUF_USAGE, &g_zero_mem, &zp); memset(zp, 0, ZERO_BUFFER_BYTES);
    // Ryujinx's support buffer: alpha test 0, is_bgra[8] at 16, viewport inverse 144, viewport size 160, render scale count 176, render scale float4[73] at 192 (all 1.0)
    g_support_buf = buf_new(4096, ALL_BUF_USAGE, &g_support_mem, &sp); memset(sp, 0, 4096);
    for (int i = 0; i < 73; i++) *(float *)((uint8_t *)sp + 192 + 16 * i) = 1.0f;
    VkCommandBuffer cb = once_begin(); to_general(cb, g_dummy_img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue black = {0}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g_dummy_img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr); once_end(cb);
}
// Unselected elements of a bindless array often contain no handle, or a handle
// of another texture kind. Their descriptors still need the shader's view type.
static VkFormat dummy_format(int num, int multisample) {
    // An R32 dummy would synthesize alpha one even when cleared to zero.
    if (multisample) return num == 1 ? VK_FORMAT_R16G16B16A16_SINT : num == 2 ? VK_FORMAT_R16G16B16A16_UINT : VK_FORMAT_R8G8B8A8_UNORM;
    return num == 1 ? VK_FORMAT_R32G32B32A32_SINT : num == 2 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R8G8B8A8_UNORM;
}
static VkImageView empty_image(int view, VkFormat format, VkImageUsageFlags usage, int multisample) {
    int one = view == VK_IMAGE_VIEW_TYPE_1D || view == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    int cube = view == VK_IMAGE_VIEW_TYPE_CUBE || view == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    if (multisample && view != VK_IMAGE_VIEW_TYPE_2D && view != VK_IMAGE_VIEW_TYPE_2D_ARRAY) return VK_NULL_HANDLE;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        one ? VK_IMAGE_TYPE_1D : view == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        format, {1, 1, 1}, 1, cube ? 6u : 1u, multisample ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageFormatProperties support;
    if (vkGetPhysicalDeviceImageFormatProperties(g_pd, format, ii.imageType, ii.tiling, ii.usage, ii.flags, &support) != VK_SUCCESS ||
        !(support.sampleCounts & ii.samples)) return VK_NULL_HANDLE;
    VkImage image;
    if (vkCreateImage(g_dev, &ii, NULL, &image) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(g_dev, image, &requirements);
    VkDeviceMemory memory = mem_alloc(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkBindImageMemory(g_dev, image, memory, 0));
    VkImageAspectFlags aspect = format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageSubresourceRange range = {aspect, 0, 1, 0, ii.arrayLayers};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, image,
        (VkImageViewType)view, format, {0}, range};
    VkImageView result;
    VKCHECK(vkCreateImageView(g_dev, &vi, NULL, &result));
    VkCommandBuffer cb = once_begin(); to_general(cb, image, aspect);
    if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
        VkClearDepthStencilValue zero = {0};
        vkCmdClearDepthStencilImage(cb, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    } else {
        VkClearColorValue zero = {0};
        vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    VkImageMemoryBarrier ready = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, range};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ready);
    once_end(cb);
    return result;
}
static VkImageView sampled_dummy(int view, int num, int multisample, int depth) {
    static VkImageView views[7][3][2][2];
    if (view < 0) view = VK_IMAGE_VIEW_TYPE_2D;
    if (view > VK_IMAGE_VIEW_TYPE_CUBE_ARRAY || num < 0 || num > 2) return VK_NULL_HANDLE;
    if (view == VK_IMAGE_VIEW_TYPE_2D && !num && !multisample && !depth) return g_dummy_view;
    VkImageView *cached = &views[view][num][!!multisample][!!depth];
    if (!*cached) *cached = empty_image(view, depth ? VK_FORMAT_D32_SFLOAT : dummy_format(num, multisample),
        VK_IMAGE_USAGE_SAMPLED_BIT, multisample);
    return *cached;
}

// ---------- Formats ----------
// Compressed formats Mali cannot sample, decoded on upload into the smallest format that samples the same: BC1-3 and BC7 to RGBA8, BC4 to R8, BC5 to RG8 (UNORM or SNORM: the
// missing channels read 0, alpha 1, as BC4 / BC5 sample), unsigned BC6H to B10G11R11 (its 16-bit floats lose only mantissa bits below what BC6H keeps), signed BC6H to RGBA16F.
// BC4-7 by bcdec (third_party/bcdec, MIT / Unlicense)
enum { DEC_NONE, DEC_BC1, DEC_BC2, DEC_BC3, DEC_BC4, DEC_BC4S, DEC_BC5, DEC_BC5S, DEC_BC7, DEC_BC6H, DEC_BC6HS };
static int dec_bpp(int dec) { return dec == DEC_BC4 || dec == DEC_BC4S ? 1 : dec == DEC_BC5 || dec == DEC_BC5S ? 2 : dec == DEC_BC6HS ? 8 : 4; }   // bytes per decoded pixel
typedef struct { VkFormat f; uint8_t noalpha, dec, depth, stencil; } fmt_t;
// Adapted from LAS Recomp (Copyright 2026 Peiikai, MIT; dm/third_party/lasrecomp).
// Capability-based depth format choice: D24S8 is optional in
// Vulkan (SwiftShader reports no features for it). Copies have their own blit
// fallback below, so only the image's actual sampling/attachment/transfer uses
// are required here. Keep D24 where all of those uses are supported.
static VkFormat depth24_stencil8(void) {
    static VkFormat chosen;
    if (!chosen) {
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(g_pd, VK_FORMAT_D24_UNORM_S8_UINT, &p);
        chosen = (p.optimalTilingFeatures & need) == need ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
#ifdef DM_VK_PUBLIC_BINDINGS
        if (getenv("DM_VK_TEST_D24_AS_D32")) chosen = VK_FORMAT_D32_SFLOAT_S8_UINT;
#endif
        vkGetPhysicalDeviceFormatProperties(g_pd, chosen, &p);
        if ((p.optimalTilingFeatures & need) != need) die("Vulkan: no sampled depth/stencil attachment format with transfer support");
        LOG("vk: depth24/stencil8 uses %s", chosen == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
    }
    return chosen;
}
static fmt_t fmt_of(int nvn) {
    fmt_t r = {VK_FORMAT_UNDEFINED, 0, 0, 0, 0};
    if (nvn >= 0x79 && nvn <= 0x94) {   // ASTC: 4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12, LDR then sRGB; Vulkan lists UNORM / SRGB in pairs
        int k = (nvn - 0x79) % 14, srgb = nvn >= 0x87; r.f = (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * k + srgb); return r;
    }
    switch (nvn) {
        case 0x01: r.f = VK_FORMAT_R8_UNORM; break; case 0x02: r.f = VK_FORMAT_R8_SNORM; break; case 0x03: r.f = VK_FORMAT_R8_UINT; break; case 0x04: r.f = VK_FORMAT_R8_SINT; break;
        case 0x05: r.f = VK_FORMAT_R16_SFLOAT; break; case 0x06: r.f = VK_FORMAT_R16_UNORM; break; case 0x07: r.f = VK_FORMAT_R16_SNORM; break;
        case 0x08: r.f = VK_FORMAT_R16_UINT; break; case 0x09: r.f = VK_FORMAT_R16_SINT; break;
        case 0x0A: r.f = VK_FORMAT_R32_SFLOAT; break; case 0x0B: r.f = VK_FORMAT_R32_UINT; break; case 0x0C: r.f = VK_FORMAT_R32_SINT; break;
        case 0x0D: r.f = VK_FORMAT_R8G8_UNORM; break; case 0x0E: r.f = VK_FORMAT_R8G8_SNORM; break; case 0x0F: r.f = VK_FORMAT_R8G8_UINT; break; case 0x10: r.f = VK_FORMAT_R8G8_SINT; break;
        case 0x11: r.f = VK_FORMAT_R16G16_SFLOAT; break; case 0x12: r.f = VK_FORMAT_R16G16_UNORM; break; case 0x13: r.f = VK_FORMAT_R16G16_SNORM; break;
        case 0x14: r.f = VK_FORMAT_R16G16_UINT; break; case 0x15: r.f = VK_FORMAT_R16G16_SINT; break;
        case 0x16: r.f = VK_FORMAT_R32G32_SFLOAT; break; case 0x17: r.f = VK_FORMAT_R32G32_UINT; break; case 0x18: r.f = VK_FORMAT_R32G32_SINT; break;
        case 0x6D: r.noalpha = 1; /* fall through */ case 0x25: r.f = VK_FORMAT_R8G8B8A8_UNORM; break;
        case 0x6E: r.noalpha = 1; /* fall through */ case 0x26: r.f = VK_FORMAT_R8G8B8A8_SNORM; break;
        case 0x6F: r.noalpha = 1; /* fall through */ case 0x27: r.f = VK_FORMAT_R8G8B8A8_UINT; break;
        case 0x70: r.noalpha = 1; /* fall through */ case 0x28: r.f = VK_FORMAT_R8G8B8A8_SINT; break;
        case 0x71: r.noalpha = 1; /* fall through */ case 0x29: r.f = VK_FORMAT_R16G16B16A16_SFLOAT; break;
        case 0x72: r.noalpha = 1; /* fall through */ case 0x2A: r.f = VK_FORMAT_R16G16B16A16_UNORM; break;
        case 0x73: r.noalpha = 1; /* fall through */ case 0x2B: r.f = VK_FORMAT_R16G16B16A16_SNORM; break;
        case 0x74: r.noalpha = 1; /* fall through */ case 0x2C: r.f = VK_FORMAT_R16G16B16A16_UINT; break;
        case 0x75: r.noalpha = 1; /* fall through */ case 0x2D: r.f = VK_FORMAT_R16G16B16A16_SINT; break;
        case 0x76: r.noalpha = 1; /* fall through */ case 0x2E: r.f = VK_FORMAT_R32G32B32A32_SFLOAT; break;
        case 0x77: r.noalpha = 1; /* fall through */ case 0x2F: r.f = VK_FORMAT_R32G32B32A32_UINT; break;
        case 0x78: r.noalpha = 1; /* fall through */ case 0x30: r.f = VK_FORMAT_R32G32B32A32_SINT; break;
        case 0x31: r.f = depth24_stencil8(); r.depth = 1; r.stencil = 1; break;   // STENCIL8 uses a depth/stencil image's stencil plane
        case 0x32: r.f = VK_FORMAT_D16_UNORM; r.depth = 1; break;
        case 0x33: case 0x35: r.f = depth24_stencil8(); r.depth = 1; r.stencil = 1; break;
        case 0x34: r.f = VK_FORMAT_D32_SFLOAT; r.depth = 1; break;
        case 0x36: r.f = VK_FORMAT_D32_SFLOAT_S8_UINT; r.depth = 1; r.stencil = 1; break;
        case 0x37: r.noalpha = 1; /* fall through */ case 0x38: r.f = VK_FORMAT_R8G8B8A8_SRGB; break;
        case 0x39: r.f = VK_FORMAT_A4B4G4R4_UNORM_PACK16; break;
        case 0x3A: r.noalpha = 1; /* fall through */ case 0x3B: r.f = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
        case 0x3C: r.f = VK_FORMAT_R5G6B5_UNORM_PACK16; break;
        case 0x3D: r.f = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break; case 0x3E: r.f = VK_FORMAT_A2B10G10R10_UINT_PACK32; break;
        case 0x3F: r.f = VK_FORMAT_B10G11R11_UFLOAT_PACK32; break; case 0x40: r.f = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32; break;
        case 0x41: r.noalpha = 1; /* fall through */ case 0x42: r.f = VK_FORMAT_R8G8B8A8_UNORM; r.dec = DEC_BC1; break;
        case 0x43: r.f = VK_FORMAT_R8G8B8A8_UNORM; r.dec = DEC_BC2; break;
        case 0x44: r.f = VK_FORMAT_R8G8B8A8_UNORM; r.dec = DEC_BC3; break;
        case 0x45: r.noalpha = 1; /* fall through */ case 0x46: r.f = VK_FORMAT_R8G8B8A8_SRGB; r.dec = DEC_BC1; break;
        case 0x47: r.f = VK_FORMAT_R8G8B8A8_SRGB; r.dec = DEC_BC2; break;
        case 0x48: r.f = VK_FORMAT_R8G8B8A8_SRGB; r.dec = DEC_BC3; break;
        case 0x49: r.f = VK_FORMAT_R8_UNORM; r.dec = DEC_BC4; break; case 0x4A: r.f = VK_FORMAT_R8_SNORM; r.dec = DEC_BC4S; break;
        case 0x4B: r.f = VK_FORMAT_R8G8_UNORM; r.dec = DEC_BC5; break; case 0x4C: r.f = VK_FORMAT_R8G8_SNORM; r.dec = DEC_BC5S; break;
        case 0x4D: r.f = VK_FORMAT_R8G8B8A8_UNORM; r.dec = DEC_BC7; break; case 0x4E: r.f = VK_FORMAT_R8G8B8A8_SRGB; r.dec = DEC_BC7; break;
        case 0x4F: r.f = VK_FORMAT_R16G16B16A16_SFLOAT; r.dec = DEC_BC6HS; break; case 0x50: r.f = VK_FORMAT_B10G11R11_UFLOAT_PACK32; r.dec = DEC_BC6H; break;
        case 0x99: r.noalpha = 1; /* fall through */ case 0x9A: r.f = VK_FORMAT_B8G8R8A8_UNORM; break;
        case 0x9B: r.noalpha = 1; /* fall through */ case 0x9C: r.f = VK_FORMAT_B8G8R8A8_SRGB; break;
        case 0x95: r.f = VK_FORMAT_B5G6R5_UNORM_PACK16; break;
        case 0x96: r.noalpha = 1; /* fall through */ case 0x97: r.f = VK_FORMAT_B5G5R5A1_UNORM_PACK16; break;
        case 0x98: r.f = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
    }
    return r;
}
enum clear_type { CLEAR_FLOAT, CLEAR_UINT, CLEAR_SINT };
static enum clear_type clear_color_type(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return CLEAR_UINT;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_SINT:
            return CLEAR_SINT;
        default: return CLEAR_FLOAT;
    }
}
// Vertex attribute formats (NVN format → Vulkan, *_UI2F / *_I2F → SCALED), 0 = none
// Maxwell fetches by the NVN format, then shaders read the resulting register bits.
// Select only a Vulkan format that produces those bits in the reflected type. The
// missing w is integer 1 or float 1.0 according to the *source* numeric class.
static VkFormat vfmt(unsigned fmt, unsigned ct) {
#define F(v) { [NVN_CT_FLOAT] = VK_FORMAT_##v }
#define U(v) { [NVN_CT_UINT] = VK_FORMAT_##v }
#define I(v) { [NVN_CT_INT] = VK_FORMAT_##v }
#define INT32(v) { [NVN_CT_UINT] = VK_FORMAT_##v##_UINT, [NVN_CT_INT] = VK_FORMAT_##v##_SINT }
#define FULL32 { VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT }
    static const VkFormat formats[][3] = {
        [NVN_FORMAT_R8] = F(R8_UNORM), F(R8_SNORM), U(R8_UINT), I(R8_SINT),
        F(R16_SFLOAT), F(R16_UNORM), F(R16_SNORM), U(R16_UINT), I(R16_SINT),
        F(R32_SFLOAT), INT32(R32), INT32(R32),
        [NVN_FORMAT_RG8] = F(R8G8_UNORM), F(R8G8_SNORM), U(R8G8_UINT), I(R8G8_SINT),
        F(R16G16_SFLOAT), F(R16G16_UNORM), F(R16G16_SNORM), U(R16G16_UINT), I(R16G16_SINT),
        F(R32G32_SFLOAT), INT32(R32G32), INT32(R32G32),
        [NVN_FORMAT_RGB8] = F(R8G8B8_UNORM), F(R8G8B8_SNORM), U(R8G8B8_UINT), I(R8G8B8_SINT),
        F(R16G16B16_SFLOAT), F(R16G16B16_UNORM), F(R16G16B16_SNORM), U(R16G16B16_UINT), I(R16G16B16_SINT),
        F(R32G32B32_SFLOAT), INT32(R32G32B32), INT32(R32G32B32),
        [NVN_FORMAT_RGBA8] = F(R8G8B8A8_UNORM), F(R8G8B8A8_SNORM), U(R8G8B8A8_UINT), I(R8G8B8A8_SINT),
        F(R16G16B16A16_SFLOAT), F(R16G16B16A16_UNORM), F(R16G16B16A16_SNORM), U(R16G16B16A16_UINT), I(R16G16B16A16_SINT),
        FULL32, FULL32, FULL32,
        [NVN_FORMAT_RGB10A2] = F(A2B10G10R10_UNORM_PACK32), U(A2B10G10R10_UINT_PACK32), F(B10G11R11_UFLOAT_PACK32),
        // Vulkan has no 32-bit scaled vertex formats.
        [NVN_FORMAT_R8_UI2F] = F(R8_USCALED), F(R8_SSCALED), F(R16_USCALED), F(R16_SSCALED),
        [NVN_FORMAT_RG8_UI2F] = F(R8G8_USCALED), F(R8G8_SSCALED), F(R16G16_USCALED), F(R16G16_SSCALED),
        [NVN_FORMAT_RGB8_UI2F] = F(R8G8B8_USCALED), F(R8G8B8_SSCALED), F(R16G16B16_USCALED), F(R16G16B16_SSCALED),
        [NVN_FORMAT_RGBA8_UI2F] = F(R8G8B8A8_USCALED), F(R8G8B8A8_SSCALED), F(R16G16B16A16_USCALED), F(R16G16B16A16_SSCALED),
        [NVN_FORMAT_RGB10A2SN] = F(A2B10G10R10_SNORM_PACK32), I(A2B10G10R10_SINT_PACK32),
        F(A2B10G10R10_USCALED_PACK32), F(A2B10G10R10_SSCALED_PACK32),
        [NVN_FORMAT_BGRA8] = F(B8G8R8A8_UNORM),
    };
#undef FULL32
#undef INT32
#undef I
#undef U
#undef F
    return fmt < sizeof formats / sizeof *formats && ct <= NVN_CT_INT ? formats[fmt][ct] : VK_FORMAT_UNDEFINED;
}

// BC1 / BC2 / BC3 → RGBA8 (Mali has no BC sampling); BC4-7 by bcdec
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#define BCDECDEF static
#include "../../../third_party/bcdec/bcdec.h"
static void bc_color(const uint8_t *b, uint8_t out[16][4], int alpha1) {
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8); uint8_t p[4][4];
    for (int i = 0; i < 2; i++) { uint16_t c = i ? c1 : c0; p[i][0] = (uint8_t)((c >> 11) * 255 / 31); p[i][1] = (uint8_t)((c >> 5 & 63) * 255 / 63); p[i][2] = (uint8_t)((c & 31) * 255 / 31); p[i][3] = 255; }
    if (c0 > c1 || !alpha1) for (int k = 0; k < 3; k++) { p[2][k] = (uint8_t)((2 * p[0][k] + p[1][k]) / 3); p[3][k] = (uint8_t)((p[0][k] + 2 * p[1][k]) / 3); }
    else for (int k = 0; k < 3; k++) { p[2][k] = (uint8_t)((p[0][k] + p[1][k]) / 2); p[3][k] = 0; }
    p[2][3] = 255; p[3][3] = c0 > c1 || !alpha1 ? 255 : 0;
    uint32_t idx = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(out[i], p[idx >> (2 * i) & 3], 4);
}
static void bc_spread(uint8_t px[16][8]) { for (int i = 15; i >= 0; i--) memmove(px[i], (uint8_t *)px + 4 * i, 4); }   // 16 packed RGBA8 pixels → one per 8-byte slot
static void bc_decode(int dec, const uint8_t *src, size_t src_pitch, uint32_t w, uint32_t h, uint8_t *dst) {   // dst: w * h * dec_bpp(dec)
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = dec == DEC_BC1 || dec == DEC_BC4 || dec == DEC_BC4S ? 8 : 16, pb = (uint32_t)dec_bpp(dec);
    for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
        const uint8_t *b = src + by * src_pitch + bx * bs; uint8_t px[16][8];
        if (dec == DEC_BC1) bc_color(b, (uint8_t (*)[4])px, 1), bc_spread(px);
        else if (dec == DEC_BC4 || dec == DEC_BC4S || dec == DEC_BC5 || dec == DEC_BC5S) {
            int sg = dec == DEC_BC4S || dec == DEC_BC5S, two = dec == DEC_BC5 || dec == DEC_BC5S; uint8_t rg[16][2] = {{0}}, r1[16] = {0};
            if (two) bcdec_bc5(b, rg, 8, sg); else bcdec_bc4(b, r1, 4, sg);   // pitch in bytes: rows of 4 RG / R pixels
            for (int i = 0; i < 16; i++) { px[i][0] = two ? rg[i][0] : r1[i]; px[i][1] = two ? rg[i][1] : 0; px[i][2] = 0; px[i][3] = sg ? 127 : 255; }
        } else if (dec == DEC_BC7) bcdec_bc7(b, px, 32);   // RGBA8 rows of 4 pixels, in a block buffer of 8-byte pixels: pitch 32 bytes
        else if (dec == DEC_BC6H || dec == DEC_BC6HS) {
            uint16_t rgb[16][3]; bcdec_bc6h_half(b, rgb, 12, dec == DEC_BC6HS);   // pitch in halves
            for (int i = 0; i < 16; i++) {
                if (dec == DEC_BC6HS) { uint16_t q[4] = {rgb[i][0], rgb[i][1], rgb[i][2], 0x3C00}; memcpy(px[i], q, 8); continue; }
                // half → unsigned 11 / 10-bit floats: same 5-bit exponent and bias, the mantissa cut to 6 / 5 bits; negative values (none in unsigned BC6H) as 0
                uint32_t r11 = rgb[i][0] & 0x8000 ? 0 : rgb[i][0] >> 4, g11 = rgb[i][1] & 0x8000 ? 0 : rgb[i][1] >> 4, b10 = rgb[i][2] & 0x8000 ? 0 : rgb[i][2] >> 5;
                uint32_t pk = (r11 & 0x7ff) | (g11 & 0x7ff) << 11 | (b10 & 0x3ff) << 22; memcpy(px[i], &pk, 4);
            }
        } else {
            bc_color(b + 8, (uint8_t (*)[4])px, 0); bc_spread(px);
            if (dec == DEC_BC2) for (int i = 0; i < 16; i++) { int a = b[i / 2] >> (4 * (i & 1)) & 15; px[i][3] = (uint8_t)(a * 17); }
            else {
                uint8_t a[8] = {b[0], b[1]};
                if (a[0] > a[1]) for (int i = 2; i < 8; i++) a[i] = (uint8_t)(((8 - i) * a[0] + (i - 1) * a[1]) / 7);
                else { for (int i = 2; i < 6; i++) a[i] = (uint8_t)(((6 - i) * a[0] + (i - 1) * a[1]) / 5); a[6] = 0; a[7] = 255; }
                uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) px[i][3] = a[bits >> (3 * i) & 7];
            }
        }
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            uint32_t X = bx * 4 + x, Y = by * 4 + y;
            if (X < w && Y < h) memcpy(dst + ((size_t)Y * w + X) * pb, dec == DEC_BC7 ? (const uint8_t *)px + y * 32 + x * 4 : px[y * 4 + x], pb);
        }
    }
}

// ---------- Images ----------
typedef struct {
    VkFormat format; VkImageViewType type; VkImageUsageFlags usage;
    VkComponentMapping components; VkImageSubresourceRange range;
} image_view_key;
typedef struct { image_view_key key; VkImageView v; } vview;
typedef struct block_view block_view;
struct vtex {
    uint64_t serial; VkImage img; VkSampleCountFlagBits samples; VkImageUsageFlags usage; VkDeviceMemory m; int blk; VkDeviceSize moff, msize; fmt_t f; int nvn_fmt, target;
    uint32_t w, h, d, levels, layers; VkImageAspectFlags aspect; int uploaded; _Atomic int gpu;
    int dirty;   // the CPU wrote its storage since the last upload (WriteTexels, a movie frame, a flushed pool range): uploaded again at its next use, unless the GPU renders into it
    const uint8_t *mem; uint64_t len;   // its storage in guest memory
    uint64_t stamp; const void *span;   // uploaded from a dirty-tracked pool (nvn_dirty.c): the write generation then and the tracked range, re-uploaded when the game writes it
    int sparse; uint64_t uploaded_mappings, seen_mappings, seen_writes;
    vview *views; int nviews;
    VkImageView window_view; _Atomic unsigned present_refs;  // Window submissions have their own fences.
    block_view *block_views;
    struct vtex *next;
    uint64_t dead;   // released by the game: freed once the GPU has finished the submission that was being recorded then (fence dead)
    uint32_t linrow, linbpb;   // a linear 2D texture (LINEAR / LINEAR_RENDER_TARGET) the CPU can read: its row pitch and bytes per texel; what passes render into it goes back to its storage (lin_writeback)
};
static vtex **g_gb; static int g_ngb, g_cgb; static pthread_mutex_t g_gbmu = PTHREAD_MUTEX_INITIALIZER;
#define NHASH 4096
static vtex *g_tex[NHASH];
static vtex *tex_find(uint64_t ser) { for (vtex *v = g_tex[ser % NHASH]; v; v = v->next) if (v->serial == ser) return v; return NULL; }
static uint64_t *g_cpu_written; static int g_ncpu, g_ccpu;   // serials WriteTexels wrote (mtl_tex_cpu_written)
static int cpu_written(uint64_t ser) { for (int i = 0; i < g_ncpu; i++) if (g_cpu_written[i] == ser) return 1; return 0; }

static int g_plog;   // DM_VK_PASSLOG=<frame>[+]: that frame's (or every frame's from there) passes, draws, copies, clears and uploads are logged (diagnostics)
static _Atomic long g_passdump_request;
static long g_passdump_left;
// Diagnostics: DM_VK_SKIP=<mask> leaves out kinds of GPU work, to find which one a lost device comes from: 1 buffer-to-texture copies, 2 texture copies, 4 texture-to-buffer
// copies, 8 texture clears, 16 texture uploads, 32 draws, 64 attachment clears
enum { SK_B2T = 1, SK_T2T = 2, SK_T2B = 4, SK_CLEAR = 8, SK_UPLOAD = 16, SK_DRAW = 32, SK_ACLEAR = 64 };
static int vk_skip(void) { static int m = -1; if (m < 0) { const char *e = getenv("DM_VK_SKIP"); m = e ? (int)strtol(e, NULL, 0) : 0; if (m) LOG("DM_VK_SKIP=%#x: GPU work of those kinds is left out", m); } return m; }
static void tex_upload(vtex *v, const tex_t *t);
// Reuse Metal's tex_init_fill rules: initial storage also belongs to a texture in
// a CPU_UNCACHED pool, or a CPU-cached pool flushed before first use. The former
// needs no flush (Link's Awakening's linear R8 font atlas). Sparse, multisample
// and depth images stay GPU textures; compression tags exclude only CPU_NO_ACCESS.
int nvn_flushed(const void *p, uint64_t n); // shared recorder's flushed-range ledger
static int mem_any(const uint8_t *p, uint64_t n) { uint64_t i = 0, x; for (; i + 8 <= n; i += 8) { memcpy(&x, p + i, 8); if (x) return 1; } for (; i < n; i++) if (p[i]) return 1; return 0; }
static int init_fill(const tex_t *t, const fmt_t *f) {
    if (!t->mem || !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || t->has_layout) return 0;
    if (t->d.samples > 1 || f->depth || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    int no_cpu = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) != 0;
    int uncached = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED) != 0;
    if (no_cpu && (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE)) return 0;
    static int flush_fill = -1;
    if (flush_fill < 0) { const char *e = getenv("DM_NVN_FLUSH_FILL"); flush_fill = !e || atoi(e) != 0; }
    if (!no_cpu && !uncached && (!flush_fill || !nvn_flushed(t->mem, t->L.size))) return 0;
    return mem_any(t->mem, t->L.size);
}
// Vulkan fixes usage at allocation. NVN may register an image after drawing into it
// or bind an ordinary texture descriptor as an image. Reserve storage usage only
// when the complete image configuration supports both its original roles and storage.
static int storage_compatible(const VkImageCreateInfo *ii) {
    if (ii->samples != VK_SAMPLE_COUNT_1_BIT && !g_storage_ms) return 0;
    VkImageFormatProperties limits;
    if (vkGetPhysicalDeviceImageFormatProperties(g_pd, ii->format, ii->imageType, ii->tiling,
            ii->usage | VK_IMAGE_USAGE_STORAGE_BIT, ii->flags, &limits) != VK_SUCCESS) return 0;
    return ii->extent.width <= limits.maxExtent.width && ii->extent.height <= limits.maxExtent.height &&
           ii->extent.depth <= limits.maxExtent.depth && ii->mipLevels <= limits.maxMipLevels &&
           ii->arrayLayers <= limits.maxArrayLayers && (limits.sampleCounts & ii->samples);
}
#include "public_texels.inc"
#include "sparse_upload.inc"

// Caller holds g_rmu through initial upload: pool finalization uses the same
// lock before guest storage can disappear. A registry lookup must stay locked
// from its live-ID check through this resolution.
static vtex *tex_get_locked(const tex_t *t, int for_rt) {
    vtex *v = tex_find(t->serial);
    if (v) {
        if (for_rt) v->gpu = 1;
        else if (!v->gpu && t->mem && (v->dirty || sparse_changed(v,t) || (v->span && nvn_dirty_since_span(v->span, t->mem, t->L.size, v->stamp)))) { v->dirty = 0; tex_upload(v, t); }   // never over what the GPU drew: a flush of the pool range around a render target is not new content
        return v;
    }
    fmt_t f = fmt_of(t->d.format);
    if (!f.f) { static int n; if (n++ < 8) LOG("[RED] vk: texture format %#x has no Vulkan format yet (serial %llu)", t->d.format, (unsigned long long)t->serial); return NULL; }
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) { RED_ONCE(3, "buffer textures are not implemented yet"); return NULL; }
    v = calloc(1, sizeof *v); v->serial = t->serial; v->f = f; v->nvn_fmt = t->d.format; v->target = t->d.target; v->mem = t->mem; v->len = t->L.size;
    v->w = t->d.width ? t->d.width : 1; v->h = t->d.height ? t->d.height : 1; v->levels = t->d.levels ? t->d.levels : 1;
    // Existing normalized 1D exports use a one-row 2D interface, as on Metal.
    // Native 1D exports are adapted at module load to share this same storage.
    if (t->d.target == NVN_TEXTURE_TARGET_1D || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY) v->h = 1;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, cube = t->d.target == NVN_TEXTURE_TARGET_CUBEMAP || t->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    v->d = is3d ? (t->d.depth ? t->d.depth : 1) : 1;
    v->layers = is3d ? 1 : t->L.layers ? t->L.layers : t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ? (t->d.depth ? t->d.depth : 1) : cube ? 6 : 1;
    if (t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY && !t->L.layers) v->layers = t->d.height ? t->d.height : 1;
    v->aspect = f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, f.f, &fp);
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkSampleCountFlagBits sc = t->d.samples > 1 ? (VkSampleCountFlagBits)t->d.samples : VK_SAMPLE_COUNT_1_BIT;
    v->samples = sc;
    // NVN permits a six-face square 2D array to be sampled through a cube view.
    int cube_views = cube || (t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY && v->w == v->h &&
                              v->layers >= 6 && sc == VK_SAMPLE_COUNT_1_BIT);
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (cube_views ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) | (is3d ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0),
                            is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                            f.f, {v->w, t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ? 1 : v->h, v->d}, v->levels, v->layers, sc, VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (sc != VK_SAMPLE_COUNT_1_BIT) ii.mipLevels = 1;
    if (!f.dec && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && storage_compatible(&ii))
        ii.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkResult r = vkCreateImage(g_dev, &ii, NULL, &v->img);
    // maxResourceSize can only be checked by creation. Keep a valid sampled/target
    // allocation if the driver's storage-capable layout would exceed its size limit.
    if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY && ii.usage != u) {
        ii.usage = u;
        r = vkCreateImage(g_dev, &ii, NULL, &v->img);
    }
    v->usage = ii.usage;
    if (r != VK_SUCCESS) { LOG("[RED] vk: image %ux%ux%u x%u levels %u format %#x target %d not created (%d)", v->w, v->h, v->d, v->layers, v->levels, t->d.format, t->d.target, (int)r); free(v); return NULL; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, v->img, &mr); VkDeviceSize off;
    v->blk = img_mem(mr, &v->m, &off); v->moff = off; v->msize = mr.size; VKCHECK(vkBindImageMemory(g_dev, v->img, v->m, off));
    if ((t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) && t->mem && t->d.target == NVN_TEXTURE_TARGET_2D && v->levels == 1 && v->layers == 1 && !f.dec && !f.depth && sc == VK_SAMPLE_COUNT_1_BIT) {
        int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); uint32_t row = t->L.layer_size / v->h;
        if (bpb && bw == 1 && bh == 1 && row % bpb == 0 && (uint64_t)row * v->h <= t->L.size) { v->linrow = row; v->linbpb = (uint32_t)bpb; }
    }
    to_general(C->up, v->img, v->aspect); C->up_used = 1;
    v->gpu = for_rt;
    v->next = g_tex[v->serial % NHASH]; g_tex[v->serial % NHASH] = v;
    v->sparse=t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) && nvn_sparse_pool(t->pool);
    // Match Metal's tex_from_storage: ordinary sampled textures in a sparse
    // virtual pool also have CPU data without an explicit packaged layout.
    int sparse_sampled=v->sparse && !(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET |
                                                    NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE));
    int packaged = (t->has_layout || cpu_written(t->serial) || sparse_sampled || init_fill(t, &f)) && t->mem && !for_rt;
    if (packaged) tex_upload(v, t);
    else if (!for_rt) {   // contents come from the GPU (copies, render passes): start cleared, as a new texture's storage would read on hardware after a clear
        VkClearColorValue z = {{0}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        if (!f.depth) vkCmdClearColorImage(C->up, v->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
    }
    return v;
}
static vtex *tex_get(const tex_t *t, int for_rt) {
    pthread_mutex_lock(&g_rmu);
    vtex *v = tex_get_locked(t, for_rt);
    pthread_mutex_unlock(&g_rmu);
    return v;
}
static void pass_end(void);
// Deswizzle each level into the ring. Initial uploads lead the submission,
// while updates to an existing image preserve their command order.
static void tex_upload(vtex *v, const tex_t *t) {
    if (vk_skip() & SK_UPLOAD) return;
    if (g_plog) LOG("plog:  upload %llu fmt %#x %ux%ux%u levels %u layers %u target %d", (unsigned long long)t->serial, t->d.format, v->w, v->h, v->d, v->levels, v->layers, t->d.target);
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { RED_ONCE(4, "texture format %#x: block size unknown, not uploaded", t->d.format); return; }
    if (v->sparse) {
        v->uploaded_mappings=v->seen_mappings=nvn_sparse_gen();
        v->stamp=v->seen_writes=nvn_dirty_now();
        nvn_sparse_protect(t->pool,(uintptr_t)t->mem,t->L.size);
    }
    // Re-uploads must follow commands that consumed the previous contents.
    // Only the first upload can run in the submission's leading upload buffer.
    VkCommandBuffer command=C->up;
    if (v->uploaded) { pass_end();command=C->cb;barrier_all(command); }
    for (unsigned lv = 0; lv < v->levels; lv++) {
        uint32_t w = v->w >> lv ? v->w >> lv : 1, h = v->h >> lv ? v->h >> lv : 1, d = v->d >> lv ? v->d >> lv : 1;
        if (t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY) h = 1;
        uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh; size_t pitch = (size_t)bx * bpb, lsz = pitch * by * d;
        for (unsigned ly = 0; ly < v->layers; ly++) {
            size_t out = v->f.dec ? (size_t)w * h * dec_bpp(v->f.dec) * d : lsz;
            VkBufferImageCopy region = {0, 0, 0, {v->aspect, lv, ly, 1}, {0, 0, 0}, {w, h, d}};
            texel_planes planes = transfer_planes(v, region, out);
            VkDeviceSize o = ring_alloc(planes.bytes, 16);
            uint8_t *dst = C->ringp + o, *tmp = dst;
            if (v->f.dec || planes.depth_bytes) {
                tmp = malloc(lsz);
                if (!tmp) die("Vulkan: cannot allocate texture conversion input");
            }
            texel_extent ext = {.bytes=pitch, .rows=by, .nz=d, .layer=ly};
            int rc;
            if (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))
                rc = !storage_texels(t, lv, tmp, pitch, pitch * by, ext, 0);
            else if (v->sparse) rc = sparse_deswizzle(t,lv,ly,tmp,pitch);
            else rc = nvn_deswizzle(&t->d, lv, ly, t->mem, tmp, pitch);
            if (rc) { RED_ONCE(5, "texture format %#x target %d: deswizzle failed", t->d.format, t->d.target); if (tmp != dst) free(tmp); continue; }
            if (v->f.dec) for (uint32_t z = 0; z < d; z++)
                bc_decode(v->f.dec, tmp + pitch * by * z, pitch, w, h, dst + (size_t)w * h * dec_bpp(v->f.dec) * z);
            else if (planes.depth_bytes)
                transfer_rows(v, ext, w, tmp, NULL, pitch, pitch * by, dst, planes.depth_bytes, planes.stencil);
            if (tmp != dst) free(tmp);
            for (unsigned i=0; i<planes.count; ++i) planes.copies[i].bufferOffset += o;
            vkCmdCopyBufferToImage(command, C->ring, v->img, VK_IMAGE_LAYOUT_GENERAL, planes.count, planes.copies);
        }
    }
    // Execution order alone does not expose transferred texels to shaders.
    // Publish the whole image after
    // its mip/layer copies, including CPU re-uploads (ASTC sampled as zero without it).
    // A later command can also copy the image or attach it as a render target.
    VkImageMemoryBarrier ready = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        v->img, {v->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, NULL, 0, NULL, 1, &ready);
    if (command==C->up) C->up_used=1;
    v->uploaded=1;
    // As nvn_mtl.m does for packaged textures: the storage is write-protected after the upload, and a game write to it (streamed texture data arriving after the texture was
    // made, as Super Mario Odyssey's normal maps do; noise until then) brings a new upload at the texture's next use (tex_get)
    if (t->mem && !v->sparse && !v->gpu && nvn_dirty_tracked(t->mem, t->L.size)) { nvn_dirty_protect(t->mem, t->L.size); v->stamp = nvn_dirty_now(); v->span = nvn_dirty_span(t->mem, t->L.size); }
}
static void block_views_release(vtex *v);
static void tex_free(vtex *v) {   // released by the game: draws already recorded (and the one being recorded) may still use it
    block_views_release(v);
    pthread_mutex_lock(&g_gbmu);
    v->dead = g_fence + 1;
    if (g_ngb == g_cgb) { g_cgb = g_cgb ? 2 * g_cgb : 64; g_gb = realloc(g_gb, g_cgb * sizeof *g_gb); }
    g_gb[g_ngb++] = v;
    pthread_mutex_unlock(&g_gbmu);
}
static void garbage_free(uint64_t done) {
    pthread_mutex_lock(&g_gbmu);
    for (int i = 0; i < g_ngb;) {
        vtex *v = g_gb[i];
        if (v->dead > done || atomic_load(&v->present_refs)) { i++; continue; }
        if (v->window_view) vkDestroyImageView(g_dev, v->window_view, NULL);
        for (int k = 0; k < v->nviews; k++) vkDestroyImageView(g_dev, v->views[k].v, NULL);
        vkDestroyImage(g_dev, v->img, NULL); img_mem_free(v->blk, v->m, v->moff, v->msize); free(v->views); free(v);
        g_gb[i] = g_gb[--g_ngb];
    }
    pthread_mutex_unlock(&g_gbmu);
}
// A view of the image: type and format from the NVN view (or the texture), levels and layers, swizzle (the format's missing alpha, the texture's and the view's swizzles)
enum { VIEW_SAMPLED, VIEW_ATTACHMENT, VIEW_STORAGE };
static fmt_t view_format(const vtex *v, const view_t *view, int has_view) {
    if (!has_view || !view->set_format) return v->f;
    fmt_t f = fmt_of(view->format);
    // The decoder preserves encoded RGB bytes. Linear and sRGB registrations
    // of the same BC encoding choose different views of that one allocation.
    if (!f.f || f.dec != v->f.dec) return v->f;
    return f;
}
static int depth_red_view(const vtex *v, fmt_t requested) {
    return (v->f.f == VK_FORMAT_D32_SFLOAT && requested.f == VK_FORMAT_R32_SFLOAT) ||
           (v->f.f == VK_FORMAT_D16_UNORM && requested.f == VK_FORMAT_R16_UNORM);
}
static int sampled_stencil(const vtex *v, const tex_t *texture, const view_t *view, int has_view) {
    return v->f.stencil && (has_view && view->set_dsmode ? view->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : texture->stencil);
}
static void sampled_swizzle(uint8_t swz[4], fmt_t f, const tex_t *t, const view_t *vw, int has_vw) {
    if (t->set_swz) for (int i = 0; i < 4; i++) swz[i] = (uint8_t)(t->swz >> (3 * i) & 7);
    if (has_vw && vw->set_swizzle) memcpy(swz, vw->swz, 4);
    if (f.noalpha) for (int i = 0; i < 4; i++) if (swz[i] == 5) swz[i] = 1;
}
static int compatible_dimension(const vtex *v, int view) {
    if (view < 0) return 1;
    if (view == VK_IMAGE_VIEW_TYPE_1D || view == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
        (v->target == NVN_TEXTURE_TARGET_3D) != (view == VK_IMAGE_VIEW_TYPE_3D)) return 0;
    return (view != VK_IMAGE_VIEW_TYPE_CUBE && view != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ||
           (v->layers >= 6 && v->w == v->h && v->samples == VK_SAMPLE_COUNT_1_BIT);
}
static VkImageView tex_view_format(vtex *v, const tex_t *t, const view_t *vw, int has_vw, int want_type, int purpose, fmt_t f) {
    int rt = purpose == VIEW_ATTACHMENT, storage = purpose == VIEW_STORAGE;
    if (purpose == VIEW_SAMPLED && v->f.dec && has_vw && vw->set_format &&
        (vw->format < NVN_FORMAT_RGB_DXT1 || vw->format > NVN_FORMAT_BPTC_UFLOAT)) {
        // The decoded image has pixels, not the encoded block words. Match
        // Metal's empty slot for a sampled raw-block alias.
        RED_ONCE(120, "sampled raw-block view of BC format %#x as %#x uses an empty slot", v->nvn_fmt, vw->format);
        return VK_NULL_HANDLE;
    }
    int target = has_vw && vw->set_target ? vw->target : t->d.target;
    uint32_t bl = has_vw && vw->set_levels ? vw->base_level : 0;
    if (bl >= v->levels) return VK_NULL_HANDLE;
    uint32_t nl = has_vw && vw->set_levels ? vw->nlevels : 0;
    if (!nl || nl > v->levels - bl) nl = v->levels - bl;
    if (rt || storage) nl = 1;
    // A 3D attachment selects a depth slice at this mip. A 3D sampled view
    // exposes the volume through Vulkan's single array layer.
    uint32_t total = rt && v->target == NVN_TEXTURE_TARGET_3D ? (v->d >> bl) : v->layers;
    if (!total) total = 1;
    uint32_t b0 = has_vw && vw->set_layers ? vw->min_layer : 0;
    if (b0 >= total) return VK_NULL_HANDLE;
    uint32_t nly = has_vw && vw->set_layers ? vw->nlayers : 0;
    if (!nly || nly > total - b0) nly = total - b0;
    if (!rt) {
        // Resolve the guest descriptor before fitting it to the shader. A
        // cube range that does not name whole faces may count entire cubes.
        int cube = target == NVN_TEXTURE_TARGET_CUBEMAP || target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
        if (cube && nly % 6 && b0 + nly <= total / 6) { b0 *= 6; nly *= 6; }
        if (target == NVN_TEXTURE_TARGET_CUBEMAP && nly >= 6 && !(nly % 6)) nly = 6;
        if (target == NVN_TEXTURE_TARGET_2D || target == NVN_TEXTURE_TARGET_1D) nly = 1;
    }
    uint8_t swz[4] = {2, 3, 4, 5};
    if (!rt && !storage) sampled_swizzle(swz, f, t, vw, has_vw);
    if (purpose == VIEW_SAMPLED && depth_red_view(v, f)) {
        // Depth and colour formats cannot alias in VkImageView. Sample the
        // original depth plane and compose (R,0,0,1) with the guest swizzle.
        f = v->f;
        for (int i = 0; i < 4; i++) {
            if (swz[i] == NVN_TEXTURE_SWIZZLE_G || swz[i] == NVN_TEXTURE_SWIZZLE_B) swz[i] = NVN_TEXTURE_SWIZZLE_ZERO;
            if (swz[i] == NVN_TEXTURE_SWIZZLE_A) swz[i] = NVN_TEXTURE_SWIZZLE_ONE;
        }
    }
    if ((v->f.depth || f.depth) && f.f != v->f.f) {
        RED_ONCE(117, "incompatible depth image view (format %d as %d)", v->f.f, f.f);
        return VK_NULL_HANDLE;
    }
    VkImageViewType vt;
    if (want_type >= 0) vt = (VkImageViewType)want_type;
    else switch (target) {
        case NVN_TEXTURE_TARGET_1D: vt = VK_IMAGE_VIEW_TYPE_2D; break; case NVN_TEXTURE_TARGET_1D_ARRAY: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case NVN_TEXTURE_TARGET_3D: vt = VK_IMAGE_VIEW_TYPE_3D; break;
        case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case NVN_TEXTURE_TARGET_CUBEMAP: vt = VK_IMAGE_VIEW_TYPE_CUBE; break; case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
        default: vt = VK_IMAGE_VIEW_TYPE_2D; break;
    }
    // A render target is a 2D view of its layer (a 2D array for several): Vulkan takes no cube or 3D view as an attachment. A cube face drawn to was a cube view of 6 layers
    // from that face on, past the image's end for faces 1-5 (Super Mario Odyssey's environment probes, its first frame in a kingdom: the Mali-G57 lost the device). A 3D
    // texture's slice is a 2D view of the image made 2D-array compatible, its slice as the layer
    if (rt) {
        if (v->target == NVN_TEXTURE_TARGET_3D) vt = nly > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY || vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY) vt = nly > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
    if ((vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_2D) && nly > 1) nly = 1;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        if (nly < 6 || v->samples != VK_SAMPLE_COUNT_1_BIT || v->w != v->h) return VK_NULL_HANDLE;
        nly = vt == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : nly - nly % 6;
    }
    VkImageAspectFlags asp = v->aspect;
    if (purpose == VIEW_SAMPLED && v->f.depth)
        asp = sampled_stencil(v, t, vw, has_vw) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkComponentMapping components = {(VkComponentSwizzle)(swz[0]+1), (VkComponentSwizzle)(swz[1]+1),
                                     (VkComponentSwizzle)(swz[2]+1), (VkComponentSwizzle)(swz[3]+1)};
    if (storage) components = (VkComponentMapping){0};
    // A sampled sRGB alias of a storage-capable UNORM image need only support
    // sampling. Do not make aliased formats inherit unrelated image usages.
    VkImageUsageFlags usage = rt ? (v->f.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                                 : storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!(v->usage & usage)) return VK_NULL_HANDLE;
    image_view_key key = {f.f, vt, usage, components, {asp, bl, nl, b0, nly}};
    for (int i = 0; i < v->nviews; i++) if (!memcmp(&v->views[i].key, &key, sizeof key)) return v->views[i].v;
    if (f.f != v->f.f) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, f.f, &fp);
        VkFormatFeatureFlags need = rt ? (v->f.depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
                                       : storage ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (!(fp.optimalTilingFeatures & need)) return VK_NULL_HANDLE;
    }
    VkImageViewUsageCreateInfo view_usage = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, usage};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &view_usage, 0, v->img, vt, f.f, components, key.range};
    VkImageView view; VkResult r = vkCreateImageView(g_dev, &vi, NULL, &view);
    if (r != VK_SUCCESS) { static int n; if (n++ < 8) LOG("[RED] vk: image view (format %d type %d levels %u+%u layers %u+%u) failed (%d)", f.f, vt, bl, nl, b0, nly, (int)r); return VK_NULL_HANDLE; }
    v->views = realloc(v->views, (v->nviews + 1) * sizeof *v->views); v->views[v->nviews++] = (vview){key, view};
    return view;
}
static VkImageView tex_view(vtex *v, const tex_t *t, const view_t *vw, int has_vw, int want_type, int purpose) {
    return tex_view_format(v, t, vw, has_vw, want_type, purpose, view_format(v, vw, has_vw));
}

// ---------- Registrations (game threads) ----------
typedef struct { tex_t t; view_t v; uint8_t ok, has_v; } treg_t;
static treg_t *g_treg; static int g_ntreg;
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static int g_nsreg;
static _Atomic int g_has_reduction_samplers;
static uint64_t g_registration_generation=1; // guarded by g_rmu
static vtex *registered_texture(int id, treg_t *registration) {
    pthread_mutex_lock(&g_rmu);
    *registration = id > 0 && id < g_ntreg ? g_treg[id] : (treg_t){0};
    vtex *v = registration->ok ? tex_get_locked(&registration->t, 0) : NULL;
    pthread_mutex_unlock(&g_rmu);
    return v;
}
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0) return;
    pthread_mutex_lock(&g_rmu);
    g_registration_generation++;
    if (!t) { if (id < g_ntreg) g_treg[id] = (treg_t){0}; pthread_mutex_unlock(&g_rmu); return; }
    if (id >= g_ntreg) { int n = id + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    g_treg[id].t = *t; g_treg[id].ok = 1; g_treg[id].has_v = v != NULL; if (v) g_treg[id].v = *v;
    pthread_mutex_unlock(&g_rmu);
}
void mtl_reg_image(int id, const tex_t *t, const view_t *v) {
    // Texture and image handles address the same NVN descriptor pool.
    mtl_reg_tex(id, t, v);
}
void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0) return;
    pthread_mutex_lock(&g_rmu);
    g_registration_generation++;
    if (id >= g_nsreg) { int n = id + 256; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg); g_nsreg = n; }
    g_sreg[id] = *p; g_sreg_ok[id] = 1;
    if (p->reduction == NVN_SAMPLER_REDUCTION_MIN || p->reduction == NVN_SAMPLER_REDUCTION_MAX) g_has_reduction_samplers = 1;
    pthread_mutex_unlock(&g_rmu);
}
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) {
    pthread_mutex_lock(&g_rmu);
    for (vtex **pp = &g_tex[t->serial % NHASH]; *pp; pp = &(*pp)->next) if ((*pp)->serial == t->serial) { vtex *v = *pp; *pp = v->next; pthread_mutex_unlock(&g_rmu); tex_free(v); return; }
    pthread_mutex_unlock(&g_rmu);
}
int mtl_tex_gpu_owned(uint64_t serial) {
    pthread_mutex_lock(&g_rmu);
    vtex *v = tex_find(serial);
    int owned = v && v->gpu;
    pthread_mutex_unlock(&g_rmu);
    return owned;
}
void mtl_tex_cpu_written(const tex_t *t) {
    pthread_mutex_lock(&g_rmu);
    vtex *v = tex_find(t->serial);
    if (v && v->gpu) { pthread_mutex_unlock(&g_rmu); return; }
    if (v) v->dirty = 1;
    if (!cpu_written(t->serial)) { if (g_ncpu == g_ccpu) { g_ccpu = g_ccpu ? 2 * g_ccpu : 64; g_cpu_written = realloc(g_cpu_written, g_ccpu * sizeof *g_cpu_written); } g_cpu_written[g_ncpu++] = t->serial; }
    pthread_mutex_unlock(&g_rmu);
}
void mtl_prog_release(prog_t *p) { (void)p; }

// Samplers, cached by their parameters
#include "../nvn_border.h"
enum { BORDER_SWIZZLED=1, BORDER_PACKED=2, BORDER_MIXED_FORMATS=4, BORDER_MIXED_NUMBERS=8, BORDER_FLOAT_AND_INT=16, BORDER_STENCIL=32 };
typedef struct { VkFormat format; unsigned flags; } border_view;
static border_view sampler_view(const vtex *v, const treg_t *tr) {
    fmt_t f = view_format(v,&tr->v,tr->has_v);
    border_view result = {depth_red_view(v,f) ? v->f.f : f.f};
    if (sampled_stencil(v,&tr->t,&tr->v,tr->has_v)) {
        result.format = v->f.f;
        result.flags |= BORDER_STENCIL;
    }
    if (!g_border_from_image && (f.noalpha || tr->t.set_swz || (tr->has_v && tr->v.set_swizzle))) {
        uint8_t swz[] = {2,3,4,5};
        sampled_swizzle(swz,f,&tr->t,&tr->v,tr->has_v);
        if (memcmp(swz,(uint8_t[]){2,3,4,5},4)) result.flags |= BORDER_SWIZZLED;
    }
    return result;
}
typedef struct { samp_par p; border_view view; VkSampler s; } scache;
static scache *g_sc; static int g_nsc;
static VkClearColorValue clear_value(const float rgba[4], enum clear_type type);
static VkSamplerAddressMode wrap(int w) {
    switch (w) { case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 4: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                 case 2: case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; }
}
static enum clear_type border_number(border_view view) {
    return view.flags & BORDER_STENCIL ? CLEAR_UINT : clear_color_type(view.format);
}
static VkSampler sampler_for(samp_par p, border_view view) {
    int border = p.ws == 4 || p.ws == 5 || p.wt == 4 || p.wt == 5 || p.wr == 4 || p.wr == 5;
    if (!border) view = (border_view){0};
    for (int i = 0; i < g_nsc; i++)
        if (g_sc[i].view.format == view.format && g_sc[i].view.flags == view.flags &&
            !memcmp(&g_sc[i].p, &p, sizeof p)) return g_sc[i].s;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = p.magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = p.minf == 1 || p.minf == 3 || p.minf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = p.minf == 4 || p.minf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = wrap(p.ws); si.addressModeV = wrap(p.wt); si.addressModeW = wrap(p.wr);
    float bias_limit = g_props.limits.maxSamplerLodBias;
    si.mipLodBias = fminf(fminf(15.99f, bias_limit), fmaxf(fmaxf(-16, -bias_limit), p.lod_bias));
    si.minLod = p.minf >= 2 ? fmaxf(0, p.lod_min) : 0;
    si.maxLod = p.minf >= 2 ? fmaxf(si.minLod, p.lod_max) : 0.25f;
    // Metal exposes whole-number anisotropy through its sampler descriptor.
    si.maxAnisotropy = fminf(g_max_anisotropy, fmaxf(1, floorf(p.aniso)));
    si.anisotropyEnable = si.maxAnisotropy > 1;
    if (p.cmp_mode) { si.compareEnable = VK_TRUE; si.compareOp = (VkCompareOp)(p.cmp_func ? p.cmp_func - 1 : 7); }
    VkSamplerCustomBorderColorCreateInfoEXT custom = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border && (view.flags & BORDER_FLOAT_AND_INT)) {
        // A single VkSampler cannot specify both integer and floating border
        // colors. Clamp only the border axes so every paired image remains valid.
        if (si.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (si.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (si.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        RED_ONCE(105, "independent sampler may select both integer and floating images, clamping border axes to edge");
    } else if (border) {
        int stock = nvn_stock_border(p.border);
        enum clear_type type = border_number(view);
        const VkBorderColor fixed[][3] = {
            {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE},
            {VK_BORDER_COLOR_INT_TRANSPARENT_BLACK,VK_BORDER_COLOR_INT_OPAQUE_BLACK,VK_BORDER_COLOR_INT_OPAQUE_WHITE}};
        si.borderColor = fixed[type != CLEAR_FLOAT][stock];
        if (memcmp(p.border, nvn_mtl_border[stock], sizeof p.border)) {
            int mixed = view.flags & BORDER_MIXED_FORMATS;
            int compatible = !mixed || (g_border_without_format && !(view.flags & (BORDER_PACKED | BORDER_MIXED_NUMBERS)));
            if (view.format != VK_FORMAT_UNDEFINED && !(view.flags & BORDER_SWIZZLED) && compatible && g_custom_border_count < g_custom_border_limit) {
                custom.format = mixed ? VK_FORMAT_UNDEFINED : view.format;
                custom.customBorderColor = clear_value(p.border,type);
                si.borderColor = type == CLEAR_FLOAT ? VK_BORDER_COLOR_FLOAT_CUSTOM_EXT : VK_BORDER_COLOR_INT_CUSTOM_EXT;
                si.pNext = &custom;
            } else {
                RED_ONCE(104, "custom border color unavailable for this sampler, using the nearest stock color");
            }
        }
    }
    int custom_border=si.pNext==&custom;
    VkSamplerReductionModeCreateInfo reduction={VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,si.pNext};
    if (g_minmax_filter && !p.cmp_mode &&
        (p.reduction==NVN_SAMPLER_REDUCTION_MIN || p.reduction==NVN_SAMPLER_REDUCTION_MAX)) {
        reduction.reductionMode=p.reduction==NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        si.pNext=&reduction;
        // Metal's reduction samplers always reduce across contributing mips.
        si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;
        if (p.minf<2) si.minLod=si.maxLod=0;
    }
    VkSampler s; if (vkCreateSampler(g_dev, &si, NULL, &s) != VK_SUCCESS) return g_dummy_samp;
    if (custom_border) ++g_custom_border_count;
    g_sc = realloc(g_sc, (g_nsc + 1) * sizeof *g_sc); g_sc[g_nsc++] = (scache){p, view, s};
    return s;
}

static VkSampler samp_get(int sid, border_view view, int reduce_in_shader) {
    if (sid <= 0 || sid >= g_nsreg || !g_sreg_ok[sid]) return g_dummy_samp;
    samp_par p = g_sreg[sid];
    if (reduce_in_shader && (p.reduction == NVN_SAMPLER_REDUCTION_MIN || p.reduction == NVN_SAMPLER_REDUCTION_MAX)) {
        p.minf = NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST; p.magf = NVN_MAG_FILTER_NEAREST;
        p.lod_min = p.lod_bias = 0; p.lod_max = VK_LOD_CLAMP_NONE; p.aniso = 1;
        p.reduction = NVN_SAMPLER_REDUCTION_AVERAGE;
    }
    return sampler_for(p,view);
}

// ---------- Shaders and pipelines ----------
enum { BT_UBO, BT_SSBO, BT_CIS, BT_TBUF, BT_UTB, BT_STB, BT_SIMG, BT_TEX, BT_SAMP };
static const VkDescriptorType descriptor_type[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
enum { K_UNK, K_CBUF, K_C1, K_SUPPORT, K_DRVCB, K_TEX, K_IMG, K_SBUF, K_TBUF, K_UBOP, K_TEXU, K_SAMPLER, K_SBUFU, K_TABLE_MAP, K_CAPTURE_RECORDS, K_CAPTURE_CONTROL, K_GEOMETRY, K_TEX_SLOTS, K_TEX_C2, K_TEX_POOL, K_POOL_BOUNDS, K_GMEM_MAP, K_GMEM_LOG, K_MESH };   // K_UBOP: a compute uniform buffer read through its address (dmsh_build.py ubo_ptr), bound as storage
enum { S_NONE, S_SAMP, S_SAMP_TEX, S_SAMP_ID, S_SAMPU, S_SAMP_SLOTS, S_SAMP_C2, S_SAMP_POOL };
static int sampler_source(const char *kind) {
    if (!strcmp(kind,"samp_pool")) return S_SAMP_POOL;
    if (!strcmp(kind,"samp_c2a")) return S_SAMP_C2;
    return !strcmp(kind,"samp")?S_SAMP:!strcmp(kind,"samp_tex")?S_SAMP_TEX:!strcmp(kind,"samp_id")?S_SAMP_ID:!strcmp(kind,"samp_ubo")?S_SAMPU:!strcmp(kind,"samp_slots")?S_SAMP_SLOTS:S_NONE;
}
typedef struct { uint32_t binding, count, storage_image, logical_count; uint8_t type, kind, skind, read_only, table, pool, texel_atomic, multisample, depth_image; int16_t slot; int16_t reg, sreg; int32_t off, soff, view, num, driver_sampler; dm_vk_split_handle twin; dm_vk_resource_shape shape; } sbind;
#include "spv_sampler_pairs.inc"
#include "spv_storage_texels.inc"
typedef struct vin_module {
    vertex_classes classes;
    VkShaderModule module;
    struct vin_module *next;
} vin_module;
typedef struct fragment_variant {
    uint8_t uint_mask, sint_mask, alpha;
    VkShaderModule module;
    struct fragment_variant *next;
} fragment_variant;
typedef struct shader shader_t;
struct shader {
    shader_t *buffer_variants, *next_buffer_variant, *table_variants, *next_table_variant;
    shader_t *wrapped_variants, *next_wrapped_variant;
    uint8_t wrapped[DM_VK_MAX_BINDINGS];
    int has_tables,logical_slots,raw_writes;
    sampler_pairs *sampler_images;
    texel_bias *texel_biases; size_t ntexel_biases; // sparse per-element prefixes of writable aliases
    sample_reduction *reductions; size_t nreductions;
    uint32_t buffer_bias[DM_VK_MAX_BINDINGS]; // byte prefix before each reflected storage binding
    vin_module *vertex_variants; int depth_zero, draw_push, layer_output;
    shader_t *capture_variant;
    dm_vk_feedback *feedback_layout;
    int capture_entry;
    dm_vk_geometry geometry;
    dm_vk_meshstage mesh;
    char key[32]; int stage, guest_stage;   // 0 vertex, 1 fragment, 5 compute (NVN stage numbers)
    VkShaderModule mod; fragment_variant *fragment_variants; fragment_outputs outputs; VkDescriptorSetLayout dsl;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup;
    sbind b[DM_VK_MAX_BINDINGS]; int nb; uint8_t vin[32]; uint32_t vin_mask; int pack_locations;   // vin: component type per vertex input location (0 float, 1 uint, 2 int)
    uint8_t *c1; uint32_t c1_len; uint32_t tg[3];
    VkPipelineLayout cpl; VkPipeline cpp;   // compute shaders: their layout and pipeline (dispatch)
};
static shader_t **g_sh; static int g_nsh;
static int g_depth_zero;   // NVN depth mode (mtl_set_depth_zero, defined with the replay state)
static const char *g_vkdir;
static uint32_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *p = malloc((size_t)sz + 1); if (fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); fclose(f); return NULL; }
    fclose(f); ((char *)p)[sz] = 0; *n = (size_t)sz; return p;
}
// NVN's GL depth convention (nvnDeviceSetDepthMode near = -w: Metroid Dread, Super Mario Odyssey...): clip-space z runs -w..w, Vulkan's 0..w. A vertex shader then ends with
// z' = (z + w) / 2 -- the GL entry point dmsh_build.py adds to the Metal libraries (vs_depth), done here on the SPIR-V: before every return of the entry point the position
// (a BuiltIn Position variable, or member of an output block) is read, rewritten and stored back. Returns a new word array (the caller frees it), NULL when no position is written
// Position is decorated Invariant in every vertex shader: games draw a depth prepass with one vertex shader and the scene with another under the depth test EQUAL (Metroid Dread's
// G-buffer pass), which on the Switch matches bit for bit; without the decoration the Mali compiler evaluated the two positions differently and the whole level failed the test
static uint32_t *spv_invariant(const uint32_t *w, size_t nw, size_t *out_n) {
    enum { OpDecorate = 71, OpMemberDecorate = 72, BuiltIn = 11, Position = 0, Invariant = 18 };
    for (size_t i = 5; i < nw;) {   // already invariant: nothing to add
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len) return NULL;
        if ((op == OpDecorate && len >= 3 && w[i + 2] == Invariant) || (op == OpMemberDecorate && len >= 4 && w[i + 3] == Invariant)) return NULL;
        i += len;
    }
    uint32_t *o = malloc((nw + 8) * 4); size_t n = 0; int added = 0;
    memcpy(o, w, 20); n = 5;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        memcpy(o + n, w + i, len * 4); n += len;
        if (added < 2 && op == OpMemberDecorate && len >= 5 && w[i + 3] == BuiltIn && w[i + 4] == Position) { o[n++] = 4u << 16 | OpMemberDecorate; o[n++] = w[i + 1]; o[n++] = w[i + 2]; o[n++] = Invariant; added++; }
        else if (added < 2 && op == OpDecorate && len >= 4 && w[i + 2] == BuiltIn && w[i + 3] == Position) { o[n++] = 3u << 16 | OpDecorate; o[n++] = w[i + 1]; o[n++] = Invariant; added++; }
        i += len;
    }
    if (!added) { free(o); return NULL; }
    *out_n = n; return o;
}
// PointSize = 1.0 stored first thing in every vertex shader: Vulkan leaves a point's size undefined unless the vertex shader writes it, and the translated shaders write it only when
// the Switch shader did (NVN then takes SetPointSize). Super Mario Odyssey draws points with shaders that do not, and the Mali-G57 lost the device at its first such draw. A shader that
// writes its own size later overrides the 1.0; topologies other than points ignore it
static uint32_t *spv_point_size(const uint32_t *w, size_t nw, size_t *out_n) {
    enum { OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpLabel = 248, OpVariable = 59, OpStore = 62, OpAccessChain = 65,
           OpDecorate = 71, OpMemberDecorate = 72, Output = 3, BuiltIn = 11, PointSize = 1 };
    uint32_t bound = w[3], entry = 0, f32 = 0, u32 = 0, ps_struct = 0, ps_member = 0, ps_var = 0, var = 0; int member_path = 0; size_t first_fn = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len) return NULL;
        if (op == OpEntryPoint && !entry) entry = w[i + 2];
        else if (op == OpMemberDecorate && len >= 5 && w[i + 3] == BuiltIn && w[i + 4] == PointSize) { ps_struct = w[i + 1]; ps_member = w[i + 2]; member_path = 1; }
        else if (op == OpDecorate && len >= 4 && w[i + 2] == BuiltIn && w[i + 3] == PointSize) ps_var = w[i + 1];
        else if (op == OpTypeFloat && w[i + 2] == 32) f32 = w[i + 1];
        else if (op == OpTypeInt && w[i + 2] == 32 && !w[i + 3] && !u32) u32 = w[i + 1];
        else if (op == OpFunction && !first_fn) first_fn = i;
        i += len;
    }
    if (!entry || !f32 || !first_fn || (!member_path && !ps_var) || (member_path && !u32)) return NULL;
    for (size_t i = 5; i < first_fn;) {   // the Output variable holding PointSize (the per-vertex block, or the variable itself)
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpVariable && w[i + 3] == Output) {
            if (!member_path && w[i + 2] == ps_var) var = ps_var;
            if (member_path) for (size_t j = 5; j < first_fn;) { uint32_t o2 = w[j] & 0xffff, l2 = w[j] >> 16; if (o2 == OpTypePointer && w[j + 1] == w[i + 1] && w[j + 3] == ps_struct) var = w[i + 2]; j += l2; }
        }
        i += len;
    }
    if (!var) return NULL;
    // the entry function's first block: after its OpLabel and OpVariables
    size_t at = 0;
    for (size_t i = first_fn; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpFunction && w[i + 2] != entry) { i += len; continue; }
        if (op == OpFunction) { size_t j = i + len; while (j < nw && (w[j] & 0xffff) != OpLabel) j += w[j] >> 16; j += w[j] >> 16; while (j < nw && (w[j] & 0xffff) == OpVariable) j += w[j] >> 16; at = j; break; }
        i += len;
    }
    if (!at) return NULL;
    uint32_t ptr = 0, one = bound++, cidx = bound++, ac = bound++;   // the Output float pointer type is reused when declared (a second declaration of it is invalid)
    for (size_t i = 5; i < first_fn;) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (op == OpTypePointer && w[i + 2] == Output && w[i + 3] == f32) ptr = w[i + 1]; i += len; }
    int new_ptr = !ptr; if (new_ptr) ptr = bound++;
    uint32_t *o = malloc((nw + 32) * 4); size_t n = 0;
    memcpy(o, w, first_fn * 4); n = first_fn;
    if (new_ptr) { o[n++] = 4u << 16 | OpTypePointer; o[n++] = ptr; o[n++] = Output; o[n++] = f32; }
    o[n++] = 4u << 16 | OpConstant; o[n++] = f32; o[n++] = one; o[n++] = 0x3f800000;
    if (member_path) { o[n++] = 4u << 16 | OpConstant; o[n++] = u32; o[n++] = cidx; o[n++] = ps_member; }
    memcpy(o + n, w + first_fn, (at - first_fn) * 4); n += at - first_fn;
    if (member_path) { o[n++] = 5u << 16 | OpAccessChain; o[n++] = ptr; o[n++] = ac; o[n++] = var; o[n++] = cidx; o[n++] = 3u << 16 | OpStore; o[n++] = ac; o[n++] = one; }
    else { o[n++] = 3u << 16 | OpStore; o[n++] = var; o[n++] = one; }
    memcpy(o + n, w + at, (nw - at) * 4); n += nw - at;
    o[3] = bound; *out_n = n; return o;
}
static uint32_t *spv_gl_depth(const uint32_t *w, size_t nw, size_t *out_n) {
    enum { OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpStore = 62,
           OpAccessChain = 65, OpDecorate = 71, OpMemberDecorate = 72, OpCompositeExtract = 81, OpCompositeInsert = 82, OpFAdd = 129, OpFMul = 133, OpReturn = 253, Output = 3 };
    uint32_t bound = w[3], entry = 0, f32 = 0, v4 = 0, u32 = 0, pos_struct = 0, pos_member = 0, pos_var = 0, ptr_v4 = 0, cidx = 0, var = 0; int member_path = 0;
    size_t first_fn = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len) return NULL;
        if (op == OpEntryPoint && !entry) entry = w[i + 2];
        else if (op == OpMemberDecorate && len >= 5 && w[i + 3] == 11 && w[i + 4] == 0) { pos_struct = w[i + 1]; pos_member = w[i + 2]; member_path = 1; }
        else if (op == OpDecorate && len >= 4 && w[i + 2] == 11 && w[i + 3] == 0) pos_var = w[i + 1];
        else if (op == OpTypeFloat && w[i + 2] == 32) f32 = w[i + 1];
        else if (op == OpTypeInt && w[i + 2] == 32 && !u32) u32 = w[i + 1];
        else if (op == OpFunction && !first_fn) first_fn = i;
        i += len;
    }
    for (size_t i = 5; i < first_fn;) {   // the vector, pointer, index constant and variable, now the type IDs are known
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpTypeVector && w[i + 2] == f32 && w[i + 3] == 4) v4 = w[i + 1];
        i += len;
    }
    for (size_t i = 5; i < first_fn;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpTypePointer && w[i + 2] == Output && w[i + 3] == v4) ptr_v4 = w[i + 1];
        if (op == OpConstant && w[i + 1] == u32 && len == 4 && w[i + 3] == pos_member) cidx = w[i + 2];
        if (op == OpVariable && w[i + 3] == Output) {
            if (!member_path && w[i + 2] == pos_var) var = w[i + 2];
            if (member_path) for (size_t j = 5; j < first_fn;) { uint32_t o2 = w[j] & 0xffff, l2 = w[j] >> 16; if (o2 == OpTypePointer && w[j + 1] == w[i + 1] && w[j + 3] == pos_struct) var = w[i + 2]; j += l2; }
        }
        i += len;
    }
    if (!entry || !f32 || !v4 || !var || (member_path && !u32)) return NULL;
    uint32_t decl[16]; size_t nd = 0;
    if (!ptr_v4) { ptr_v4 = bound++; decl[nd++] = 4u << 16 | OpTypePointer; decl[nd++] = ptr_v4; decl[nd++] = Output; decl[nd++] = v4; }
    if (member_path && !cidx) { cidx = bound++; decl[nd++] = 4u << 16 | OpConstant; decl[nd++] = u32; decl[nd++] = cidx; decl[nd++] = pos_member; }
    uint32_t half = bound++; float hf = 0.5f; uint32_t hb; memcpy(&hb, &hf, 4);
    decl[nd++] = 4u << 16 | OpConstant; decl[nd++] = f32; decl[nd++] = half; decl[nd++] = hb;
    size_t nret = 0; for (size_t i = first_fn; i < nw; i += w[i] >> 16) if ((w[i] & 0xffff) == OpReturn) nret++;
    uint32_t *o = malloc((nw + nd + nret * 40) * 4); size_t n = 0; int in_entry = 0;
    for (size_t i = 0; i < 5; i++) o[n++] = w[i];
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == first_fn) { memcpy(o + n, decl, nd * 4); n += nd; }
        if (op == OpFunction) in_entry = w[i + 2] == entry;
        if (op == OpReturn && in_entry) {
            uint32_t p = var, ld = bound++, z = bound++, ww = bound++, sum = bound++, hz = bound++, nv = bound++;
            if (member_path) { p = bound++; o[n++] = 5u << 16 | OpAccessChain; o[n++] = ptr_v4; o[n++] = p; o[n++] = var; o[n++] = cidx; }
            o[n++] = 4u << 16 | OpLoad; o[n++] = v4; o[n++] = ld; o[n++] = p;
            o[n++] = 5u << 16 | OpCompositeExtract; o[n++] = f32; o[n++] = z; o[n++] = ld; o[n++] = 2;
            o[n++] = 5u << 16 | OpCompositeExtract; o[n++] = f32; o[n++] = ww; o[n++] = ld; o[n++] = 3;
            o[n++] = 5u << 16 | OpFAdd; o[n++] = f32; o[n++] = sum; o[n++] = z; o[n++] = ww;
            o[n++] = 5u << 16 | OpFMul; o[n++] = f32; o[n++] = hz; o[n++] = sum; o[n++] = half;
            o[n++] = 6u << 16 | OpCompositeInsert; o[n++] = v4; o[n++] = nv; o[n++] = hz; o[n++] = ld; o[n++] = 2;
            o[n++] = 3u << 16 | OpStore; o[n++] = p; o[n++] = nv;
        }
        memcpy(o + n, w + i, len * 4); n += len; i += len;
    }
    o[3] = bound; *out_n = n; return o;
}
static uint32_t *vertex_spv(uint32_t *spv, size_t *bytes, shader_t *s) {
    size_t ns = *bytes;
    {
        size_t nn; int ok; uint32_t *w2=spv_draw_parameters(spv,ns/4,g_draw_parameters,&nn,&ok);
        if (!ok) { LOG("[RED] vk: shader %s draw-parameter lowering failed",s->key);free(spv);return NULL; }
        if (w2) { free(spv);spv=w2;ns=nn*4;s->draw_push=1; }
    }
    { size_t nn; uint32_t *w2 = spv_output_defaults(spv, ns / 4, &nn); if (w2) { free(spv); spv = w2; ns = nn * 4; } }
    { size_t nn; uint32_t *w2 = spv_invariant(spv, ns / 4, &nn); if (w2) { free(spv); spv = w2; ns = nn * 4; } }
    { size_t nn; uint32_t *w2 = spv_point_size(spv, ns / 4, &nn); if (w2) { free(spv); spv = w2; ns = nn * 4; } }
    if (!s->depth_zero) {   // NVN's GL depth convention (see spv_gl_depth)
        size_t nn; uint32_t *w2 = spv_gl_depth(spv, ns / 4, &nn);
        if (w2) { free(spv); spv = w2; ns = nn * 4; } else LOG("[RED] vk: vertex shader %s: no position written, left as it is for the GL depth convention", s->key);
    }
    *bytes = ns;
    return spv;
}

static uint32_t *image_shape_spv(uint32_t *spv, size_t *bytes, const shader_t *s) {
    size_t words; const char *reason;
    uint32_t *adjusted = spv_one_row(spv, *bytes / 4, &words, &reason);
    if (reason) {
        LOG("[RED] vk: shader %s one-row image conversion failed: %s", s->key, reason);
        free(spv); return NULL;
    }
    if (adjusted) { free(spv); spv = adjusted; *bytes = words * 4; }
    return spv;
}
static uint32_t *shader_bias_spv(uint32_t *spv, size_t *bytes, const shader_t *s) {
    uint32_t wrapped[DM_VK_MAX_BINDINGS];size_t wrapped_count=0;
    for (int i=0;i<s->nb;i++) if (s->wrapped[i]) wrapped[wrapped_count++]=s->b[i].binding;
    if (spv && wrapped_count) {
        size_t count;const char *reason;
        uint32_t *adjusted=spv_wrapped_buffer(spv,*bytes/4,wrapped,wrapped_count,&count,&reason);
        free(spv);spv=adjusted;
        if (!spv) LOG("[RED] vk: shader %s wrapped buffer lowering failed: %s",s->key,reason);
        else *bytes=count*4;
    }
    if (spv) spv = image_shape_spv(spv, bytes, s);
    if (!spv) return NULL;
    uint32_t bindings[DM_VK_MAX_BINDINGS]; int any=0;
    for (int i=0;i<s->nb;i++) { bindings[i]=s->b[i].binding;any|=s->buffer_bias[i]!=0; }
    size_t words; const char *reason;
    if (s->nreductions) {
        uint32_t *adjusted = spv_sample_reduction(spv, *bytes / 4, s->reductions, s->nreductions, &words, &reason);
        free(spv); spv = adjusted;
        if (!spv) { LOG("[RED] vk: shader %s sampler reduction failed: %s", s->key, reason); return NULL; }
        *bytes = words * 4;
    }
    if (spv && s->ntexel_biases) {
        uint32_t *adjusted=spv_texel_bias(spv,*bytes/4,s->texel_biases,s->ntexel_biases,&words,&reason);
        free(spv); spv=adjusted;
        if (!spv) LOG("[RED] vk: shader %s typed view rebasing failed: %s",s->key,reason);
        else *bytes=words*4;
    }
    if (!spv || !any) return spv;
    uint32_t *biased=spv_buffer_bias(spv,*bytes/4,bindings,s->buffer_bias,s->nb,&words,&reason);
    free(spv);
    if (!biased) LOG("[RED] vk: shader %s buffer rebasing failed: %s",s->key,reason);
    else *bytes=words*4;
    return biased;
}
static unsigned shader_location(const shader_t *s, unsigned location) {
    return s->pack_locations ? (unsigned)__builtin_popcount(s->vin_mask & ((1u<<location)-1)) : location;
}
// NVN has 32 attribute locations. On a device with fewer slots a shader may
// still fit: compact its sparse vertex inputs, leaving interpolator outputs alone.
static int pack_vertex_locations(uint32_t *words, size_t count, const shader_t *s) {
    if (!s->pack_locations) return 1;
    uint8_t *input=calloc(words[3],1);
    if (!input) return 0;
    for (size_t i=5;i<count;i+=words[i]>>16) {
        unsigned n=words[i]>>16, op=words[i]&0xffff;
        if (!n || n>count-i) { free(input);return 0; }
        if (op==59 && n>=4 && words[i+2]<words[3] && words[i+3]==1) input[words[i+2]]=1;
    }
    for (size_t i=5;i<count;i+=words[i]>>16) if ((words[i]&0xffff)==71 && (words[i]>>16)==4 &&
        words[i+1]<words[3] && input[words[i+1]] && words[i+2]==30) {
        unsigned loc=words[i+3];
        if (loc>=32 || !(s->vin_mask & (1u<<loc))) { free(input);return 0; }
        words[i+3]=shader_location(s,loc);
    }
    free(input);return 1;
}
#include "descriptor_tables.inc"
static uint32_t *stage_spv(uint32_t *spv, size_t *bytes, shader_t *s) {
    spv=driver_table_spv(spv,bytes,s);
    if (!spv) return NULL;
    if (s->stage==0) spv=vertex_spv(spv,bytes,s);
    else if (!g_draw_parameters) {
        size_t nn;int ok;uint32_t *w2=spv_draw_parameters(spv,*bytes/4,0,&nn,&ok);
        if (!ok) { LOG("[RED] vk: shader %s draw-parameter lowering failed",s->key);free(spv);return NULL; }
        if (w2) { free(spv);spv=w2;*bytes=nn*4; }
    }
    if (!spv) return NULL;
    spv=shader_bias_spv(spv,bytes,s);
    if (!spv) return NULL;
    if (!pack_vertex_locations(spv,*bytes/4,s)) { free(spv);return NULL; }
    uint32_t set=s->stage==1?1:0;
    for (size_t i=5;i<*bytes/4;) {
        uint32_t op=spv[i]&0xffff,len=spv[i]>>16;
        if (!len) break;
        if (op==71 && len>=4 && spv[i+2]==34) spv[i+3]=set;
        i+=len;
    }
    return spv;
}

// Reuse one module across PSOs that fetch the same locations in the same classes.
// Only a new class combination reopens the vertex artifact; its bytes live for this call.
static VkShaderModule vertex_variant(shader_t *s, vertex_classes classes) {
    for (vin_module *v = s->vertex_variants; v; v = v->next) if (!memcmp(&v->classes,&classes,sizeof classes)) return v->module;
    char path[1024]; size_t ns = 0, words = 0;
    snprintf(path, sizeof path, "%s/%s.spv", g_vkdir, s->key);
    uint32_t *spv = read_file(path, &ns);
    if (!spv) { LOG("[RED] vk: cannot reopen vertex shader %s", s->key); return VK_NULL_HANDLE; }
    spv = vertex_spv(spv, &ns, s);
    if (!spv) return VK_NULL_HANDLE;
    spv = shader_bias_spv(spv, &ns, s);
    if (!spv) return VK_NULL_HANDLE;
    const char *reason;
    uint32_t *variant = spv_vertex_fetch(spv, ns / 4, classes, s->vin, &words, &reason);
    free(spv);
    VkShaderModule module = VK_NULL_HANDLE;
    if (variant && !pack_vertex_locations(variant,words,s)) { free(variant); variant=NULL; reason="vertex input packing failed"; }
    if (variant) {
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, words * 4, variant};
        VkResult r = vkCreateShaderModule(g_dev, &mi, NULL, &module);
        free(variant);
        if (r != VK_SUCCESS) {
            module = VK_NULL_HANDLE;
            LOG("[RED] vk: vertex shader %s fetch variant rejected (%d)", s->key, (int)r);
        }
    } else {
        LOG("[RED] vk: vertex shader %s cannot convert input classes: %s", s->key, reason);
    }
    vin_module *v = malloc(sizeof *v);
    if (!v) { if (module) vkDestroyShaderModule(g_dev, module, NULL); return VK_NULL_HANDLE; }
    *v = (vin_module){classes, module, s->vertex_variants}; s->vertex_variants = v;
    return module;
}
// Core descriptor limits apply across the layouts in a pipeline as well as per
// shader stage. A large handle table can legitimately reach either limit.
static int table_descriptor_limits(const shader_t *a, const shader_t *b) {
    const VkPhysicalDeviceLimits *limit = &g_props.limits;
    uint64_t all_images=0, all_samplers=0;
    const shader_t *stages[]={a,b};
    for (int st=0; st<2; st++) {
        const shader_t *s=stages[st];
        if (!s) continue;
        uint64_t images=0, samplers=0, resources=0;
        for (int i=0; i<s->nb; i++) {
            const sbind *binding=&s->b[i];
            unsigned type=binding->type;
            if (type==BT_TEX || type==BT_CIS || type==BT_TBUF || type==BT_UTB) images+=binding->count;
            if (type==BT_SAMP || type==BT_CIS) samplers+=binding->count;
            if (type!=BT_SAMP) resources+=binding->count;
        }
        if (images>limit->maxPerStageDescriptorSampledImages || samplers>limit->maxPerStageDescriptorSamplers ||
            resources>limit->maxPerStageResources) return 0;
        all_images+=images; all_samplers+=samplers;
    }
    return all_images<=limit->maxDescriptorSetSampledImages && all_samplers<=limit->maxDescriptorSetSamplers;
}
static VkDescriptorSetLayout shader_layout(const shader_t *s) {
    VkShaderStageFlags stage = s->stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT :
        s->stage == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding bindings[DM_VK_MAX_BINDINGS];
    for (int i = 0; i < s->nb; i++) {
        const sbind *b = &s->b[i];
        bindings[i] = (VkDescriptorSetLayoutBinding){b->binding, descriptor_type[b->type], b->count, stage, NULL};
    }
    VkDescriptorSetLayoutCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)s->nb, bindings};
    VkDescriptorSetLayout layout;
    if (s->has_tables || s->logical_slots) {
        if (!table_descriptor_limits(s,NULL)) return VK_NULL_HANDLE;
        VkDescriptorSetLayoutSupport support = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
        vkGetDescriptorSetLayoutSupport(g_dev, &info, &support);
        if (!support.supported) return VK_NULL_HANDLE;
    }
    return vkCreateDescriptorSetLayout(g_dev, &info, NULL, &layout) == VK_SUCCESS ? layout : VK_NULL_HANDLE;
}
static shader_t *shader_by_key(const char *key) {
    for (int i = 0; i < g_nsh; i++) if (!strcmp(g_sh[i]->key, key)) return g_sh[i];
    if (!g_vkdir && !(g_vkdir = getenv("DM_VK_DIR"))) die("vk: DM_VK_DIR is not set (the folder of <key>.spv / <key>.vk.txt from dm/android/tools/spv_android.py)");
    char path[1024]; size_t ns, nt;
    snprintf(path, sizeof path, "%s/%s.spv", g_vkdir, key); uint32_t *spv = read_file(path, &ns);
    snprintf(path, sizeof path, "%s/%s.vk.txt", g_vkdir, key); char *txt = (char *)read_file(path, &nt);
    if (strlen(key)>=sizeof(((shader_t *)0)->key)) return NULL;
    shader_t *s = calloc(1, sizeof *s); strcpy(s->key, key); s->stage = -1;
    g_sh = realloc(g_sh, (g_nsh + 1) * sizeof *g_sh); g_sh[g_nsh++] = s;
    if (!spv || !txt) { LOG("[RED] vk: shader %s not in %s", key, g_vkdir); free(spv); free(txt); return s; }
    dm_vk_warp_width warp = {0};
    uint32_t local = 0;
    const char *contract_error = ns % 4 ? "unaligned SPIR-V length" : dm_vk_shader_contract_width(txt, nt, spv, ns / 4, &warp, &local);
    if (contract_error) {
        LOG("[RED] vk: shader %s rejected: %s; export shaders again", key, contract_error);
        free(spv); free(txt); return s;
    }
    dm_vk_capture capture = {0};
    for (char *ln = strtok(txt, "\n"); ln; ln = strtok(NULL, "\n")) {
        char a[16], ty[8], kind[16], sk[16], access[4] = ""; int b, cnt, reg, off, sreg, l, t;
        if (sscanf(ln, "stage %15s", a) == 1) s->stage = !strcmp(a, "vs") ? 0 : !strcmp(a, "ps") ? 1 : !strcmp(a, "cs") ? 5 : !strcmp(a, "gs") ? 2 : !strcmp(a, "ts") ? 3 : !strcmp(a, "ms") ? 4 : -1;
        else if (!strncmp(ln,"tf ",3)) {
            if (!s->feedback_layout) s->feedback_layout = calloc(1, sizeof *s->feedback_layout);
            if (!s->feedback_layout) die("Vulkan: cannot allocate feedback layout");
            if (dm_vk_feedback_line(ln,strlen(ln),s->feedback_layout)) goto bad_capture;
        } else if (!strncmp(ln,"capture ",8)) {
            if (dm_vk_capture_line(ln,strlen(ln),&capture)) goto bad_capture;
            s->capture_entry=1;
        }
        else if (!strncmp(ln,"geometry ",9)) {
            if (dm_vk_geometry_line(ln,strlen(ln),s->stage,&s->geometry)) goto bad_capture;
        }
        else if (!strncmp(ln,"mesh ",5)) {
            if (dm_vk_meshstage_line(ln,strlen(ln),&s->mesh)) goto bad_capture;
        }
        else if (!strncmp(ln,"meshv ",6) || !strncmp(ln,"meshp ",6)) {
            if (dm_vk_meshstage_map(ln,strlen(ln),ln[4]=='v',&s->mesh)) goto bad_capture;
        }
        else if (sscanf(ln, "tg %u %u %u", &s->tg[0], &s->tg[1], &s->tg[2]) == 3) {}
        else if (!strncmp(ln, "c1 ", 3)) { size_t n = strlen(ln + 3) / 2; s->c1 = malloc(n + 16); s->c1_len = (uint32_t)n; for (size_t i = 0; i < n; i++) sscanf(ln + 3 + 2 * i, "%2hhx", &s->c1[i]); }
        else if (sscanf(ln, "vin %d %d", &l, &t) == 2 && l >= 0 && l < 32) { s->vin[l] = (uint8_t)t; s->vin_mask |= 1u << l; }
        else if (sscanf(ln, "b %d %7s %d %15s %d %d %15s %d %3s", &b, ty, &cnt, kind, &reg, &off, sk, &sreg, access) >= 8 && s->nb < DM_VK_MAX_BINDINGS) {
            if (!strcmp(kind,"gmem_map") && !g_buffer_address) {
                LOG("[RED] vk: shader %s requires buffer device addresses",key);free(txt);free(spv);return s;
            }
            sbind *e = &s->b[s->nb++]; e->binding = (uint32_t)b; e->count = cnt > 0 ? (uint32_t)cnt : 1; e->reg = (int16_t)reg; e->off = off; e->sreg = (int16_t)sreg;
            if (dm_vk_resource_line(ln, strlen(ln), &e->shape)) { free(txt); free(spv); return s; }
            e->soff = e->view = -1;
            const char *field;
            if ((field = strstr(ln, " soff="))) e->soff = atoi(field + 6);
            if ((field = strstr(ln, " view="))) e->view = atoi(field + 6);
            if ((field = strstr(ln, " num="))) e->num = atoi(field + 5);
            if (!dm_vk_split_handle_parse(ln, &e->twin)) {
                LOG("[RED] vk: shader %s has invalid split handle metadata", key); free(txt); free(spv); return s;
            }
            e->read_only = !strcmp(access, "ro");
            e->type = !strcmp(ty, "ubo") ? BT_UBO : !strcmp(ty, "ssbo") ? BT_SSBO : !strcmp(ty, "cis") ? BT_CIS : !strcmp(ty, "tbuf") ? BT_TBUF : !strcmp(ty, "utb") ? BT_UTB :
                      !strcmp(ty, "stb") ? BT_STB : !strcmp(ty, "simg") ? BT_SIMG : !strcmp(ty, "tex") ? BT_TEX : BT_SAMP;
            e->kind = !strcmp(kind, "cbuf") ? K_CBUF : !strcmp(kind, "c1") ? K_C1 : !strcmp(kind, "support") ? K_SUPPORT : !strcmp(kind, "drvcb") ? K_DRVCB : !strcmp(kind, "tex") ? K_TEX :
                      !strcmp(kind, "img") ? K_IMG : !strcmp(kind, "sbuf") ? K_SBUF : !strcmp(kind, "sbuf_ubo") ? K_SBUFU : !strcmp(kind, "tbuf") ? K_TBUF : !strcmp(kind, "ubo_ptr") ? K_UBOP : !strcmp(kind, "tex_ubo") ? K_TEXU : K_UNK;
            if (!strcmp(kind,"mesh")) {
                if (reg<0 || reg>2 || cnt!=1 || e->type!=(reg==2 ? BT_UBO : BT_SSBO)) goto bad_capture;
                e->kind=K_MESH;
            }
            if (!strcmp(kind,"geometry") && reg>=0 && reg<5) e->kind=K_GEOMETRY;
            if (!strcmp(kind,"geometry") && reg == 5 && e->type == BT_UBO) e->kind=K_CAPTURE_CONTROL;
            if (!strcmp(kind,"tex_slots")) e->kind=K_TEX_SLOTS;
            if (!strcmp(kind,"tex_c2a")) e->kind=K_TEX_C2;
            if (!strcmp(kind,"tex_pool")) e->kind=K_TEX_POOL;
            if (!strcmp(kind,"pool_bounds")) e->kind=K_POOL_BOUNDS;
            if (!strcmp(kind,"gmem_map")) {
                if (e->type!=BT_SSBO || cnt!=1) goto bad_table;
                e->kind=K_GMEM_MAP;
                field=strstr(ln," gmem_write=");
                if (!field || (field[12]!='0' && field[12]!='1') || (field[13] && field[13]!=' ')) goto bad_table;
                s->raw_writes=field[12]=='1';
            }
            if (!strcmp(kind,"gmem_log")) {
                if (e->type!=BT_SSBO || cnt!=1 || e->read_only) goto bad_table;
                e->kind=K_GMEM_LOG;
            }
            e->skind = sampler_source(sk);
            if (e->type == BT_SAMP) { e->kind = K_SAMPLER; e->skind = sampler_source(kind); e->sreg = (int16_t)reg; e->soff = off; }
            if (e->kind==K_TEX_SLOTS || e->skind==S_SAMP_SLOTS) {
                s->logical_slots=1;
                int texel=e->type==BT_TBUF || e->type==BT_UTB;
                if (!DM_GAME_TEX_DYN_SLOTS || !(texel ? g_texel_indexing : g_sampled_indexing)) {
                    LOG("[RED] vk: shader %s logical slot arrays need tex_dyn_slots and nonuniform descriptor support",key);
                    free(txt);free(spv);return s;
                }
            }
            if (e->kind==K_TEX_C2 || e->skind==S_SAMP_C2) {
                if (cnt<1 || cnt>8192 || (e->type!=BT_SAMP && e->off<0) ||
                    (e->type!=BT_TEX && e->type!=BT_UTB && e->type!=BT_TBUF && (e->skind!=S_SAMP_C2 || e->soff<0))) goto bad_table;
                e->logical_count=(uint32_t)cnt;
                e->driver_sampler=-1;
                if ((field=strstr(ln," c2_or="))) {
                    unsigned offset;
                    if (sscanf(field+7,"%u",&offset)!=1 || offset>INT32_MAX) goto bad_table;
                    e->driver_sampler=(int32_t)offset;
                }
            }
            if (e->kind==K_TEX_POOL || e->skind==S_SAMP_POOL) {
                int slot;
                field=strstr(ln," runtime=1 pool_slot=");
                int sampler=e->type==BT_SAMP;
                if (!field || sscanf(field," runtime=1 pool_slot=%d",&slot)!=1 || slot<0 || slot>=DM_VK_MAX_BINDINGS ||
                    cnt!=(sampler ? 4096 : 1048576) || (!sampler && e->type!=BT_TEX)) goto bad_table;
                e->slot=(int16_t)slot;e->table=1;e->pool=1;e->logical_count=cnt;e->count=1;
                s->has_tables=1;
            }
            if (e->kind==K_POOL_BOUNDS && (e->type!=BT_UBO || cnt!=1 || reg<1 || reg>=DM_VK_MAX_BINDINGS)) goto bad_table;
            if (!strcmp(kind, "htab_map")) {
                if (e->type != BT_SSBO || cnt != 1 || reg < 0 || reg >= DM_VK_MAX_BINDINGS) goto bad_table;
                e->kind = K_TABLE_MAP; e->slot = reg;
            } else if (!strcmp(kind, "tex_htab") || !strcmp(kind, "samp_htab")) {
                int slot;
                field = strstr(ln, " runtime=1 pool_slot=");
                if (!field || sscanf(field, " runtime=1 pool_slot=%d", &slot) != 1 ||
                    slot < 0 || slot >= DM_VK_MAX_BINDINGS || cnt != 8192 || reg < 0 || reg >= 16 || off < 0 ||
                    (!strcmp(kind,"samp_htab") ? e->type != BT_SAMP : e->type != BT_CIS && e->type != BT_TEX)) goto bad_table;
                if (e->type == BT_CIS && (strcmp(sk, "samp_htab") || sreg < 0 || sreg >= 16 || e->soff < 0)) goto bad_table;
                e->kind = e->type == BT_SAMP ? K_SAMPLER : K_TEXU;
                if (e->type != BT_TEX) e->skind = S_SAMPU;
                e->driver_sampler = -1;
                if ((field = strstr(ln," c2_or="))) {
                    int offset = atoi(field+7);
                    if (offset < 0 || offset > 4092 || offset % 4) goto bad_table;
                    e->driver_sampler = offset;
                }
                e->slot = slot; e->table = 1; e->count = 1;
                s->has_tables = 1;
            }
            if (e->kind == K_UNK) LOG("[RED] vk: shader %s binding %d has kind %s, not implemented (reads zero)", key, b, kind);
        }
    }
    {
        unsigned maps=0,logs=0;
        for (int i=0;i<s->nb;i++) { maps+=s->b[i].kind==K_GMEM_MAP;logs+=s->b[i].kind==K_GMEM_LOG; }
        if (maps>1 || logs!=(unsigned)s->raw_writes || (logs && maps!=1)) goto bad_table;
    }
    if (!prepare_driver_tables(s)) goto bad_table;
    if (s->capture_entry) {
        unsigned seen = 0;
        for (int i = 0; i < s->nb; ++i) {
            if (s->b[i].kind==K_GEOMETRY && s->b[i].reg==0 && s->b[i].type==BT_SSBO) s->b[i].kind=K_CAPTURE_RECORDS;
            const sbind *b = &s->b[i];
            if (b->kind == K_CAPTURE_RECORDS && b->binding == capture.output_binding && b->count == 1) seen |= 1;
            if (b->kind == K_CAPTURE_CONTROL && b->binding == capture.control_binding && b->count == 1) seen |= 2;
        }
        if (seen != 3) goto bad_capture;
    }
    for (int i = 0; i < s->nb; ++i) {
        const sbind *b = &s->b[i];
        if (!b->table && b->kind != K_TABLE_MAP) continue;
        int arrays = 0, maps = 0;
        for (int j = 0; j < s->nb; ++j) if (s->b[j].slot == b->slot) {
            arrays += s->b[j].table; maps += s->b[j].kind == K_TABLE_MAP;
        }
        if (arrays != 1 || maps != 1) goto bad_table;
    }
    if (s->has_tables && !g_descriptor_tables) {
        LOG("[RED] vk: shader %s requires runtime sampled-image descriptor indexing",key);
        free(txt); free(spv); return s;
    }
    free(txt);
    s->guest_stage=s->geometry.stride && !s->geometry.reader ? 2 : s->stage;
    if (s->mesh.words) {
        unsigned seen=0;
        uint32_t bindings[]={s->mesh.input_binding,s->mesh.output_binding,s->mesh.control_binding};
        for (int i=0;i<s->nb;i++) if (s->b[i].kind==K_MESH) {
            if (s->b[i].binding!=bindings[s->b[i].reg]) goto bad_mesh;
            seen|=1u<<s->b[i].reg;
        }
        if ((seen&6)!=6 || (s->mesh.task && !(seen&1))) goto bad_mesh;
        s->stage=5;
    }
    if (s->stage==2) { free(spv); return s; } // Its exported compute/reader family executes the draw.
    if (s->stage < 0) { LOG("[RED] vk: shader %s needs a stage the renderer does not execute", key); free(spv); return s; }
    VkShaderStageFlags sf = s->stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : s->stage == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    if (warp.min) {
        int selectable = !!(g_selectable_subgroup_stages & sf);
        int size = dm_vk_warp_select(warp, g_subgroup.subgroupSize, selectable ? g_subgroup_sizes.minSubgroupSize : 0,
                                     selectable ? g_subgroup_sizes.maxSubgroupSize : 0, local, g_subgroup_sizes.maxComputeWorkgroupSubgroups);
        if (!(g_subgroup.supportedStages & sf) || size < 0) {
            LOG("[RED] vk: shader %s requires subgroup width %u..%u, stage %#x, device default %u, supported stages %#x",
                key, warp.min, warp.max, sf, g_subgroup.subgroupSize, g_subgroup.supportedStages);
            free(spv); return s;
        }
        if (size) s->subgroup = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, NULL, (uint32_t)size};
    }
    uint32_t location_limit=g_props.limits.maxVertexInputAttributes;
    if (getenv("DM_VK_TEST_16_ATTRIBUTES") && location_limit>16) location_limit=16;
    if (s->stage==0 && location_limit<32 && (s->vin_mask>>location_limit)) {
        if ((uint32_t)__builtin_popcount(s->vin_mask)>location_limit) {
            LOG("[RED] vk: vertex shader %s needs more than the device's %u inputs",key,location_limit); free(spv);return s;
        }
        s->pack_locations=1;
    }
    if (s->stage==1) s->outputs=spv_fragment_outputs(spv,ns/4);
    if (s->stage==0) for (size_t i=5; i<ns/4;) {
        uint32_t op=spv[i]&0xffff, len=spv[i]>>16;
        if (!len || len>ns/4-i) break;
        // BuiltIn Layer may decorate a standalone output or a block member.
        if ((op==71 && len==4 && spv[i+2]==11 && spv[i+3]==9) ||
            (op==72 && len==5 && spv[i+3]==11 && spv[i+4]==9)) s->layer_output=1;
        i+=len;
    }
    if (s->layer_output && !g_layer_output) {
        LOG("[RED] vk: vertex shader %s needs shaderOutputLayer",key);free(spv);return s;
    }
    s->depth_zero=g_depth_zero;
    spv=stage_spv(spv,&ns,s);
    if (!spv) return s;
    reflect_shader_images(spv,ns/4,s->b,s->nb);
    s->sampler_images=reflect_sampler_pairs(spv,ns/4,s->b,s->nb);
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, ns, spv};
    VkResult r = vkCreateShaderModule(g_dev, &mi, NULL, &s->mod); free(spv);
    if (r != VK_SUCCESS) { LOG("[RED] vk: shader %s rejected (%d)", key, (int)r); s->mod = VK_NULL_HANDLE; }
    s->dsl = shader_layout(s);
    if (!s->dsl) { LOG("[RED] vk: shader %s descriptor layout exceeds device support", key); s->mod=VK_NULL_HANDLE; }
    return s;
bad_mesh:
    LOG("[RED] vk: shader %s has invalid task/mesh helper bindings",key);
    free(spv); return s;
bad_capture:
    LOG("[RED] vk: shader %s has an invalid feedback capture interface",key);
    free(txt); free(spv); return s;
bad_table:
    LOG("[RED] vk: shader %s has an invalid uniform handle table interface", key);
    free(txt); free(spv); return s;
}
static shader_t *shader_get(const uint8_t md[8]) {
    char key[17];
    for (int i=0; i<8; i++) sprintf(key+2*i,"%02x",md[i]);
    return shader_by_key(key);
}
static shader_t *buffer_variant(shader_t *base, const uint32_t bias[DM_VK_MAX_BINDINGS],
                                const texel_bias *texels, size_t nt, const sample_reduction *reductions, size_t nr) {
    int any=0;for(int i=0;i<base->nb;i++) any|=bias[i]!=0;
    if (!any && !nt && !nr) return base;
    for(shader_t *s=base->buffer_variants;s;s=s->next_buffer_variant)
        if (s->ntexel_biases==nt && s->nreductions==nr && (!nr || !memcmp(s->reductions,reductions,nr*sizeof *reductions)) &&
            !memcmp(s->buffer_bias,bias,sizeof s->buffer_bias) &&
            (!nt || !memcmp(s->texel_biases,texels,nt*sizeof *texels))) return s->mod?s:NULL;
    shader_t *s=malloc(sizeof *s);
    if(!s) return NULL;
    *s=*base;s->buffer_variants=NULL;s->vertex_variants=NULL;s->cpp=VK_NULL_HANDLE;s->cpl=VK_NULL_HANDLE;s->mod=VK_NULL_HANDLE;
    s->fragment_variants=NULL;
    s->texel_biases=NULL; s->ntexel_biases=nt;
    s->reductions=NULL; s->nreductions=nr;
    if (nt) {
        s->texel_biases=malloc(nt*sizeof *texels);
        if (!s->texel_biases) { free(s); return NULL; }
        memcpy(s->texel_biases,texels,nt*sizeof *texels);
    }
    if (nr) {
        s->reductions=malloc(nr*sizeof *reductions);
        if (!s->reductions) { free(s->texel_biases); free(s); return NULL; }
        memcpy(s->reductions,reductions,nr*sizeof *reductions);
    }
    memcpy(s->buffer_bias,bias,sizeof s->buffer_bias);
    s->next_buffer_variant=base->buffer_variants;base->buffer_variants=s;
    char path[1024];size_t bytes;
    snprintf(path,sizeof path,"%s/%s.spv",g_vkdir,s->key);
    uint32_t *spv=read_file(path,&bytes);
    if(!spv) return NULL;
    spv=stage_spv(spv,&bytes,s);
    if(!spv) return NULL;
    VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,bytes,spv};
    VkResult r=vkCreateShaderModule(g_dev,&mi,NULL,&s->mod);free(spv);
    if(r!=VK_SUCCESS) { s->mod=VK_NULL_HANDLE;LOG("[RED] vk: shader %s rebased buffer module rejected (%d)",s->key,r);return NULL; }
    return s;
}
static VkDescriptorSetLayout g_empty_dsl;
static VkShaderModule fragment_module(shader_t *fs, uint8_t uint_mask, uint8_t sint_mask, int alpha) {
    if (!uint_mask && !sint_mask && !alpha) return fs->mod;
    for (fragment_variant *v=fs->fragment_variants;v;v=v->next)
        if (v->uint_mask==uint_mask && v->sint_mask==sint_mask && v->alpha==alpha) return v->module;
    fragment_variant *v=calloc(1,sizeof *v);
    if (!v) return VK_NULL_HANDLE;
    v->uint_mask=uint_mask; v->sint_mask=sint_mask; v->alpha=alpha;
    v->next=fs->fragment_variants; fs->fragment_variants=v;
    char path[1024]; size_t bytes=0, words=0;
    snprintf(path,sizeof path,"%s/%s.spv",g_vkdir,fs->key);
    uint32_t *spv=read_file(path,&bytes);
    if (spv) spv=stage_spv(spv,&bytes,fs); // retain this variant's SSBO offsets and descriptor set
    const char *reason="cannot reload fragment shader";
    if (spv && (uint_mask || sint_mask)) {
        uint32_t *typed=spv_integer_outputs(spv,bytes/4,uint_mask,sint_mask,&words,&reason);
        free(spv); spv=typed; bytes=words*4;
    }
    if (spv && alpha) {
        uint32_t *tested=spv_alpha(spv,bytes/4,&words,&reason);
        free(spv); spv=tested; bytes=words*4;
    }
    if (spv) {
        VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,bytes,spv};
        VkResult r=vkCreateShaderModule(g_dev,&mi,NULL,&v->module); free(spv);
        if (r==VK_SUCCESS) return v->module;
        v->module=VK_NULL_HANDLE; reason="driver rejected fragment variant";
    }
    LOG("[RED] vk: fragment %s with uint mask %#x, sint mask %#x, alpha test %d unavailable (%s)",
        fs->key,uint_mask,sint_mask,alpha,reason);
    return VK_NULL_HANDLE;
}
typedef struct { shader_t *vs, *fs; VkPipelineLayout pl; } prog_vk;
static prog_vk *g_pl; static int g_npl;
static VkPipelineLayout layout_get(shader_t *vs, shader_t *fs) {
    for (int i = 0; i < g_npl; i++) if (g_pl[i].vs == vs && g_pl[i].fs == fs) return g_pl[i].pl;
    if ((vs->has_tables || vs->logical_slots || (fs && (fs->has_tables || fs->logical_slots))) && !table_descriptor_limits(vs,fs)) {
        RED_ONCE(83, "combined graphics handle tables exceed the device descriptor limits"); return VK_NULL_HANDLE;
    }
    if (!g_empty_dsl) { VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; VKCHECK(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_empty_dsl)); }
    VkDescriptorSetLayout sl[2] = {vs ? vs->dsl : g_empty_dsl, fs ? fs->dsl : g_empty_dsl};
    VkPushConstantRange ranges[2]; unsigned count=0;
    if (vs->draw_push) ranges[count++]=(VkPushConstantRange){VK_SHADER_STAGE_VERTEX_BIT,0,16};
    if (fs) ranges[count++]=(VkPushConstantRange){VK_SHADER_STAGE_FRAGMENT_BIT,DM_VK_ALPHA_PUSH_OFFSET,DM_VK_ALPHA_PUSH_BYTES};
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, fs ? 2 : 1, sl,
        count,ranges};
    VkPipelineLayout pl; VKCHECK(vkCreatePipelineLayout(g_dev, &pi, NULL, &pl));
    g_pl = realloc(g_pl, (g_npl + 1) * sizeof *g_pl); g_pl[g_npl++] = (prog_vk){vs, fs, pl};
    return pl;
}

// ---------- Render passes: dynamic rendering, or a render pass + framebuffer made for each pass ----------
typedef struct { int n; VkImageView cv[8]; VkFormat cf[8]; VkAttachmentLoadOp cl[8]; VkClearColorValue cc[8];
                 VkImageView dv; VkFormat df; int stencil; VkAttachmentLoadOp dl, sl; VkClearDepthStencilValue dc; uint32_t w, h, layers; VkSampleCountFlagBits samples; VkImageLayout layout; } rp_desc;
static struct { uint64_t key; VkRenderPass rp; } g_rpc[512]; static int g_nrpc; static pthread_mutex_t g_rpmu = PTHREAD_MUTEX_INITIALIZER;
// The render pass of these attachments (cached; also the "compatible" one pipelines are made against: load actions do not affect compatibility)
static VkRenderPass rp_get(const VkFormat *cf, const VkAttachmentLoadOp *cl, int n, VkFormat df, int stencil, VkAttachmentLoadOp dl, VkAttachmentLoadOp sl, VkImageLayout layout, VkSampleCountFlagBits samples) {
    if (!samples) samples = VK_SAMPLE_COUNT_1_BIT;
    uint64_t key = 0xcbf29ce484222325ull;
    #define MIX(v) key = (key ^ (uint64_t)(v)) * 0x100000001b3ull
    for (int i = 0; i < n; i++) { MIX(cf[i]); MIX(cl[i]); } MIX(n); MIX(df); MIX(stencil); MIX(dl); MIX(sl); MIX(layout); MIX(samples);
    #undef MIX
    pthread_mutex_lock(&g_rpmu);
    for (int i = 0; i < g_nrpc; i++) if (g_rpc[i].key == key) { VkRenderPass r = g_rpc[i].rp; pthread_mutex_unlock(&g_rpmu); return r; }
    VkAttachmentDescription at[9]; VkAttachmentReference cr[8], dr = {VK_ATTACHMENT_UNUSED, layout}; uint32_t na = 0;
    for (int i = 0; i < n; i++) {
        if (!cf[i]) { cr[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, layout}; continue; }
        at[na] = (VkAttachmentDescription){0, cf[i], samples, cl[i], VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, layout, layout};
        cr[i] = (VkAttachmentReference){na++, layout};
    }
    if (df) { at[na] = (VkAttachmentDescription){0, df, samples, dl, VK_ATTACHMENT_STORE_OP_STORE, stencil ? sl : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE, layout, layout}; dr.attachment = na++; }
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, (uint32_t)n, cr, NULL, df ? &dr : NULL, 0, NULL};
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, na, at, 1, &sp, 0, NULL};
    VkRenderPass r; VKCHECK(vkCreateRenderPass(g_dev, &ci, NULL, &r));
    if (g_nrpc < 512) g_rpc[g_nrpc++] = (typeof(g_rpc[0])){key, r};
    pthread_mutex_unlock(&g_rpmu);
    return r;
}
// Begins rendering into d's attachments; without dynamic rendering the framebuffer it makes is returned, for the caller to destroy once the GPU is done with it
static VkFramebuffer rp_begin(VkCommandBuffer cb, const rp_desc *d) {
    if (g_dynrend) {
        VkRenderingAttachmentInfo ca[8], da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        for (int i = 0; i < d->n; i++) ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, d->cv[i], d->layout, 0, NULL, 0,
                                                                          d->cv[i] ? d->cl[i] : VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, {.color = d->cc[i]}};
        if (d->dv) { da.imageView = d->dv; da.imageLayout = d->layout; da.loadOp = d->dl; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; da.clearValue.depthStencil = d->dc;
                     if (d->stencil) { sa = da; sa.loadOp = d->sl; } }
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {d->w, d->h}}, d->layers ? d->layers : 1, 0, (uint32_t)d->n, ca, d->dv ? &da : NULL, d->dv && d->stencil ? &sa : NULL};
        p_begin_rendering(cb, &ri);
        return VK_NULL_HANDLE;
    }
    VkFormat cf[8]; VkImageView views[9]; VkClearValue cv[9]; uint32_t nv = 0;
    for (int i = 0; i < d->n; i++) { cf[i] = d->cv[i] ? d->cf[i] : VK_FORMAT_UNDEFINED; if (d->cv[i]) { cv[nv].color = d->cc[i]; views[nv++] = d->cv[i]; } }
    if (d->dv) { cv[nv].depthStencil = d->dc; views[nv++] = d->dv; }
    VkRenderPass rp = rp_get(cf, d->cl, d->n, d->dv ? d->df : VK_FORMAT_UNDEFINED, d->stencil, d->dl, d->sl, d->layout, d->samples);
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, nv, views, d->w, d->h, d->layers ? d->layers : 1};
    VkFramebuffer fb; VKCHECK(vkCreateFramebuffer(g_dev, &fi, NULL, &fb));
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {d->w, d->h}}, nv, cv};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    return fb;
}
static void rp_end(VkCommandBuffer cb) { if (g_dynrend) p_end_rendering(cb); else vkCmdEndRenderPass(cb); }

// ---------- Replay state ----------
#define NTEX 256
static struct {
    const prog_t *bst[5], *cprog;   // the program bound to each graphics stage (NVN stage bits 1 vertex, 2 fragment, 4 geometry, 8 / 16 tessellation): games bind a program to some stages and NULL to others
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[32]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][NTEX], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    visibility_counter *visibility;
    feedback_control *feedback;
    int feedback_active;
    p_tf_bind feedback_streams[4];
    p_prim_restart restart; p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp, depth_clamp; p_scissor sc; int has_sc, rasterizer_discard;
    uint8_t vp_sw[16][4];   // component selectors XOR the identity, as recorded by NVN
    uint8_t push[2][DMSH_PUSH_SIZE];   // graphics stages share one NVN2 c[24], compute has its own
    float blend_color[4], alpha_ref; int64_t copy_stride, copy_img_stride;
    int tex_base, samp_base;
} S;
static int g_depth_zero = 1, g_queue_origin_ll;
static _Atomic int g_origin_ll;
static uint32_t g_dispatch_dim[3];
static uint32_t g_draw_base_instance;
static VkBuffer g_dispatch_args;
static VkDeviceSize g_dispatch_offset;
typedef struct {
    vtex *base;
    tex_t texture;
    texel_extent extent;
    VkBufferImageCopy region;
    size_t bytes;
} block_target;
static struct {   // the current render targets and the open dynamic rendering; clears that come before the pass's first draw become its load actions (a tiler then never reads the old contents)
    int n; vtex *ct[8]; VkImageView cv[8]; VkFormat cf[8]; vtex *dt; VkImageView dv; uint32_t w, h; VkSampleCountFlagBits samples; int incompatible_samples, open;
    block_target blocks[8];
    uint32_t clv[8], cly[8]; uint8_t cwr[8], cdrawn[8];   // each colour attachment's level and layer, and whether a draw wrote it since its last same-pass snapshot (fb_prepare)
    uint8_t clr_c[8], clr_d, clr_s; VkClearColorValue cc[8]; float cd; uint32_t cs;
    uint32_t max_layers, layers;
} P;
static int pass_begin(void);
// A linear texture the GPU rendered into is read by the CPU from its storage: Super Mario Odyssey renders the scene's luminance into a 2x1 linear RG11B10F target and computes
// the exposure from it on the CPU; left in the image, the CPU read stale bytes and the exposure swung between a black and a yellow picture. The Metal host renders such
// textures in place on the pool's buffer; here the pass's result is copied into the ring and back into the storage when the submission completes (post kind 3)
static uint64_t g_lin_wb;
static void lin_writeback(vtex *v) {
    size_t width = (size_t)v->w * v->linbpb, n = width * v->h;
    VkDeviceSize o = ring_alloc(n, 16); if (o == VK_WHOLE_SIZE) return;
    VkBufferImageCopy bc = {o, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {v->w, v->h, 1}};
    vkCmdCopyImageToBuffer(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, C->ring, 1, &bc);
    if (v->linrow == width) buffer_written((uintptr_t)v->mem, n, C->ring, o, C->ringp + o);
    else for (uint32_t y = 0; y < v->h; ++y)
        buffer_written((uintptr_t)v->mem + (size_t)y * v->linrow, width, C->ring, o + y * width, C->ringp + o + y * width);
    if (!g_lin_wb++) LOG("vk: linear render target %ux%u format %d goes back to its storage after each pass (serial %llu)", v->w, v->h, (int)v->f.f, (unsigned long long)v->serial);
}
static int g_pass_shader_writes;
static void pass_end(void) {
    if (!P.open) { int pend = P.clr_d || P.clr_s; for (int i = 0; i < 8; i++) pend |= P.clr_c[i]; if (pend) pass_begin(); }   // clears with no draw after them still happen
    if (P.open) {
        visibility_end(&C->visibility, C->cb);
        rp_end(C->cb); P.open = 0;
        if (g_pass_shader_writes) { barrier_all(C->cb); g_pass_shader_writes = 0; }
        // what the pass wrote is read by later passes, samples and copies; their vertex work may start before it ends
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        int wb = 0;
        for (int i = 0; i < P.n; i++) if (P.ct[i] && P.ct[i]->linrow && P.cdrawn[i] && !P.clv[i] && !P.cly[i]) { lin_writeback(P.ct[i]); wb = 1; }
        memset(P.cdrawn, 0, sizeof P.cdrawn);
        if (wb) barrier_all(C->cb);
    }
}
static void diagnostic_note_targets(const rp_desc *pass);
static void begin_tracked_pass(const rp_desc *d) {
    diagnostic_note_targets(d);
    VkFramebuffer fb = rp_begin(C->cb, d);
    if (fb) {
        if (C->nfb == C->cfb) { C->cfb = C->cfb ? 2 * C->cfb : 256; C->fb = realloc(C->fb, C->cfb * sizeof *C->fb); }
        C->fb[C->nfb++] = fb;
    }
}
static int pass_begin(void) {
    if (P.open) return 1;
    if (P.incompatible_samples) return 0;
    if (!P.n && !P.dt) {
        if (!S.rasterizer_discard && !S.feedback_active) return 0;
        P.w = P.h = 1; // Vertex-only work still needs a valid render area.
    }
    rp_desc d = {.n = P.n, .w = P.w, .h = P.h, .layers=P.layers, .samples = P.samples, .layout = VK_IMAGE_LAYOUT_GENERAL};
    for (int i = 0; i < P.n; i++) { d.cv[i] = P.cv[i]; d.cf[i] = P.cf[i]; d.cl[i] = P.clr_c[i] ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; d.cc[i] = P.cc[i]; if (P.clr_c[i]) P.cdrawn[i] = 1; }
    if (P.dt) { d.dv = P.dv; d.df = P.dt->f.f; d.stencil = P.dt->f.stencil; d.dl = P.clr_d ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                d.sl = P.clr_s ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; d.dc = (VkClearDepthStencilValue){P.cd, P.cs}; }
    memset(P.clr_c, 0, sizeof P.clr_c); P.clr_d = P.clr_s = 0;
    if (g_plog) { char b[512]; int o = 0; for (int i = 0; i < P.n; i++) o += snprintf(b + o, sizeof b - o, " c%d=%llu/%d%s", i, P.ct[i] ? (unsigned long long)P.ct[i]->serial : 0ull, (int)d.cf[i], d.cl[i] == VK_ATTACHMENT_LOAD_OP_CLEAR ? "/clr" : "");
                  if (P.dt) snprintf(b + o, sizeof b - o, " d=%llu/%d%s%s", (unsigned long long)P.dt->serial, (int)d.df, d.dl == VK_ATTACHMENT_LOAD_OP_CLEAR ? "/clr" : "", d.sl == VK_ATTACHMENT_LOAD_OP_CLEAR ? "/sclr" : "");
                  LOG("plog: pass %ux%u%s", P.w, P.h, b); }
    begin_tracked_pass(&d); P.open = 1;
    return 1;
}
// The Apple timing mode advances at a pass break after a texture upload or
// the draw threshold. Vulkan can timestamp that boundary without submitting
// early or changing when a guest fence sees the reports.
static void timestamp_cut(void) {
    if (!C->cut_copy && (g_cut_draws<=0 || C->raster_draws<(uint64_t)g_cut_draws)) return;
    uint32_t slot;
    query_page *page=query_slot(&C->time_boundaries,VK_QUERY_TYPE_TIMESTAMP,C->up,&slot);
    vkCmdWriteTimestamp(C->cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,page->pool,slot);
    post_add((post_t){.kind=POST_TIME_BOUNDARY,.samples=&page->samples[slot]});
    C->raster_draws=0; C->cut_copy=0;
}
static image_view_key attachment_key(const vtex *v, VkImageView view) {
    for (int i=0; i<v->nviews; i++) if (v->views[i].v==view) return v->views[i].key;
    return (image_view_key){.range={.layerCount=1}};
}
static void cpu_ready(uint64_t addr, uint64_t size);
static vtex *sampled_scratch(fmt_t format, VkSampleCountFlagBits samples, uint32_t w, uint32_t h, uint32_t levels, VkImageUsageFlags usage);
#include "block_targets.inc"
static void set_rt(const p_set_rt *r) {
    int timing=g_segment_timestamps && C->qp;
    VkImageView old_color[8], old_depth=P.dv;
    int continued=timing && P.open && P.n==r->n;
    if (continued) memcpy(old_color,P.cv,sizeof old_color);
    pass_end();
    memset(&P, 0, sizeof P);
    P.n = r->n > 8 ? 8 : r->n; P.w = P.h = 0x7fffffff;
    P.layers=1; P.max_layers=g_props.limits.maxFramebufferLayers;
    for (int i = 0; i < P.n; i++) {
        if (!r->color[i]) continue;
        const tex_t *attachment=r->color[i];
        const view_t *view=&r->cview[i];
        int has_view=r->has_cview[i];
        tex_t shadow={.d={.target=NVN_TEXTURE_TARGET_2D}};
        vtex *v;
        fmt_t original=fmt_of(attachment->d.format);
        int color_view=has_view && view->set_format && !fmt_of(view->format).depth;
        if (original.dec || (original.depth && color_view)) {
            v=block_target_prepare(i,attachment,view,has_view);
            attachment=&shadow;has_view=0;
        } else v=tex_get(attachment,1);
        if (!v) continue;
        P.ct[i] = v; P.cv[i] = tex_view(v,attachment,view,has_view,-1,VIEW_ATTACHMENT);
        if (!P.cv[i]) { P.ct[i] = NULL; continue; }
        uint32_t layers=attachment_key(v,P.cv[i]).range.layerCount;
        if (layers<P.max_layers) P.max_layers=layers;
        P.cf[i] = view_format(v,view,has_view).f;
        uint32_t lv = has_view && view->set_levels ? view->base_level : 0, w = v->w >> lv ? v->w >> lv : 1, h = v->h >> lv ? v->h >> lv : 1;
        P.clv[i] = lv; P.cly[i] = has_view && view->set_layers ? view->min_layer : 0; P.cwr[i] = 1;
        if (w < P.w) P.w = w; if (h < P.h) P.h = h;
    }
    if (r->depth) {
        vtex *v = tex_get(r->depth, 1);
        if (v && v->f.depth) P.dv = tex_view(v, r->depth, &r->dview, r->has_dview, -1, 1);
        if (P.dv) { P.dt = v; uint32_t lv = r->has_dview && r->dview.set_levels ? r->dview.base_level : 0;
                               if ((v->w >> lv) < P.w) P.w = v->w >> lv ? v->w >> lv : 1; if ((v->h >> lv) < P.h) P.h = v->h >> lv ? v->h >> lv : 1; }
    }
    P.samples = P.dv ? P.dt->samples : 0;
    if (P.dv) { uint32_t layers=attachment_key(P.dt,P.dv).range.layerCount; if (layers<P.max_layers) P.max_layers=layers; }
    for (int i = 0; i < P.n; i++) if (P.cv[i]) {
        if (P.samples && P.samples != P.ct[i]->samples) P.incompatible_samples = 1;
        P.samples = P.ct[i]->samples;
    }
    if (P.incompatible_samples) RED_ONCE(95, "render target sample counts differ");
    if (!P.samples) P.samples = VK_SAMPLE_COUNT_1_BIT;
    if (P.w == 0x7fffffff) P.w = P.h = 1;
    if (timing && (!continued || old_depth!=P.dv || memcmp(old_color,P.cv,sizeof old_color))) timestamp_cut();
}

// Pipelines, cached by everything they are built from
typedef struct {
    shader_t *vs, *fs; VkShaderModule fragment_module; int prim;
    va_st attr[32]; int nattr; vs_st stream[16]; int nstream, split_instances;
    blend_st blend[8]; color_st color; chmask_st chmask; ds_st ds; poly_st poly; int samples, logic, alpha_to_coverage;
    VkFormat cf[8]; int nc; VkFormat df; int stencil, origin_ll, rasterizer_discard, depth_clamp;
} pso_key;
typedef struct { pso_key k; uint64_t h; VkPipeline p; } pso_ent;
static pso_ent *g_pso; static int g_npso, g_cpso;
static uint64_t fnv(const void *p, size_t n) { const uint8_t *b = p; uint64_t h = 0xcbf29ce484222325ull; while (n--) h = (h ^ *b++) * 0x100000001b3ull; return h; }
static VkBlendFactor bfac(int f) {
    switch (f) {
        case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR; case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 0x10: case 0x11: case 0x12: case 0x13: RED_ONCE(10, "dual-source blending: Mali has none, the second source reads as one");   /* fall through */
        default: return f == 0x11 || f == 0x13 ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp bop(int e) { switch (e) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; } return VK_BLEND_OP_ADD; }
static VkStencilOp sop(int o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
static VkCompareOp cop(int f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static VkPipeline pso_create(const pso_key *k, VkPipelineLayout pl) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, k->vs->mod, "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, k->fs ? k->fs->mod : VK_NULL_HANDLE, "main", NULL}};
    if (k->fragment_module) st[1].module=k->fragment_module;
    if (k->vs->subgroup.requiredSubgroupSize) st[0].pNext = &k->vs->subgroup;
    if (k->fs && k->fs->subgroup.requiredSubgroupSize) st[1].pNext = &k->fs->subgroup;
    VkVertexInputBindingDescription vb[16]; VkVertexInputAttributeDescription va[32]; uint32_t nvb = 0, nva = 0; uint32_t used = 0;
    VkVertexInputBindingDivisorDescriptionEXT divisors[16]; uint32_t ndiv=0;
    vertex_classes classes={0};
    int convert_inputs=0;
    for (int i = 0; i < 32; i++) {
        if (!(k->vs->vin_mask >> i & 1)) continue;
        if (i >= k->nattr || k->attr[i].stream >= 16) {
            LOG("[RED] vk: vertex shader %s input %d has no usable NVN attribute or stream", k->vs->key, i);
            return VK_NULL_HANDLE;
        }
        unsigned ct = k->vs->vin[i];
        VkFormat f = vfmt(k->attr[i].fmt, ct);
        unsigned fmt = k->attr[i].fmt;
        // R, RG, RGB, RGBA each list the 8/16/32-bit scaled formats in unsigned/signed pairs.
        // The shader converts raw unsigned words for either sign, including the fetch's integer w=1.
        if (ct <= NVN_CT_INT && fmt >= NVN_FORMAT_R32_UI2F && fmt <= NVN_FORMAT_RGBA32_I2F &&
            (fmt - NVN_FORMAT_R32_UI2F) % 6 < 2) {
            static const VkFormat raw[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
            unsigned index = fmt - NVN_FORMAT_R32_UI2F;
            f = raw[index / 6];
            classes.at[i]=(uint8_t)(4+index%2);
            convert_inputs=1;
        } else if (!f && ct <= NVN_CT_INT) {
            // Fetch using the source class for widening, normalization and w padding.
            // The shader variant bitcasts that result back to the declared input type.
            for (unsigned c = NVN_CT_FLOAT; c <= NVN_CT_INT; c++) {
                f = vfmt(k->attr[i].fmt, c);
                if (f) { classes.at[i]=(uint8_t)(c+1); convert_inputs=1; break; }
            }
        }
        if (!f) {
            const char *type = ct == NVN_CT_FLOAT ? "float" : ct == NVN_CT_INT ? "int" : ct == NVN_CT_UINT ? "uint" : "unknown";
            LOG("[RED] vk: vertex shader %s input %d (%s), NVN format %#x: no Vulkan vertex format preserves the fetched register values",
                k->vs->key, i, type, k->attr[i].fmt);
            return VK_NULL_HANDLE;
        }
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_pd, f, &fp);
        if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            LOG("[RED] vk: vertex shader %s input %d: device does not support Vulkan vertex format %d (NVN %#x)",
                k->vs->key, i, f, k->attr[i].fmt);
            return VK_NULL_HANDLE;
        }
        va[nva++] = (VkVertexInputAttributeDescription){shader_location(k->vs,(unsigned)i), k->attr[i].stream, f, k->attr[i].offset};
        used |= 1u << k->attr[i].stream;
    }
    if (convert_inputs) {
        st[0].module = vertex_variant(k->vs, classes);
        if (!st[0].module) return VK_NULL_HANDLE;
    }
    for (int s = 0; s < 16; s++) if (used >> s & 1) {
        uint32_t stride = s < k->nstream ? k->stream[s].stride : 0, div = s < k->nstream ? k->stream[s].divisor : 0;
        if (div > 1 && stride) {
            if (k->split_instances) stride=0;
            else divisors[ndiv++]=(VkVertexInputBindingDivisorDescriptionEXT){(uint32_t)s,div};
        }
        vb[nvb++] = (VkVertexInputBindingDescription){(uint32_t)s, stride, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    }
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, nvb, vb, nva, va};
    VkPipelineVertexInputDivisorStateCreateInfoEXT rates = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,NULL,ndiv,divisors};
    if (ndiv) vi.pNext=&rates;
    static const VkPrimitiveTopology TOPO[] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, TOPO[k->prim < 15 ? k->prim : 4], VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.rasterizerDiscardEnable = k->rasterizer_discard;
    rs.depthClampEnable = k->depth_clamp;
    // Metal maps NVN's LINE to wireframe and both FILL and POINT to solid.
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    if (k->poly.mode==NVN_POLYGON_MODE_LINE) {
        if (g_non_solid_fill) rs.polygonMode=VK_POLYGON_MODE_LINE;
        else RED_ONCE(116,"wireframe is unavailable on this device; drawing filled triangles");
    }
    rs.cullMode = (VkCullModeFlags)k->poly.cull; rs.frontFace = k->poly.front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (k->origin_ll) rs.frontFace = rs.frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = (k->poly.offset_en & NVN_POLYGON_OFFSET_ENABLE_FILL) != 0;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)(k->samples > 1 ? k->samples : 1)};
    ms.alphaToCoverageEnable=k->alpha_to_coverage;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (k->df) {
        ds.depthTestEnable = k->ds.dt; ds.depthWriteEnable = k->ds.dw; ds.depthCompareOp = k->ds.dt ? cop(k->ds.df) : VK_COMPARE_OP_ALWAYS;
        if (k->stencil && k->ds.st) {
            ds.stencilTestEnable = VK_TRUE;
            ds.front = (VkStencilOpState){sop(k->ds.fail_f), sop(k->ds.zpass_f), sop(k->ds.zfail_f), cop(k->ds.sf_f), 0xff, 0xff, 0};
            ds.back = (VkStencilOpState){sop(k->ds.fail_b), sop(k->ds.zpass_b), sop(k->ds.zfail_b), cop(k->ds.sf_b), 0xff, 0xff, 0};
        }
    }
    VkPipelineColorBlendAttachmentState ba[8];
    for (int i = 0; i < k->nc; i++) {
        const blend_st *b = &k->blend[i]; int en = k->color.blend_en >> i & 1;
        ba[i] = (VkPipelineColorBlendAttachmentState){en, bfac(b->src_rgb), bfac(b->dst_rgb), bop(b->eq_rgb), bfac(b->src_a), bfac(b->dst_a), bop(b->eq_a), (k->chmask.mask >> (4 * i)) & 15};
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0,
        k->logic, k->logic ? (VkLogicOp)k->color.logic_op : VK_LOGIC_OP_COPY, (uint32_t)k->nc, ba, {0}};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof dyn / sizeof *dyn, dyn};
    VkPipelineRenderingCreateInfo rinfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, (uint32_t)k->nc, k->cf, k->df, k->stencil ? k->df : VK_FORMAT_UNDEFINED};
    VkRenderPass crp = VK_NULL_HANDLE;
    if (!g_dynrend) { VkAttachmentLoadOp ld[8]; for (int i = 0; i < 8; i++) ld[i] = VK_ATTACHMENT_LOAD_OP_LOAD;
                      crp = rp_get(k->cf, ld, k->nc, k->df, k->stencil, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_GENERAL, ms.rasterizationSamples); }
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, g_dynrend ? &rinfo : NULL, 0, k->fs ? 2 : 1, st, &vi, &ia, NULL, &vp, &rs, &ms, &ds, &cb, &dy, pl, crp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p; VkResult r = vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &gi, NULL, &p);
    if (r != VK_SUCCESS) { LOG("[RED] vk: pipeline for %s / %s not created (%d)", k->vs->key, k->fs ? k->fs->key : "-", (int)r); p = VK_NULL_HANDLE; }
    return p;
}
static VkPipeline pso_get(const pso_key *k, VkPipelineLayout pl) {
    uint64_t h = fnv(k, sizeof *k);
    for (int i = 0; i < g_npso; i++) if (g_pso[i].h == h && !memcmp(&g_pso[i].k, k, sizeof *k)) return g_pso[i].p;
    VkPipeline p = pso_create(k, pl);
    if (g_npso == g_cpso) { g_cpso = g_cpso ? 2 * g_cpso : 256; g_pso = realloc(g_pso, g_cpso * sizeof *g_pso); }
    g_pso[g_npso++] = (pso_ent){*k, h, p};
    return p;
}

// ---------- Draw resources ----------
typedef struct { uint64_t addr, size, gen; VkDeviceSize off; } dcopy;
#define NDC 4096
static dcopy g_dc[NDC]; static int g_ndc;
// Overlay the intersection of one ordered guest write onto a staging range.
static int snapshot_overlay(const gw_t *w, uint64_t addr, uint64_t size, VkDeviceSize offset) {
    if (!overlaps(addr, size, w->addr, w->size)) return 0;
    uint64_t lo = addr > w->addr ? addr : w->addr;
    uint64_t count = size - (lo - addr), available = w->size - (lo - w->addr);
    if (count > available) count = available;
    VkBufferCopy region = {w->off + (lo - w->addr), offset + (lo - addr), count};
    vkCmdCopyBuffer(C->cb, w->buf, C->ring, 1, &region);
    return 1;
}
static VkDeviceSize copy_in_at(uint64_t addr, uint64_t size, VkDeviceSize align, VkDeviceSize prefix) {   // a prefix can align a typed view inside an overlapping storage range
    gw_wait_prior(addr, size);
    dcopy *e = &g_dc[(addr >> 4 ^ addr >> 13) % NDC];
    if (e->addr == addr && e->size >= size && e->gen == g_wgen && e->gen && e->off % align == prefix) return e->off;
    int gpu = 0;
    pthread_mutex_lock(&g_gwmu);
    for (size_t i = 0; i < g_ngw; i++) gpu |= overlaps(addr, size, g_gw[i].addr, g_gw[i].size);
    pthread_mutex_unlock(&g_gwmu);
    if (gpu) pass_end();
    if (prefix >= align || size > UINT64_MAX - prefix) die("Vulkan: invalid buffer snapshot padding");
    VkDeviceSize base = ring_alloc(size + prefix, align), o = base + prefix;
    if (prefix) memset(C->ringp + base, 0, prefix);
    memcpy(C->ringp + o, (const void *)(uintptr_t)addr, size);
    if (gpu) {
        barrier_all(C->cb);
        pthread_mutex_lock(&g_gwmu);
        // Copy in write order. Each later range overwrites only its intersection;
        // untouched bytes retain the CPU snapshot made above.
        for (size_t i = 0; i < g_ngw; i++) {
            if (snapshot_overlay(&g_gw[i], addr, size, o))
                barrier_all(C->cb); // later intersections may write these bytes again
        }
        pthread_mutex_unlock(&g_gwmu);
    }
    *e = (dcopy){addr, size, g_wgen, o};
    return o;
}
static VkDeviceSize copy_in(uint64_t addr, uint64_t size, VkDeviceSize align) { return copy_in_at(addr, size, align, 0); }
static uint64_t g_tex_miss[4];   // texture slots read as the black dummy: 0 no handle bound, 1 handle not registered, 2 texture not created, 3 no view (logged with the frame stats)
int nvn_dpool_base(const void *pool, int texture);   // shared recorder's host-table numbering
// Index widening/scanning and descriptor-handle decoding happen on the CPU.
// Finish only an actual GPU dependency, before allocating this command's sets
// or buffers: a submission cut here would invalidate those allocations later.
static void begin_segment(void);
static void cpu_access_ready(uint64_t addr, uint64_t size, int writing) {
    if (!size) return;
    for (int i = 0; i < P.n; i++) {
        const vtex *v = P.ct[i];
        if (v && v->linrow && (P.cdrawn[i] || P.clr_c[i]) &&
            overlaps(addr, size, (uintptr_t)v->mem, (uint64_t)v->linrow * v->h)) { pass_end(); break; }
    }
    gw_wait_prior(addr, size);
    int gpu = 0;
    pthread_mutex_lock(&g_gwmu);
    for (size_t i = 0; i < g_ngw && !gpu; i++) {
        const gw_t *w = &g_gw[i];
        if (!overlaps(addr, size, w->addr, w->size)) continue;
        // Even a known fill will publish on completion. Land it before
        // overwriting these bytes, or it can undo the new texture contents.
        if (writing) { gpu = 1; break; }
        if (w->cpu_known) continue;
        uint64_t at = addr > w->addr ? addr : w->addr;
        uint64_t left = size - (at - addr), wn = w->size - (at - w->addr);
        if (left > wn) left = wn;
        // Later inline updates/fills can completely replace these GPU bytes.
        // Their mapped snapshots are known without submitting any GPU work.
        while (left) {
            uint64_t covered = 0;
            for (size_t j = i + 1; j < g_ngw; j++) {
                const gw_t *k = &g_gw[j];
                if (k->cpu_known && at >= k->addr && at - k->addr < k->size) {
                    uint64_t n = k->size - (at - k->addr);
                    if (n > covered) covered = n;
                }
            }
            if (!covered) { gpu = 1; break; }
            if (covered >= left) break;
            at += covered; left -= covered;
        }
    }
    if (!gpu) for (size_t i = 0; i < g_ngw; i++) {
        const gw_t *w = &g_gw[i];
        if (!w->cpu_known || !overlaps(addr, size, w->addr, w->size)) continue;
        uint64_t lo = addr > w->addr ? addr : w->addr;
        uint64_t n = size - (lo - addr), wn = w->size - (lo - w->addr);
        if (n > wn) n = wn;
        dm_guest_write_prepare((void *)(uintptr_t)lo, n);
        memcpy((void *)(uintptr_t)lo, w->p + (lo - w->addr), n);
    }
    pthread_mutex_unlock(&g_gwmu);
    if (gpu) { mtl_wait(mtl_sub_commit(), UINT64_MAX); begin_segment(); }
}
static void cpu_ready(uint64_t addr, uint64_t size) { cpu_access_ready(addr,size,0); }
static void uniform_ready(int stage, int reg, int64_t off, uint32_t count) {
    if (stage < 0 || stage >= 6 || reg < 0 || reg >= 16 || off < 0 || !count) return;
    const p_bind_buf *u = &S.ubo[stage][reg];
    if (!u->addr || (uint64_t)off >= u->size || u->addr > UINT64_MAX - (uint64_t)off) return;
    uint64_t size = 8ull * (count - 1) + 4;
    if (size > u->size - (uint64_t)off) size = u->size - (uint64_t)off;
    cpu_ready(u->addr + off, size);
}
static uint64_t uniform_storage_field(int stage, int reg, int64_t off) {
    if (stage < 0 || stage >= 6 || reg < 0 || reg >= 16) return 0;
    const p_bind_buf *u = &S.ubo[stage][reg];
    return dm_vk_uniform_address_field(u->addr, u->size, off);
}
static void shader_cpu_ready(const shader_t *s, int stage) {
    if (!s) return;
    for (int i = 0; i < s->nb; i++) {
        const sbind *b = &s->b[i];
        if (b->kind == K_TEXU) uniform_ready(stage, b->reg, b->off, b->table ? 8192 : b->count);
        if (b->kind == K_SBUFU) {
            uint64_t field = uniform_storage_field(stage, b->reg, b->off);
            if (field) cpu_ready(field, 8);
        }
        if (b->skind == S_SAMPU) uniform_ready(stage, b->sreg, b->soff, b->table ? 8192 : b->count);
        if (!b->table && b->count == 1) {
            if (b->kind == K_TEXU) uniform_ready(stage, b->twin.texture.reg, b->twin.texture.off, 1);
            if (b->skind == S_SAMPU) uniform_ready(stage, b->twin.sampler.reg, b->twin.sampler.off, 1);
        }
    }
}
// Same ABI as Metal's TEXU/SAMPU: handles occupy 8-byte elements, with the
// texture ID in the low 20 bits and sampler ID in the next 12 bits. These IDs
// are relative to the pools bound at replay time, unlike recorded slot binds.
static uint32_t uniform_handle(int stage, int reg, int64_t off) {
    if (stage < 0 || stage >= 6 || reg < 0 || reg >= 16 || off < 0) return 0;
    const p_bind_buf *u = &S.ubo[stage][reg];
    if (!u->addr || u->size < 4 || (uint64_t)off > u->size - 4 || u->addr > UINT64_MAX - (uint64_t)off - 4) return 0;
    if (!mtl_addr(u->addr + off, 4, NULL, NULL)) return 0;
    uint32_t handle; memcpy(&handle, (const void *)(uintptr_t)(u->addr + off), 4);
    return handle;
}
static uint64_t sampled_handle(int stage, const sbind *b, uint32_t element) {
    uint64_t handle = 0;
    if ((b->kind == K_TEX || b->kind == K_TBUF) && b->reg >= 0 && (uint32_t)b->reg + element < NTEX) handle = dm_vk_texture_slot(S.tex[stage], S.img[stage], b->reg + element);
    else if (b->kind==K_TEX_SLOTS && element<32) {
        handle=S.tex[stage][element];
        if (!handle) handle=S.tex[stage][DMSH_SEPTEX_SLOT0+element];
    }
    else if (b->kind==K_TEX_C2) handle=dm_vk_c2_handle(S.tex[stage],S.img[stage],S.samp[stage],(uint64_t)b->off+8ull*element);
    else if (b->kind == K_TEXU && b->off >= 0) {
        uint32_t raw = uniform_handle(stage, b->reg, b->off + 8ll * element);
        if (b->count == 1) raw |= uniform_handle(stage, b->twin.texture.reg, b->twin.texture.off);
        uint32_t tid = raw & 0xfffff;
        handle = tid ? (uint64_t)tid + S.tex_base : 0;
    }
    uint64_t sid = 0;
    uint32_t sampler_element = b->type == BT_SAMP ? element : 0; // a paired texture array may share one fixed sampler slot
    if (b->skind == S_SAMP && b->sreg >= 0 && (uint32_t)b->sreg + sampler_element < 128) sid = S.samp[stage][b->sreg + sampler_element] >> 20;
    else if (b->skind == S_SAMP_TEX) {
        if (b->type == BT_SAMP && b->sreg >= 0 && (uint32_t)b->sreg + element < NTEX) sid = S.tex[stage][b->sreg + element] >> 20;
        else sid = handle >> 20;
    }
    else if (b->skind==S_SAMP_SLOTS && element<32) {
        sid=S.samp[stage][element]>>20;
        if (!sid) sid=S.tex[stage][element]>>20;
    }
    else if (b->skind==S_SAMP_C2) {
        uint64_t raw=dm_vk_c2_handle(S.tex[stage],S.img[stage],S.samp[stage],(uint64_t)b->soff+8ull*element);
        if (b->driver_sampler>0) raw|=dm_vk_c2_handle(S.tex[stage],S.img[stage],S.samp[stage],(uint32_t)b->driver_sampler);
        sid=raw>>20;
    }
    else if (b->skind == S_SAMP_ID && b->sreg >= 0) sid = b->sreg + (b->type == BT_SAMP ? element : 0);
    else if (b->skind == S_SAMPU && b->soff >= 0) {
        uint32_t raw = uniform_handle(stage, b->sreg, b->soff + 8ll * element);
        if (b->count == 1) raw |= uniform_handle(stage, b->twin.sampler.reg, b->twin.sampler.off);
        sid = raw >> 20;
        if (sid) sid += S.samp_base;
    }
    return (handle & 0xfffff) | sid << 20;
}
#include "uniform_table.inc"
#include "attachment_feedback.inc"
// A float declaration on these integer formats reads the stored IEEE words,
// including denormals used as indices. Match Metal's tbuf_view_format.
static NVNformat float_word_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: return NVN_FORMAT_R32F;
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return NVN_FORMAT_RG32F;
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return NVN_FORMAT_RGBA32F;
        default: return NVN_FORMAT_NONE;
    }
}
static VkImageView tex_slot(uint64_t h, const sbind *b, int stage, border_view *sampled) {
    VkImageView dummy = sampled_dummy(b->view, b->num, b->multisample, b->depth_image);
    int tid = (int)(h & 0xfffff);
    if (!tid) { g_tex_miss[0]++; return dummy; }
    treg_t tr; vtex *v = registered_texture(tid, &tr);
    if (!tr.ok) { g_tex_miss[1]++; return dummy; }
    if (tr.t.d.target == NVN_TEXTURE_TARGET_BUFFER && b->view == VK_IMAGE_VIEW_TYPE_2D && (b->shape.fetch1d || b->kind == K_TEXU)) {
        RED_ONCE(24, "wrapped buffer fetch requires a 16384-texel row view"); return VK_NULL_HANDLE;
    }
    if (!v) { g_tex_miss[2]++; return dummy; }
    if (!compatible_dimension(v, b->view) || b->multisample != (v->samples > 1) ||
        (b->depth_image && !v->f.depth)) return dummy;
    int a = stage == 1 ? fb_slot(v, &tr, b, g_fb_mask) : -1;
    if (a >= 0) {
        // Recreate the sampled format and channel mapping on the copied
        // subresource. Its selected mip and layer are now at (0, 0).
        v = g_fb_active[a]->image;
        if (!tr.has_v) tr.v = (view_t){0};
        tr.has_v = tr.v.set_levels = tr.v.set_layers = 1;
        tr.v.base_level = tr.v.min_layer = 0;
        tr.v.nlevels = tr.v.nlayers = 1;
    }
    if (b->num == 0) {
        NVNformat fit = float_word_format(view_format(v, &tr.v, tr.has_v).f);
        if (fit != NVN_FORMAT_NONE) {
            tr.has_v = tr.v.set_format = 1;
            tr.v.format = fit;
        }
    }
    VkImageView view = tex_view(v, &tr.t, &tr.v, tr.has_v, b->view, 0);
    if (view && sampled) *sampled = sampler_view(v,&tr);
    if (!view) g_tex_miss[3]++;
    return view ? view : dummy;
}
// All storage descriptors of a command are resolved together. Overlapping
// ranges share one backing allocation, so writes and atomics through either
// descriptor see the same bytes, including across vertex and fragment sets.
#define MAX_STAGE_TEXELS (DM_VK_MAX_BINDINGS * 32)
#define MAX_BUFFER_BINDINGS (2 * (MAX_STAGE_TEXELS + DM_VK_MAX_BINDINGS))
typedef struct { uint64_t addr, size; VkBuffer buf; VkDeviceSize off; uint8_t *p; uint64_t texel; VkDeviceSize texel_align; int writable, conflict; } storage_range;
static storage_range g_storage[MAX_BUFFER_BINDINGS]; static size_t g_nstorage;
typedef struct { const uint8_t *mem; uint64_t size; int addressed; } pool_range;
static pool_range *g_pool;
static size_t g_npool, g_cpool;
static pool_range *g_addressed;static size_t g_naddressed;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static int is_texel(const sbind *b) { return b->type == BT_TBUF || b->type == BT_UTB || b->type == BT_STB; }
typedef struct { p_bind_buf range; VkFormat format; uint32_t bytes, swizzle, scaled; VkBufferUsageFlags single_usage; } texel_range;
// The staging buffers carry both texel usage bits. Their views must meet
// both limits, even if this binding only reads. For three-component formats
// Vulkan uses the component size for its single-texel alignment rule.
static VkDeviceSize texel_alignment(uint32_t bytes) {
    if (!g_texel_limits.uniformTexelBufferOffsetAlignmentBytes) return g_props.limits.minTexelBufferOffsetAlignment;
    VkDeviceSize unit=bytes%3==0 ? bytes/3 : bytes;
    VkDeviceSize u=g_texel_limits.uniformTexelBufferOffsetAlignmentBytes, s=g_texel_limits.storageTexelBufferOffsetAlignmentBytes;
    if (g_texel_limits.uniformTexelBufferOffsetSingleTexelAlignment && unit<u) u=unit;
    if (g_texel_limits.storageTexelBufferOffsetSingleTexelAlignment && unit<s) s=unit;
    return u>s ? u : s;
}
// Scaled vertex formats are often vertex-only in Vulkan. A sampled UNORM
// view preserves 8/16-bit channel data even on those devices; the shader
// reconstructs the signed/unsigned value before applying the NVN swizzle.
static VkFormat scaled_texel_format(unsigned format,uint32_t *encoding) {
    static const VkFormat normalized[][2]={
        {VK_FORMAT_R8_UNORM,VK_FORMAT_R16_UNORM}, {VK_FORMAT_R8G8_UNORM,VK_FORMAT_R16G16_UNORM},
        {VK_FORMAT_R8G8B8_UNORM,VK_FORMAT_R16G16B16_UNORM}, {VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R16G16B16A16_UNORM}
    };
    uint32_t widths[4]={0}; unsigned channels, sign;
    VkFormat fallback;
    if (format==NVN_FORMAT_RGB10A2_UI2F || format==NVN_FORMAT_RGB10A2_I2F) {
        channels=4; widths[0]=widths[1]=widths[2]=10; widths[3]=2;
        sign=format==NVN_FORMAT_RGB10A2_I2F; fallback=VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    } else {
        if (format<NVN_FORMAT_R8_UI2F || format>NVN_FORMAT_RGBA32_I2F) return VK_FORMAT_UNDEFINED;
        unsigned index=format-NVN_FORMAT_R8_UI2F, lane=(index%6)/2;
        channels=1+index/6; sign=index&1;
        if (lane==2) {
            // Each guest word becomes two UNORM16 lanes. A floating-point
            // view of the original bits would lose NaNs or denormals on some GPUs.
            *encoding=(channels<<TEXEL_WORD_SHIFT)|(sign<<20);
            return VK_FORMAT_R16G16_UNORM;
        }
        fallback=normalized[channels-1][lane];
        for (unsigned c=0;c<channels;c++) widths[c]=lane ? 16 : 8;
    }
    VkFormat native=vfmt(format,NVN_CT_FLOAT);
    VkFormatProperties props; vkGetPhysicalDeviceFormatProperties(g_pd,native,&props);
    if (props.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) return native;
    *encoding=sign<<20;
    for (unsigned c=0;c<channels;c++) *encoding|=widths[c]<<(5*c);
    return fallback;
}
static int texel_binding(const sbind *b, int stage, uint32_t element, texel_range *out) {
    *out = (texel_range){0};
    uint64_t handle = b->kind == K_IMG && b->reg >= 0 && (uint32_t)b->reg + element < 32 ? S.img[stage][b->reg + element] : descriptor_handle(stage, b, element);
    int id = (int)(handle & 0xfffff), ok = 1;
    pthread_mutex_lock(&g_rmu);
    treg_t tr = id > 0 && id < g_ntreg ? g_treg[id] : (treg_t){0};
    if (tr.ok && tr.t.d.target == NVN_TEXTURE_TARGET_BUFFER && tr.t.mem) {
        int original_bw, original_bh, original_bytes = nvn_fmt_bpb(tr.t.d.format, &original_bw, &original_bh);
        int format = tr.has_v && tr.v.set_format ? tr.v.format : tr.t.d.format;
        fmt_t f = fmt_of(format);
        int swap_rb=0;
        if (b->type!=BT_STB) {
            NVNformat fit = b->num == 0 ? float_word_format(f.f) : NVN_FORMAT_NONE;
            if (fit != NVN_FORMAT_NONE) f.f = fmt_of(fit).f;
            // These views read guest bytes directly. NVN's RGB family puts
            // red in the low bits, and the BGR family puts blue there.
            switch (format) {
                case NVN_FORMAT_RGB565: f.f=VK_FORMAT_B5G6R5_UNORM_PACK16; break;
                case NVN_FORMAT_BGR565: f.f=VK_FORMAT_R5G6B5_UNORM_PACK16; break;
                case NVN_FORMAT_RGB5: case NVN_FORMAT_RGB5A1:
                    swap_rb=1; f.f=VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
                case NVN_FORMAT_BGR5: case NVN_FORMAT_BGR5A1:
                    f.f=VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
                case NVN_FORMAT_A1BGR5: f.f=VK_FORMAT_R5G5B5A1_UNORM_PACK16; break;
            }
        }
        uint32_t scaled=0;
        if (!f.f && b->type!=BT_STB && b->num==0) f.f=scaled_texel_format(format,&scaled);
        int bw, bh, bytes = nvn_fmt_bpb(format, &bw, &bh);
        int physical_bytes=scaled>>TEXEL_WORD_SHIFT ? 4 : bytes;
        uint64_t size = (uint64_t)tr.t.d.width * (unsigned)original_bytes;
        if (original_bytes <= 0 || original_bw != 1 || original_bh != 1 || bytes <= 0 || bw != 1 || bh != 1 || f.dec || f.depth || f.f == VK_FORMAT_UNDEFINED ||
            !size || size % bytes || size / physical_bytes > g_props.limits.maxTexelBufferElements || size > UINT64_MAX - (uintptr_t)tr.t.mem ||
            (tr.has_v && tr.v.set_target && tr.v.target!=NVN_TEXTURE_TARGET_BUFFER)) ok=0;
        if (ok && tr.t.pool) {
            uint64_t base = (uintptr_t)tr.t.pool->mem, addr = (uintptr_t)tr.t.mem;
            if (addr < base || addr - base > tr.t.pool->size || size > tr.t.pool->size - (addr - base)) ok = 0;
        }
        VkFormatProperties fp = {0};
        if (ok) vkGetPhysicalDeviceFormatProperties(g_pd, f.f, &fp);
        const VkFormatFeatureFlags both=VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT|VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
        int storage=b->type==BT_STB;
        if (!(fp.bufferFeatures & (storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT))) ok=0;
        if (ok) {
            *out=(texel_range){.range={.addr=(uintptr_t)tr.t.mem,.size=size},.format=f.f,.bytes=(uint32_t)physical_bytes,.scaled=scaled};
            if (!storage) {
                // Storage operations address the original channels, like
                // Metal's identity image view of a swizzled registration.
                uint8_t sw[4]={2,3,4,5};
                sampled_swizzle(sw,f,&tr.t,&tr.v,tr.has_v);
                for (unsigned c=0;c<4;c++) if (sw[c]>5) sw[c]=2+c;
                if (swap_rb) for (unsigned c=0;c<4;c++) {
                    if (sw[c]==2) sw[c]=4; else if (sw[c]==4) sw[c]=2;
                }
                uint32_t map=sw[0]|(sw[1]<<3)|(sw[2]<<6)|(sw[3]<<9);
                if (map!=(2|3<<3|4<<6|5<<9)) out->swizzle=map+1;
            }
            if ((fp.bufferFeatures & both)!=both)
                out->single_usage=storage ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
        }
    }
    pthread_mutex_unlock(&g_rmu);
    if (!out->range.addr && b->type==BT_STB && ok) {
        uint32_t code=b->storage_image ? b->storage_image-1 : UINT32_MAX;
        if (!code) { // formatless shader: pick the declared numeric class
            if (b->texel_atomic) code=b->num==1 ? 24 : 33;
            else code=b->num==1 ? 21 : b->num==2 ? 30 : 1;
        }
        storage_texel_format f=spirv_texel_format(code);
        VkFormatProperties props={0};
        if (f.bytes) vkGetPhysicalDeviceFormatProperties(g_pd,f.format,&props);
        VkFormatFeatureFlags need=VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
        if (b->texel_atomic) need|=VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT;
        if ((props.bufferFeatures & need)!=need) ok=0;
        out->format=f.format; out->bytes=f.bytes;
        if (!(props.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT))
            out->single_usage=VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    }
    if (!ok) RED_ONCE(21, "unsupported typed buffer range, format, view or storage binding");
    return ok;
}
static uint64_t pool_remaining(uint64_t addr);
static p_bind_buf storage_binding(const sbind *b, int stage) {
    p_bind_buf u = {0};
    if (b->type != BT_SSBO || b->count != 1 || b->reg < 0 || b->reg >= 16) return u;
    if (b->kind == K_SBUF) u = S.ssbo[stage][b->reg];
    else if (b->kind == K_UBOP) u = S.ubo[stage][b->reg];
    else if (b->kind == K_SBUFU) {
        uint64_t field = uniform_storage_field(stage, b->reg, b->off);
        if (field && mtl_addr(field, 8, NULL, NULL)) memcpy(&u.addr, (const void *)(uintptr_t)field, 8);
    }
    // Like Metal's sbw_len: size zero means the rest of the registered pool,
    // not an empty descriptor. Resolve it afresh after pool replacement.
    if (u.addr && !u.size) u.size = pool_remaining(u.addr);
    return u;
}
static int storage_order(const void *a, const void *b) {
    uint64_t x = ((const storage_range *)a)->addr, y = ((const storage_range *)b)->addr;
    return (x > y) - (x < y);
}
static const storage_range *prepared_storage(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off, uint8_t **p) {
    for (size_t i = 0; i < g_nstorage; i++) {
        const storage_range *r = &g_storage[i];
        if (r->buf && addr >= r->addr && addr - r->addr < r->size && size <= r->size - (addr - r->addr)) {
            *buf = r->buf; *off = r->off + addr - r->addr; *p = r->p + (addr - r->addr); return r;
        }
    }
    return 0;
}
// Three-component formats may need up to two more alignment units to reach
// a whole texel. Only expose a padded view if it still fits the device limit.
static int texel_prefix(const texel_range *t, VkDeviceSize offset, VkDeviceSize *prefix) {
    VkDeviceSize align=texel_alignment(t->bytes), pad=offset%align;
    for (int i=0;i<3 && pad<=offset;i++,pad+=align) {
        if (pad%t->bytes) continue;
        if (pad/t->bytes+t->range.size/t->bytes>g_props.limits.maxTexelBufferElements) break;
        *prefix=pad; return 1;
    }
    RED_ONCE(23,"writable typed alias cannot fit a whole-texel prefix within the device view limit");
    return 0;
}
static int shader_texel_biases(const shader_t *s, texel_bias *edits, size_t *count) {
    for (int i=0;i<s->nb;i++) {
        const sbind *b=&s->b[i]; if (!is_texel(b) || b->kind==K_GEOMETRY) continue;
        for (uint32_t j=0;j<b->count;j++) {
            texel_range t; if (!texel_binding(b,s->guest_stage,j,&t)) return 0;
            if (!t.range.addr) continue;
            VkBuffer buf; VkDeviceSize offset, prefix; uint8_t *mapped;
            const storage_range *group=prepared_storage(t.range.addr,t.range.size,&buf,&offset,&mapped);
            if (!group) return 0;
            prefix=0;
            if (group->writable && group->conflict && !texel_prefix(&t,offset,&prefix)) return 0;
            if (prefix || t.swizzle || t.scaled) edits[(*count)++]=(texel_bias){b->binding,j,(uint32_t)(prefix/t.bytes),t.swizzle,t.scaled};
        }
    }
    return 1;
}
#include "native_reduction.inc"
static int shader_reductions(const shader_t *s, sample_reduction **edits, size_t *capacity, size_t *count) {
    *count = 0;
    if (!g_has_reduction_samplers) return 1;
    if (native_shader_reduction(s)) return 1;
    for (int i=0; i<s->nb; i++) {
        const sbind *b = &s->b[i];
        if (b->type != BT_CIS && b->type != BT_SAMP) continue;
        for (uint32_t element=0; element<b->count; element++) {
            uint64_t handle = descriptor_handle(s->guest_stage, b, element);
            int sid = (int)(handle >> 20);
            pthread_mutex_lock(&g_rmu);
            samp_par p = sid > 0 && sid < g_nsreg && g_sreg_ok[sid] ? g_sreg[sid] : (samp_par){0};
            pthread_mutex_unlock(&g_rmu);
            if (p.reduction != NVN_SAMPLER_REDUCTION_MIN && p.reduction != NVN_SAMPLER_REDUCTION_MAX) continue;
            float anisotropy = fminf(g_max_anisotropy, fmaxf(1, floorf(p.aniso)));
            if (anisotropy > 1) {
                RED_ONCE(121, "shader sampler reduction anisotropy remains unsupported");
                return 0;
            }
            float lo = p.minf >= 2 ? fmaxf(0, p.lod_min) : 0;
            float hi = p.minf >= 2 ? fmaxf(lo, p.lod_max) : 0;
            float limit = g_props.limits.maxSamplerLodBias;
            float lod_bias = fminf(fminf(15.99f, limit), fmaxf(fmaxf(-16, -limit), p.lod_bias));
            if (*count == *capacity) {
                if (*capacity > SIZE_MAX / (2 * sizeof **edits)) return 0;
                size_t next = *capacity ? 2 * *capacity : 16;
                void *grown = realloc(*edits, next * sizeof **edits);
                if (!grown) return 0;
                *edits = grown; *capacity = next;
            }
            (*edits)[(*count)++] = (sample_reduction){.binding=b->binding, .element=element,
                .minimum=p.reduction == NVN_SAMPLER_REDUCTION_MIN,
                .min_linear=p.minf == 1 || p.minf == 3 || p.minf == 5, .mag_linear=p.magf != 0,
                .min_lod=lo, .max_lod=hi, .bias=lod_bias, .max_bias=limit};
        }
    }
    return 1;
}
#include "wrapped_buffer.inc"
#include "global_memory.inc"
static int prepare_storage(shader_t **first, shader_t **second) {
    g_nstorage = 0;
    int needs_texel_variant=0;
    if (!prepare_tables(first) || (second && !prepare_tables(second))) return 0;
    if (!prepare_wrapped_buffers(first) || (second && !prepare_wrapped_buffers(second))) return 0;
    shader_t *stages[] = {*first, second ? *second : NULL};
    for (int st = 0; st < 2; st++) {
        shader_t *s = stages[st]; if (!s) continue;
        uint32_t texel_count=0;
        for (int i = 0; i < s->nb; i++) {
            const sbind *b = &s->b[i];
            if (b->kind==K_GEOMETRY || b->kind==K_MESH) continue;
            uint32_t count = is_texel(b) ? b->count : 1;
            if (is_texel(b)) {
                if (count>MAX_STAGE_TEXELS-texel_count) {
                    RED_ONCE(23,"typed descriptor count exceeds the per-stage replay capacity");return 0;
                }
                texel_count+=count;
            }
            for (uint32_t element = 0; element < count; element++) {
            p_bind_buf u = storage_binding(b, s->guest_stage);
            VkDeviceSize texel_align=0;
            if (is_texel(b)) {
                texel_range t; if (!texel_binding(b,s->guest_stage,element,&t)) return 0;
                u=t.range; if (u.addr) texel_align=texel_alignment(t.bytes);
                needs_texel_variant|=t.swizzle!=0 || t.scaled!=0;
            }
            if (!u.addr || !u.size) continue;
            if (u.size > UINT64_MAX - u.addr) return 0;
            if (!is_texel(b) && u.size > g_props.limits.maxStorageBufferRange) { RED_ONCE(1, "storage range exceeds the device descriptor limit: requires shader address lowering"); return 0; }
            if (g_nstorage == sizeof g_storage / sizeof *g_storage) die("Vulkan: too many storage bindings");
            g_storage[g_nstorage++] = (storage_range){.addr=u.addr, .size=u.size, .texel=u.addr, .texel_align=texel_align,
                .writable=(b->type==BT_SSBO || b->type==BT_STB) && !b->read_only};
            }
        }
    }
    if (!collect_global_memory(stages,2)) return 0;
    qsort(g_storage, g_nstorage, sizeof *g_storage, storage_order);
    size_t n = 0;
    for (size_t i = 0; i < g_nstorage; i++) {
        storage_range *r = &g_storage[i];
        if (n && overlaps(g_storage[n-1].addr, g_storage[n-1].size, r->addr, r->size)) {
            storage_range *prev = &g_storage[n-1];
            // Both alignments are powers of two. The stricter residue satisfies
            // both if they agree modulo the smaller alignment.
            VkDeviceSize common=prev->texel_align<r->texel_align ? prev->texel_align : r->texel_align;
            if (common && prev->texel%common!=r->texel%common) prev->conflict=1;
            if (r->texel_align>prev->texel_align) { prev->texel_align=r->texel_align; prev->texel=r->texel; }
            prev->writable|=r->writable;
            uint64_t end = r->addr + r->size;
            if (end > prev->addr + prev->size) prev->size = end - prev->addr;
        } else g_storage[n++] = *r;
    }
    g_nstorage = n;

    for (size_t i = 0; i < n; i++) {
        storage_range *r = &g_storage[i];
        VkDeviceSize ta=r->texel_align, align=g_props.limits.minStorageBufferOffsetAlignment;
        if (ta>align) align=ta;
        VkDeviceSize prefix=ta ? (ta-(r->texel-r->addr)%ta)%ta : 0;
        if (r->conflict && r->writable) {
            // Keep the guest residue for every power-of-two texel alignment
            // and reserve space for a possible three-component view prefix.
            prefix=2*ta+r->addr%ta;
            if (align<4*ta) align=4*ta;
            needs_texel_variant=1;
        }
        if (!prefix && gw_find(r->addr, r->size, align, &r->buf, &r->off, &r->p)) continue;
        VkDeviceSize offset = copy_in_at(r->addr, r->size, align, prefix);
        if (offset == VK_WHOLE_SIZE) return 0;
        r->buf = C->ring; r->off = offset; r->p = C->ringp + offset;
    }
    for (int st=0;st<2;st++) {
        shader_t *s=stages[st];if(!s) continue;
        uint32_t bias[DM_VK_MAX_BINDINGS]={0};
        for (int b=0;b<s->nb;b++) {
            p_bind_buf u=storage_binding(&s->b[b],s->guest_stage);
            if(!u.addr || !u.size) continue;
            for(size_t i=0;i<n;i++) if(u.addr>=g_storage[i].addr && u.addr-g_storage[i].addr<g_storage[i].size) {
                bias[b]=(uint32_t)((g_storage[i].off+u.addr-g_storage[i].addr)%g_props.limits.minStorageBufferOffsetAlignment);break;
            }
        }
        static texel_bias texels[MAX_STAGE_TEXELS];
        size_t nt=0;
        if (needs_texel_variant && !shader_texel_biases(s,texels,&nt)) return 0;
        static sample_reduction *reductions;
        static size_t reduction_capacity;
        size_t nr;
        if (!shader_reductions(s, &reductions, &reduction_capacity, &nr)) return 0;
        shader_t *variant=buffer_variant(s,bias,texels,nt,reductions,nr);
        if(!variant) return 0;
        if(st) *second=variant;else *first=variant;
    }
    return 1;
}
static gw_t g_wb[MAX_BUFFER_BINDINGS]; static int g_nwb;   // writable buffers of this draw or dispatch, published only after command encoding
// A Vulkan 1.1 view inherits both usage bits of the general-purpose ring.
// Formats such as sRGB can be sampled but cannot be storage images on some
// GPUs. A second buffer handle bound to exactly the same memory and offset
// gives the view its legal usage while retaining live aliases and transfers.
static VkBuffer single_usage_buffer(VkBuffer backing, VkBufferUsageFlags usage) {
    VkDeviceMemory memory=C->ringm0;
    VkDeviceSize size=RING;
    VkBuffer *handles=C->typed0;
    if (backing!=C->ring0) {
        int i=0; while (i<C->nextra && C->extra[i].b!=backing) i++;
        if (i==C->nextra) return VK_NULL_HANDLE;
        memory=C->extra[i].m; size=C->extra[i].size; handles=C->extra[i].typed;
    }
    VkBuffer *slot=&handles[usage==VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT];
    if (*slot) return *slot;
    VkBufferCreateInfo create={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,NULL,0,size,usage,VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer alias;
    if (vkCreateBuffer(g_dev,&create,NULL,&alias)!=VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements allocated;
    vkGetBufferMemoryRequirements(g_dev,backing,&allocated);
    uint32_t type=mem_type(allocated.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryDedicatedRequirements dedicated={VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req={VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,&dedicated};
    VkBufferMemoryRequirementsInfo2 info={VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,NULL,alias};
    vkGetBufferMemoryRequirements2(g_dev,&info,&req);
    if (dedicated.requiresDedicatedAllocation || req.memoryRequirements.size>allocated.size ||
        !(req.memoryRequirements.memoryTypeBits & (1u<<type)) || vkBindBufferMemory(g_dev,alias,memory,0)!=VK_SUCCESS) {
        vkDestroyBuffer(g_dev,alias,NULL); return VK_NULL_HANDLE;
    }
    *slot=alias;
    return alias;
}
static int bind_texel(VkDescriptorSet ds, const sbind *b, int stage) {
    VkBufferView views[32];
    for (uint32_t element = 0; element < b->count; element++) {
        texel_range t; if (!texel_binding(b, stage, element, &t)) return 0;
        VkBuffer buffer = g_zero_buf; VkDeviceSize offset = 0, size = ZERO_BUFFER_BYTES; uint8_t *mapped = NULL;
        VkDeviceSize prefix=0;
        if (t.range.addr) {
            const storage_range *group=prepared_storage(t.range.addr,t.range.size,&buffer,&offset,&mapped);
            if (!group) return 0;
            size = t.range.size;
            VkDeviceSize align=texel_alignment(t.bytes);
            if (group->writable && group->conflict) {
                if (!texel_prefix(&t,offset,&prefix)) return 0;
            } else if (offset%align) {
                // Read-only snapshots can be aligned independently.
                if (group->writable) return 0;
                offset=copy_in(t.range.addr,size,align);
                if (offset==VK_WHOLE_SIZE) return 0;
                buffer=C->ring;
            }
        } else if (b->type==BT_STB) {
            // The Metal atomic sink exposes 16 texels. Other missing storage
            // images have one. Give every empty descriptor private zeroed
            // scratch so a store cannot corrupt another slot or a later draw.
            size=(b->texel_atomic ? 16u : 1u)*t.bytes;
            offset=ring_alloc(size,texel_alignment(t.bytes));
            if (offset==VK_WHOLE_SIZE) return 0;
            buffer=C->ring;
            memset(C->ringp+offset,0,size);
        } else t.format = b->num == 1 ? VK_FORMAT_R32G32B32A32_SINT : b->num == 2 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
        VkBuffer view_buffer=t.single_usage ? single_usage_buffer(buffer,t.single_usage) : buffer;
        if (!view_buffer) { RED_ONCE(21,"cannot share Vulkan ring memory with a single-usage typed view"); return 0; }
        VkBufferViewCreateInfo vi = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, view_buffer, t.format, offset-prefix, size+prefix};
        unsigned slot=element%32;
        VKCHECK(vkCreateBufferView(g_dev, &vi, NULL, &views[slot]));
        if (C->nbv == C->cbv) {
            if (C->cbv > SIZE_MAX / 2 / sizeof *C->bv) die("Vulkan: typed view tracking size overflow");
            size_t capacity = C->cbv ? C->cbv * 2 : 64;
            void *next = realloc(C->bv, capacity * sizeof *C->bv);
            if (!next) die("Vulkan: cannot track typed buffer views");
            C->bv = next; C->cbv = capacity;
        }
        C->bv[C->nbv++] = views[slot];
        if (t.range.addr && b->type == BT_STB && !b->read_only) {
            if (g_nwb == sizeof g_wb / sizeof *g_wb) die("Vulkan: too many writable buffer bindings");
            g_wb[g_nwb++] = (gw_t){.addr = t.range.addr, .size = size, .buf = buffer, .off = offset, .p = mapped};
        }
        if (slot==31 || element+1==b->count) {
            VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, b->binding,
                element-slot,slot+1,descriptor_type[b->type],NULL,NULL,views};
            vkUpdateDescriptorSets(g_dev,1,&write,0,NULL);
        }
    }
    return 1;
}
// The descriptor set is populated before the draw/dispatch. Only transfer ownership
// to the GPU after a command using it has actually been recorded.
static vtex *g_bound_images[6][32];
static int g_bound_image_writes[6];
static int mark_image_writes(int stage) {
    for (int i = 0; i < 32; ++i) {
        vtex *v = g_bound_images[stage][i];
        if (v) v->gpu = 1;
    }
    return g_bound_image_writes[stage];
}
static VkFormat storage_register_format(int bytes, int is_uint) {
    switch (bytes) {
        case 1: return is_uint ? VK_FORMAT_R8_UINT : VK_FORMAT_R8_SINT;
        case 2: return is_uint ? VK_FORMAT_R16_UINT : VK_FORMAT_R16_SINT;
        case 4: return is_uint ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SINT;
        case 8: return is_uint ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SINT;
        case 16: return is_uint ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}
// Keep write sinks separate from sampled zeros and readonly image zeros. A
// missing element in a descriptor array must not cancel its live neighbours.
static VkImageView storage_dummy(const sbind *b) {
    typedef struct empty_storage {
        VkFormat format;
        int dimension, read_only, multisample;
        VkImageView view;
        struct empty_storage *next;
    } empty_storage;
    static empty_storage *cache;
    int dimension = b->view < 0 ? VK_IMAGE_VIEW_TYPE_2D : b->view;
    if (dimension > VK_IMAGE_VIEW_TYPE_CUBE_ARRAY || (b->multisample && !g_storage_ms)) return VK_NULL_HANDLE;
    VkFormat format = b->num ? dummy_format(b->num, b->multisample) : VK_FORMAT_R16G16B16A16_SFLOAT;
    if (b->texel_atomic) format = storage_register_format(4, b->num == 2);
    if (b->storage_image > 1) format = spirv_texel_format(b->storage_image - 1).format;
    if (!format) return VK_NULL_HANDLE;
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(g_pd, format, &properties);
    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (b->texel_atomic) required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
    if ((properties.optimalTilingFeatures & required) != required) return VK_NULL_HANDLE;
    for (empty_storage *item = cache; item; item = item->next)
        if (item->format == format && item->dimension == dimension && item->read_only == b->read_only && item->multisample == b->multisample)
            return item->view;
    VkImageView view = empty_image(dimension, format, VK_IMAGE_USAGE_STORAGE_BIT, b->multisample);
    if (!view) return VK_NULL_HANDLE;
    empty_storage *item = malloc(sizeof *item);
    if (!item) die("Vulkan: cannot track an empty storage image");
    *item = (empty_storage){.format=format, .dimension=dimension, .read_only=b->read_only,
        .multisample=b->multisample, .view=view, .next=cache};
    cache = item;
    return view;
}
static int bind_storage(VkDescriptorSet ds, const sbind *b, int stage) {
    if (b->kind != K_IMG || b->reg < 0 || b->reg >= 32 || !b->count || b->count > 32u - b->reg) return 0;
    VkDescriptorImageInfo images[32];
    for (uint32_t i = 0; i < b->count; ++i) {
        int slot = b->reg + i, tid = (int)(S.img[stage][slot] & 0xfffff);
        treg_t tr; vtex *v = registered_texture(tid, &tr);
        if (!tr.ok) {
            VkImageView view = storage_dummy(b);
            if (!view) return 0;
            images[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            continue;
        }
        if (!v || !(v->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return 0;
        if (!compatible_dimension(v, b->view)) return 0;
        fmt_t f = view_format(v, &tr.v, tr.has_v);
        int bw, bh, bytes = nvn_fmt_bpb(tr.t.d.format, &bw, &bh);
        if (b->storage_image > 1) {
            // The image instruction specifies the texel layout. For example,
            // r32ui on an RGBA8 surface reads and writes one packed word.
            storage_texel_format shader = spirv_texel_format(b->storage_image-1);
            if (!shader.bytes || shader.bytes != bytes || bw != 1 || bh != 1 || v->f.depth) {
                RED_ONCE(119, "storage image format %u does not fit NVN format %#x", b->storage_image-1, tr.t.d.format);
                return 0;
            }
            f.f = shader.format;
        } else if (b->storage_image == 1 && b->num &&
                   clear_color_type(f.f) != (b->num == 2 ? CLEAR_UINT : CLEAR_SINT)) {
            // Formatless int/uint instructions use the same register-width
            // view as Metal img_fit when the descriptor is of another class.
            if (bw != 1 || bh != 1 || v->f.depth) return 0;
            f.f = storage_register_format(bytes, b->num == 2);
            if (!f.f) return 0;
        }
        if (b->texel_atomic) {
            VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, f.f, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT)) return 0;
        }
        VkImageView view = tex_view_format(v, &tr.t, &tr.v, tr.has_v, b->view, VIEW_STORAGE, f);
        if (!view) return 0;
        images[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        if (!b->read_only) g_bound_images[stage][slot] = v;
    }
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, b->binding, 0,
                              b->count, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, images, NULL, NULL};
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
    g_bound_image_writes[stage] |= !b->read_only;
    return 1;
}
static VkDescriptorSet allocate_shader_set(const shader_t *s) {
    VkDescriptorSet ds; VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, C->dp, 1, &s->dsl};
    VkResult result = vkAllocateDescriptorSets(g_dev, &ai, &ds);
    if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
        uint32_t need[VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT + 1] = {0};
        for (int i = 0; i < s->nb; i++) need[descriptor_type[s->b[i].type]] += s->b[i].count;
        if (C->nextra_dp == C->cextra_dp) {
            if (C->cextra_dp > SIZE_MAX / 2 / sizeof *C->extra_dp) die("Vulkan: descriptor pool tracking size overflow");
            size_t capacity = C->cextra_dp ? C->cextra_dp * 2 : 8;
            void *next = realloc(C->extra_dp, capacity * sizeof *C->extra_dp);
            if (!next) die("Vulkan: cannot track submission descriptor pools");
            C->extra_dp = next; C->cextra_dp = capacity;
        }
        C->dp = descriptor_pool_new(need);
        C->extra_dp[C->nextra_dp++] = C->dp;
        ai.descriptorPool = C->dp;
        result = vkAllocateDescriptorSets(g_dev, &ai, &ds);
    }
    VKCHECK(result);
    return ds;
}
// A shader may dynamically pair any element of its separate texture and
// sampler arrays. Combine the formats of the actual views written to its set.
static void include_border_view(border_view *common, border_view view) {
    switch (view.format) {
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16: case VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR:
            view.flags |= BORDER_PACKED; break; // Vulkan requires an explicit format for these borders.
        default: break;
    }
    if (!common->format) { *common = view; return; }
    enum clear_type first = border_number(*common), next = border_number(view);
    common->flags |= view.flags;
    if (common->format == view.format && first == next) return;
    common->flags |= BORDER_MIXED_FORMATS;
    if (first != next) common->flags |= BORDER_MIXED_NUMBERS;
    if ((first == CLEAR_FLOAT) != (next == CLEAR_FLOAT)) common->flags |= BORDER_FLOAT_AND_INT;
}

static int write_sampled_binding(VkDescriptorSet ds, const sbind *b, int stage, border_view *possible_views, int reduce_in_shader) {
    VkDescriptorImageInfo images[32] = {0};
    // The driver consumes descriptor writes immediately. Stream large
    // tables in bounded chunks; no scratch allocation scales with the layout.
    for (uint32_t first = 0; first < b->count; first += 32) {
        uint32_t count = b->count - first < 32 ? b->count - first : 32;
        for (uint32_t element = 0; element < count; ++element) {
            uint64_t handle = descriptor_handle(stage, b, first + element);
            border_view sampled = {0};
            if (b->type != BT_SAMP) {
                VkImageView view = tex_slot(handle, b, stage, &sampled);
                if (!view) return 0;
                if (possible_views) {
                    border_view effective = sampled;
                    if (!effective.format) effective.format = dummy_format(b->num, b->multisample);
                    include_border_view(possible_views,effective);
                }
                images[element].imageView = view; images[element].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
            if (b->type == BT_SAMP) sampled = *possible_views;
            if (b->type != BT_TEX) {
                pthread_mutex_lock(&g_rmu); images[element].sampler = samp_get((int)(handle >> 20),sampled,reduce_in_shader); pthread_mutex_unlock(&g_rmu);
            }
            if (b->type == BT_CIS && sampled.format == VK_FORMAT_UNDEFINED && b->num) {
                static VkSampler nearest;
                if (!nearest) { VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; VKCHECK(vkCreateSampler(g_dev, &si, NULL, &nearest)); }
                images[element].sampler = nearest;
            }
        }
        VkDescriptorType type = b->type == BT_CIS ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : b->type == BT_TEX ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        VkWriteDescriptorSet array = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, b->binding, first,
            count, type, images, NULL, NULL};
        vkUpdateDescriptorSets(g_dev, 1, &array, 0, NULL);
    }
    return 1;
}

typedef struct {
    VkDescriptorBufferInfo records, control, order, indirect;
    const dm_vk_feedback *layout;
    uint32_t source_records, capacity, primitive;
} capture_draw;
static struct {
    int phase;
    shader_t *vertex;
    capture_draw capture;
    VkDescriptorBufferInfo buffers[5], indices, arguments;
} G;
static int write_geometry_binding(VkDescriptorSet set, const sbind *binding) {
    if (!G.phase || binding->reg<0 || binding->reg>=5 || binding->count!=1) return 0;
    const VkDescriptorBufferInfo *buffer=&G.buffers[binding->reg];
    if (!buffer->buffer || !buffer->range) return 0;
    VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,NULL,set,binding->binding,0,1,
        descriptor_type[binding->type],NULL,buffer,NULL};
    if (is_texel(binding)) {
        VkBufferView view;
        VkBufferViewCreateInfo info={VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,NULL,0,buffer->buffer,
            VK_FORMAT_R32_UINT,buffer->offset,buffer->range};
        if (vkCreateBufferView(g_dev,&info,NULL,&view)!=VK_SUCCESS) return 0;
        if (C->nbv==C->cbv) {
            C->cbv=C->cbv ? 2*C->cbv : 64;
            C->bv=realloc(C->bv,C->cbv*sizeof *C->bv);
            if (!C->bv) die("Vulkan: geometry buffer view tracking allocation");
        }
        C->bv[C->nbv++]=view;
        write.pBufferInfo=NULL;write.pTexelBufferView=&view;
        vkUpdateDescriptorSets(g_dev,1,&write,0,NULL);
    } else vkUpdateDescriptorSets(g_dev,1,&write,0,NULL);
    return 1;
}
static VkDescriptorSet write_set(shader_t *s, int stage, const capture_draw *capture) {
    int has_separate = 0;
    // Every statically used descriptor must be populated before submitting this pipeline.
    for (int i = 0; i < s->nb; ++i) {
        const sbind *b = &s->b[i];
        if (b->type == BT_SAMP) has_separate = 1;
        if (b->type == BT_SIMG || b->table) continue;
        if (is_texel(b) && b->count <= 32) continue;
        if ((b->type == BT_CIS || b->type == BT_TEX) && b->count <= 32 && (b->kind == K_TEXU || b->kind == K_TEX || b->kind==K_TEX_SLOTS)) continue;
        if (b->type == BT_SAMP && b->count <= 32 && b->skind != S_NONE) continue;
        if (b->count != 1 || (b->type != BT_UBO && b->type != BT_SSBO && b->type != BT_CIS)) {
            RED_ONCE(22, "cannot populate descriptor %u type %d count %u (shader %s)", b->binding, b->type, b->count, s->key);
            return VK_NULL_HANDLE;
        }
    }
    memset(g_bound_images[stage], 0, sizeof g_bound_images[stage]);
    g_bound_image_writes[stage] = 0;
    VkDescriptorSet ds = allocate_shader_set(s);
    VkWriteDescriptorSet w[DM_VK_MAX_BINDINGS]; VkDescriptorBufferInfo bi[DM_VK_MAX_BINDINGS]; int nw = 0;
    int deferred_samplers[DM_VK_MAX_BINDINGS];
    int ndeferred = 0;
    border_view binding_views[DM_VK_MAX_BINDINGS];
    if (has_separate) memset(binding_views,0,s->nb * sizeof *binding_views);
    for (int i = 0; i < s->nb; i++) {
        const sbind *b = &s->b[i];
        if (b->type == BT_SAMP) { deferred_samplers[ndeferred++] = i; continue; }
        VkWriteDescriptorSet *x = &w[nw]; *x = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, b->binding, 0, 1};
        if (b->kind==K_GEOMETRY || b->kind==K_MESH) { if (!write_geometry_binding(ds,b)) return VK_NULL_HANDLE; continue; }
        if (is_texel(b)) { if (!bind_texel(ds, b, stage)) return VK_NULL_HANDLE; continue; }
        if (b->type == BT_SIMG) {
            if (!bind_storage(ds, b, stage)) {
                RED_ONCE(80, "invalid storage image for shader %s binding %u (guest image slot %d)", s->key, b->binding, b->reg);
                return VK_NULL_HANDLE;
            }
            continue;
        }
        if (b->type == BT_UBO || b->type == BT_SSBO) {
            VkDescriptorBufferInfo *d = &bi[nw]; *d = (VkDescriptorBufferInfo){g_zero_buf, 0, 65536};
            if (b->kind == K_CAPTURE_RECORDS || b->kind == K_CAPTURE_CONTROL) {
                if (!capture) return VK_NULL_HANDLE;
                *d = b->kind == K_CAPTURE_RECORDS ? capture->records : capture->control;
            } else if (b->kind==K_GMEM_MAP) {
                if (!global_memory_map(d)) return VK_NULL_HANDLE;
            } else if (b->kind==K_GMEM_LOG) {
                if (!global_memory_log(d)) return VK_NULL_HANDLE;
            } else if (b->kind == K_TABLE_MAP) {
                const handle_table *table = table_cache[stage][b->slot];
                VkDeviceSize bytes = (table->map[0]+2u) * sizeof(uint32_t);
                VkDeviceSize offset = ring_put(table->map, bytes, g_props.limits.minStorageBufferOffsetAlignment);
                if (offset == VK_WHOLE_SIZE) return VK_NULL_HANDLE;
                *d = (VkDescriptorBufferInfo){C->ring, offset, bytes};
            } else if (b->kind==K_POOL_BOUNDS) {
                uint32_t bounds[DM_VK_MAX_BINDINGS][4]={{0}};
                for (int j=0;j<s->nb;j++) if (s->b[j].pool) {
                    int slot=s->b[j].slot;
                    if (slot>=b->reg) return VK_NULL_HANDLE;
                    bounds[slot][0]=table_cache[stage][slot]->map[0];
                }
                VkDeviceSize size=(unsigned)b->reg*sizeof bounds[0];
                VkDeviceSize offset=ring_put(bounds,size,g_props.limits.minUniformBufferOffsetAlignment);
                if (offset==VK_WHOLE_SIZE) return VK_NULL_HANDLE;
                *d=(VkDescriptorBufferInfo){C->ring,offset,size};
            } else if (b->kind == K_CBUF && b->reg >= 0 && b->reg < 16) {
                const p_bind_buf *u = &S.ubo[stage][b->reg];
                uint64_t n = u->size > 65536 ? 65536 : u->size;
                VkBuffer gb; VkDeviceSize go;
                if (u->addr && n && gw_find(u->addr, n, 256, &gb, &go, NULL)) *d = (VkDescriptorBufferInfo){gb, go, n};
                else if (u->addr && n) { VkDeviceSize o = copy_in(u->addr, n, 256); if (o != VK_WHOLE_SIZE) *d = (VkDescriptorBufferInfo){C->ring, o, n}; }
            } else if (b->kind == K_C1 && s->c1_len) { VkDeviceSize o = ring_put(s->c1, s->c1_len, 256); if (o != VK_WHOLE_SIZE) *d = (VkDescriptorBufferInfo){C->ring, o, s->c1_len}; }
            else if (b->kind == K_SUPPORT) *d = (VkDescriptorBufferInfo){g_support_buf, 0, 4096};
            else if ((b->kind == K_SBUF || b->kind == K_UBOP || b->kind == K_SBUFU) && b->reg >= 0 && b->reg < 16) {   // preserve prior GPU writes, then publish writable bindings after the command
                p_bind_buf range = storage_binding(b, stage);
                const p_bind_buf *u = &range;
                VkBuffer gb = VK_NULL_HANDLE; VkDeviceSize go = 0; uint8_t *gp = NULL;
                if (u->addr && u->size && (prepared_storage(u->addr, u->size, &gb, &go, &gp) || gw_find(u->addr, u->size, 256, &gb, &go, &gp))) *d = (VkDescriptorBufferInfo){gb, go, u->size};
                else if (u->addr && u->size) { VkDeviceSize o = copy_in(u->addr, u->size, 256); if (o != VK_WHOLE_SIZE) { *d = (VkDescriptorBufferInfo){C->ring, o, u->size}; gb = C->ring; go = o; gp = C->ringp + o; } }
                if (gb && b->type == BT_SSBO && !b->read_only) {
                    if (g_nwb == sizeof g_wb / sizeof *g_wb) die("Vulkan: too many writable buffer bindings");
                    g_wb[g_nwb++] = (gw_t){u->addr, u->size, gb, go, gp, 0};
                }
            } else if (b->kind == K_DRVCB && b->off == DMSH_PUSH_BANK) {
                VkDeviceSize align = b->type == BT_UBO ? g_props.limits.minUniformBufferOffsetAlignment : g_props.limits.minStorageBufferOffsetAlignment;
                VkDeviceSize offset = ring_put(S.push[stage == 5], DMSH_PUSH_SIZE, align);
                if (offset == VK_WHOLE_SIZE) return VK_NULL_HANDLE;
                *d = (VkDescriptorBufferInfo){C->ring, offset, DMSH_PUSH_SIZE};
            } else if (b->kind == K_DRVCB) {   // driver segments: c2 holds the bound texture / sampler handles (as nvn_mtl.m fills them), c0 the storage buffer table
                uint8_t seg[4096];
                int nvn2 = stage == 5 && dm_sdk_ounce();
                if (b->off == 2) {
                    dm_vk_c2_fill(seg, sizeof seg, S.tex[stage], S.img[stage], S.samp[stage]);
                    if (s->logical_slots) dm_vk_c2_logical(seg,sizeof seg,S.tex[stage],S.img[stage],S.samp[stage]);
                } else {
                    // Replay currently rasterizes at one sample per pixel.
                    // Multisample replay must supply its actual sample positions.
                    const uint8_t center = 0x88;
                    if (!dm_vk_c0_fill(seg, sizeof seg, stage, S.ssbo[stage], S.ubo[stage],
                                       g_dispatch_dim, g_draw_base_instance, &center, 1, nvn2)) return VK_NULL_HANDLE;
                }
                VkDeviceSize o = ring_put(seg, sizeof seg, 256);
                if (o != VK_WHOLE_SIZE) {
                    *d = (VkDescriptorBufferInfo){C->ring, o, sizeof seg};
                    if (stage == 5 && b->off == 0 && g_dispatch_args) {
                        // Use the same resolved GPU/CPU argument bytes as the
                        // indirect command, without reading unfinished GPU writes.
                        VkBufferCopy copies[] = {{g_dispatch_offset, o + DMSH_C0_DISPATCH, sizeof g_dispatch_dim},
                                                 {g_dispatch_offset, o + DMSH_C0_DISPATCH_NVN2, sizeof g_dispatch_dim}};
                        vkCmdCopyBuffer(C->cb, g_dispatch_args, C->ring, nvn2 ? 2 : 1, copies);
                        barrier_all(C->cb);
                    }
                }
            }
            d->offset -= s->buffer_bias[i]; d->range += s->buffer_bias[i];
            if (b->type == BT_SSBO && d->range > g_props.limits.maxStorageBufferRange) { RED_ONCE(1, "biased storage range exceeds the device descriptor limit"); return VK_NULL_HANDLE; }
            x->descriptorType = b->type == BT_UBO ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; x->pBufferInfo = d;
        } else if (b->type == BT_TEX || b->type == BT_CIS) {
            if (!write_sampled_binding(ds,b,stage,has_separate ? &binding_views[i] : NULL,s->nreductions != 0)) return VK_NULL_HANDLE;
            continue;
        }
        nw++;
    }
    for (int i = 0; i < ndeferred; ++i) {
        int slot = deferred_samplers[i];
        const sampler_pairs *pairs = s->sampler_images ? &s->sampler_images[slot] : NULL;
        border_view possible = {0};
        for (int j = 0; j < s->nb; ++j) {
            if (!binding_views[j].format || (pairs && !(pairs->mask[j/64] & (UINT64_C(1)<<(j%64))))) continue;
            include_border_view(&possible,binding_views[j]);
        }
        if (!write_sampled_binding(ds,&s->b[slot],stage,&possible,s->nreductions != 0)) return VK_NULL_HANDLE;
    }
    vkUpdateDescriptorSets(g_dev, (uint32_t)nw, w, 0, NULL);
    return ds;
}
static _Atomic uint64_t g_wait_ns, g_rec_ns, g_pwait_ns;   // time the submit thread waited for the GPU, spent recording, and the present waited for its slot
static uint64_t g_stat_draws, g_stat_skip, g_skip_why[8];   // why: 0 no program, 1 shader missing / rejected, 2 no render target, 3 indirect, 4 primitive, 5 pipeline, 6 descriptors
#define SKIP(w) do { g_stat_skip++; g_skip_why[w]++; return; } while (0)
static int draw_begin(uint32_t write_mask) {
    if (!pass_begin()) return 0;
    if (S.rasterizer_discard) return 1;
    if (S.visibility) {
        const uint64_t *samples = visibility_begin(&C->visibility, C->cb, C->up, g_precise_visibility);
        if (samples) post_add((post_t){.kind=POST_VISIBILITY_ADD,.visibility=visibility_retain(S.visibility),.samples=samples});
    }
    for (int a = 0; a < P.n; a++) if (write_mask >> (4 * a) & 0xf) P.cwr[a] = P.cdrawn[a] = 1;
    return 1;
}
typedef struct {
    uint32_t slot, stride, divisor;
    VkBuffer buffer;
    VkDeviceSize start, size;
} instance_stream;

// With no native divisor, bind a constant element for each run over which
// all divided streams stay unchanged. The underlying buffer may be GPU-owned.
static void emit_instances(VkCommandBuffer cb, int indexed, uint32_t count, int32_t first,
                           uint32_t base, uint32_t instances, const instance_stream *streams, unsigned n) {
    for (uint32_t done=0;done<instances;) {
        uint32_t run=instances-done;
        for (unsigned i=0;i<n;i++) {
            const instance_stream *s=&streams[i];
            uint32_t until=s->divisor-done%s->divisor;
            if (until<run) run=until;
            VkDeviceSize delta=(VkDeviceSize)(done/s->divisor)*s->stride;
            VkBuffer buffer=delta<s->size ? s->buffer : g_zero_buf;
            VkDeviceSize offset=delta<s->size ? s->start+delta : 0;
            vkCmdBindVertexBuffers(cb,s->slot,1,&buffer,&offset);
        }
        if (indexed) vkCmdDrawIndexed(cb,count,run,0,first,base+done);
        else vkCmdDraw(cb,count,run,(uint32_t)first,base+done);
        done+=run;
    }
}
static VkRect2D clipped_rect(p_scissor sc, uint32_t width, uint32_t height) {
    int64_t right = (int64_t)sc.x + sc.w, bottom = (int64_t)sc.y + sc.h;
    int32_t x = sc.x > 0 ? sc.x : 0, y = sc.y > 0 ? sc.y : 0;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    if (right <= x || bottom <= y) return (VkRect2D){0};
    return (VkRect2D){{x, y}, {(uint32_t)(right - x), (uint32_t)(bottom - y)}};
}
static VkRect2D render_scissor(void) {
    return S.has_sc ? clipped_rect(S.sc, P.w, P.h) : (VkRect2D){{0, 0}, {P.w, P.h}};
}

#include "primitive_replay.h"
// Like Metal's unorm_chan_masks, operate on the stored, unsigned bits. Vulkan
// encodes the sixteen truth tables in the same order as NVN.
static int logic_format(VkFormat f) {
    if (clear_color_type(f)==CLEAR_UINT) return 1;
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return 1;
        default: return 0;
    }
}
static void color_operations(pso_key *k) {
    int eligible=0, other=0;
    for (int i=0; i<k->nc; ++i) {
        unsigned bit=1u<<i;
        if (!k->fs || !(k->fs->outputs.targets & bit)) {
            k->chmask.mask &= ~(15u<<(4*i));
            k->color.blend_en &= ~bit;
            continue;
        }
        if (clear_color_type(k->cf[i])!=CLEAR_FLOAT) k->color.blend_en &= ~(1u<<i);
        if (!k->cf[i] || !(k->chmask.mask>>(4*i)&15)) continue;
        if (logic_format(k->cf[i])) eligible=1; else other=1;
    }
    if (!k->fs || !eligible || k->color.logic_op==NVN_LOGIC_OP_COPY) return;
    if (other || k->color.alpha_func) {
        RED_ONCE(115,"logic operation on mixed formats or with alpha testing uses COPY"); return;
    }
    k->color.blend_en=0; // NVN logic replaces every blend equation.
    k->logic=g_logic_op;
    if (!k->logic) RED_ONCE(115,"device lacks logicOp; drawing color with COPY");
}

static void feedback_hold(feedback_control *control) {
    if (control->last_submission != g_fence + 1) {
        control->last_submission = g_fence + 1;
        atomic_fetch_add(&control->refs, 1);
        post_add((post_t){.kind=POST_FEEDBACK_RELEASE, .dst=control});
    }
}
#include "transform_feedback.inc"
static int compute_pipeline(shader_t *cs);
static void geometry_draw(shader_t *vertex, const p_draw *draw, const p_draw_idx *indexed,
                          uint32_t draw_index, uint32_t primitive_base);

static void draw_direct(const p_draw *dw, const p_draw_idx *di, uint32_t draw_index) {
    g_nwb = 0;
    if (g_pass_shader_writes) pass_end(); // order prior vertex/fragment storage writes before the next command
    if (!G.phase && !S.bst[0]) SKIP(0);
    shader_t *vs = NULL, *fs = NULL;
    for (int st = 0; st < (S.rasterizer_discard ? 1 : 2); st++) {   // the vertex shader of the program bound to the vertex stage, the fragment shader of the one bound to the fragment stage (cached by program and serial)
        const prog_t *p = S.bst[st]; if (!p) continue;
        static struct { const prog_t *p; uint64_t ser; shader_t *sh[2]; } pc[1024];
        unsigned h = (unsigned)(((uintptr_t)p >> 4) ^ p->serial) % 1024;
        if (pc[h].p != p || pc[h].ser != p->serial) {
            pc[h].p = p; pc[h].ser = p->serial; pc[h].sh[0] = pc[h].sh[1] = NULL;
            for (unsigned i = 0; i < p->nstage; i++) { uint8_t md[8]; if (!prog_key(p, i, md)) continue; shader_t *s = shader_get(md); if (s->stage == 0 || s->stage == 1) pc[h].sh[s->stage] = s; }
        }
        if (st) fs = pc[h].sh[1]; else vs = pc[h].sh[0];
    }
    if (G.phase) { vs=G.vertex; if (G.phase==1) fs=NULL; }
    if (vk_skip() & SK_DRAW) SKIP(4);
    if (!G.phase && S.bst[2]) { geometry_draw(vs,dw,di,draw_index,0); return; }
    if (!G.phase && (S.bst[3] || S.bst[4])) { RED_ONCE(32,"tessellation replay remains unavailable"); SKIP(4); }
    if (!vs || !vs->mod || (fs && !fs->mod)) SKIP(1);
    uint32_t layers=vs->layer_output && G.phase!=1 ? P.max_layers : 1;
    if (P.layers!=layers) { pass_end(); P.layers=layers; }
    if (di && di->count) cpu_ready(di->ibuf, (uint64_t)(uint32_t)di->count * (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4));
    shader_cpu_ready(vs, 0); shader_cpu_ready(fs, 1);
    capture_draw capture = G.phase==1 ? G.capture : (capture_draw){0};
    if (!G.phase && S.feedback_active && vs->feedback_layout)
        vs = feedback_vertex(vs, dw, &capture);
    if (!pass_begin()) SKIP(2);
    int prim = dw ? dw->prim : di->prim;
    if (prim == NVN_DRAW_PRIMITIVE_LINE_LOOP || prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP || prim == NVN_DRAW_PRIMITIVE_PATCHES) { RED_ONCE(31, "primitive %d not implemented yet", prim); SKIP(4); }
    int restart = di && S.restart.on;
    if (restart && prim > NVN_DRAW_PRIMITIVE_QUADS) { RED_ONCE(31,"primitive restart on topology %d requires adjacency/patch replay",prim); SKIP(4); }
    if (!prepare_storage(&vs, &fs)) SKIP(6);
    pso_key k; memset(&k, 0, sizeof k);
    k.rasterizer_discard = S.rasterizer_discard;
    k.depth_clamp = S.depth_clamp && g_depth_clamp;
    if (S.depth_clamp && !g_depth_clamp) RED_ONCE(109, "depth clamp unavailable on this device; using depth clipping");
    k.origin_ll = g_queue_origin_ll;
    k.vs = vs; k.fs = fs; k.prim = replay_topology(prim,restart); k.nattr = S.nattr; memcpy(k.attr, S.attr, sizeof k.attr); k.nstream = S.nstream; memcpy(k.stream, S.stream, sizeof k.stream);
    for (int i=0;i<k.nattr;i++) if ((vs->vin_mask >> i & 1) && k.attr[i].stream<(unsigned)k.nstream) {
        vs_st stream=k.stream[k.attr[i].stream];
        if (stream.stride && stream.divisor>g_vertex_divisor_limit) k.split_instances=1;
    }
    k.nc = P.n; memcpy(k.cf, P.cf, sizeof k.cf); memcpy(k.blend, S.blend, P.n * sizeof *k.blend);
    k.color = S.color; k.chmask = S.chmask; k.poly = S.poly; k.samples = P.samples;
    k.alpha_to_coverage=S.ms.a2c && fs && fs->outputs.alpha_var && clear_color_type(P.cf[0])==CLEAR_FLOAT;
    // Like the Metal side entry, all compare functions share one pipeline.
    // Integer color 0 bypasses the test; a depth-only pass may still discard.
    k.color.alpha_func = fs && S.color.alpha_func && S.color.alpha_func!=NVN_ALPHA_FUNC_ALWAYS &&
        clear_color_type(P.cf[0])==CLEAR_FLOAT && fs->outputs.alpha_var;
    if (fs) {
        uint8_t uint_mask=0, sint_mask=0;
        for (int i=0;i<k.nc;++i) if (fs->outputs.targets & (1u<<i)) {
            enum clear_type target=clear_color_type(k.cf[i]);
            if (target==CLEAR_UINT) uint_mask|=1u<<i;
            if (target==CLEAR_SINT) sint_mask|=1u<<i;
        }
        uint_mask &= ~fs->outputs.uint_targets; sint_mask &= ~fs->outputs.sint_targets;
        k.fragment_module=fragment_module(fs,uint_mask,sint_mask,k.color.alpha_func);
        if (!k.fragment_module && k.color.alpha_func) {
            k.color.alpha_func=0; k.fragment_module=fragment_module(fs,uint_mask,sint_mask,0);
        }
        if (!k.fragment_module) SKIP(1);
    }
    color_operations(&k);
    if (P.dt) { k.df = P.dt->f.f; k.stencil = P.dt->f.stencil; k.ds = S.ds; }
    if (g_plog) LOG("plog:  draw vs %s fs %s n %u depth t%d w%d f%d stencil %d f%d ops %d/%d/%d ref %u vm %#x wm %#x blend %#x mask %#x cull %d", vs->key, fs ? fs->key : "-", dw ? dw->count : di->count,
                    S.ds.dt, S.ds.dw, S.ds.df, S.ds.st, S.ds.sf_f, S.ds.fail_f, S.ds.zfail_f, S.ds.zpass_f, S.st_ref[0], S.st_vmask[0], S.st_wmask[0], S.color.blend_en, S.chmask.mask, S.poly.cull);
    VkPipelineLayout pl = layout_get(vs, fs);
    if (!pl) SKIP(6);
    VkPipeline pp = pso_get(&k, pl);
    if (!pp) SKIP(5);
    if (fs && P.n) fb_prepare(fs); else g_fb_mask = 0;
    g_draw_base_instance = (uint32_t)(dw ? dw->base_instance : di->base_instance);
    VkDescriptorSet sets[2] = {write_set(vs, 0, &capture), fs ? write_set(fs, 1, NULL) : VK_NULL_HANDLE};
    g_fb_mask = 0;
    if (!sets[0] || (fs && !sets[1])) SKIP(6);
    VkCommandBuffer cb = C->cb;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp);
    if (k.color.alpha_func) {
        static const uint32_t accept[]={15,0,1,2,3,4,13,6,15};
        struct { uint32_t mask; float reference; } alpha={
            S.color.alpha_func<sizeof accept/sizeof *accept ? accept[S.color.alpha_func] : 15,S.alpha_ref};
        vkCmdPushConstants(cb,pl,VK_SHADER_STAGE_FRAGMENT_BIT,DM_VK_ALPHA_PUSH_OFFSET,sizeof alpha,&alpha);
    }
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, fs ? 2 : 1, sets, 0, NULL);
    // As in Metal, the device origin changes clip Y inside the viewport.
    // Viewport/scissor coordinates always address the same image memory rows.
    // Vulkan's positive height implements NVN lower-left; upper-left needs a flip.
    float x = 0, y = 0, w = (float)P.w, h = (float)P.h;
    if (S.has_vp) { x = S.vp[0][0]; y = S.vp[0][1]; w = S.vp[0][2]; h = S.vp[0][3]; }
    const uint8_t *sw = S.vp_sw[0];
    int flip_y = !sw[0] && sw[1] == 1 && !sw[2] && !sw[3];
    if (!flip_y && (sw[0] || sw[1] || sw[2] || sw[3]))
        RED_ONCE(108, "viewport swizzle %u,%u,%u,%u not implemented (drawn unswizzled)", sw[0], sw[1] ^ 2, sw[2] ^ 4, sw[3] ^ 6);
    // Mirroring the viewport about its centre also reverses facing; leave
    // the queue's winding and the scissor unchanged, as on Metal.
    int positive_height = g_origin_ll ^ flip_y;
    VkViewport vv = {x, positive_height ? y : y + h, w, positive_height ? h : -h, S.dr[0], S.dr[1]};
    vkCmdSetViewport(cb, 0, 1, &vv);
    VkRect2D sc = render_scissor();
    vkCmdSetScissor(cb, 0, 1, &sc);
    p_poly_offset bias = (S.poly.offset_en & NVN_POLYGON_OFFSET_ENABLE_FILL) ? S.po : (p_poly_offset){0};
    if (bias.clamp && !g_depth_bias_clamp) {
        RED_ONCE(109, "polygon depth bias clamp unavailable; using the unclamped bias");
        bias.clamp = 0; // vkCmdSetDepthBias requires zero unless depthBiasClamp is enabled.
    }
    vkCmdSetDepthBias(cb, bias.units, bias.clamp, bias.factor);
    vkCmdSetBlendConstants(cb, S.blend_color);
    for (int f = 0; f < 2; f++) {
        VkStencilFaceFlags ff = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilCompareMask(cb, ff, S.st_vmask[f]); vkCmdSetStencilWriteMask(cb, ff, S.st_wmask[f]); vkCmdSetStencilReference(cb, ff, S.st_ref[f]);
    }
    // vertex streams: only the vertices this draw reads are copied (games bind whole vertex pools, tens of MB). Per-vertex streams are copied from vertex vlo on and the draw's
    // vertex fetch indices shifted down by vlo (the vertex shader restores guest IDs); per-instance streams from instance 0 up to the last one drawn
    uint32_t inst = dw ? (dw->instances ? (uint32_t)dw->instances : 1) : (di->instances ? (uint32_t)di->instances : 1);
    uint32_t isz = di ? (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4) : 0;
    const uint8_t *isrc = di ? (const uint8_t *)(uintptr_t)di->ibuf : NULL;
    int64_t vlo = 0, vhi = -1;
    if (dw) { vlo = (uint32_t)dw->first; vhi = vlo + (uint32_t)dw->count - 1; }
    else if (di->count) {
        uint32_t lo = UINT32_MAX, hi = 0;
        for (uint32_t i = 0; i < (uint32_t)di->count; i++) { uint32_t v = source_index(isrc,isz,i); if (restart && v == S.restart.index) continue; if (v < lo) lo = v; if (v > hi) hi = v; }
        if (lo <= hi) { vlo = (int64_t)lo + di->base_vertex; vhi = (int64_t)hi + di->base_vertex; }
    }
    if (vhi < vlo) { g_stat_draws++; C->raster_draws++; return; }
    if (vlo < 0) vlo = 0;
    if (vs->draw_push) {
        // The guest sees its original indices even though Vulkan fetches from
        // a compact buffer starting at vlo. InstanceIndex remains native.
        int32_t values[] = {dw ? dw->first : di->base_vertex, (int32_t)g_draw_base_instance, (int32_t)draw_index, (int32_t)vlo};
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof values, values);
    }
    uint32_t used = 0, ext[16] = {0};
    for (int i = 0; i < S.nattr; i++) if (vs->vin_mask >> i & 1) { uint32_t st = S.attr[i].stream, e = S.attr[i].offset + 16; used |= 1u << st; if (e > ext[st]) ext[st] = e; }
    instance_stream split[16]; unsigned nsplit=0;
    for (int s = 0; s < 16; s++) if (used >> s & 1) {
        VkDeviceSize o = 0; VkBuffer b = g_zero_buf;
        uint64_t stride = s < S.nstream ? S.stream[s].stride : 0, size = S.vbuf[s].size, from, to;
        uint32_t divisor=s<S.nstream ? S.stream[s].divisor : 0;
        int emulate=k.split_instances && stride && divisor>1;
        if (divisor) {
            uint64_t last=(uint64_t)g_draw_base_instance+(inst-1)/divisor;
            from=emulate ? (uint64_t)g_draw_base_instance*stride : 0;
            to=stride && last>(UINT64_MAX-ext[s])/stride ? size : last*stride+ext[s];
        }
        else { from = (uint64_t)vlo * stride; to = (uint64_t)vhi * stride + ext[s]; }
        if (to > size) to = size;
        if (S.vbuf[s].address && to > from && gw_find(S.vbuf[s].address + from, to - from, 1, &b, &o, NULL)) {}   // vertices a dispatch wrote (GPU particles)
        else if (S.vbuf[s].address && to > from) { VkDeviceSize ro = copy_in(S.vbuf[s].address + from, to - from, 16); if (ro != VK_WHOLE_SIZE) { b = C->ring; o = ro; atomic_fetch_add(&g_vbuf_copies, 1); atomic_fetch_add(&g_vbuf_copy_bytes, to - from); } }
        if (emulate) split[nsplit++]=(instance_stream){(uint32_t)s,(uint32_t)stride,divisor,b,o,b!=g_zero_buf && to>from ? to-from : 0};
        else vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &b, &o);
    }
    int convert = restart || prim==NVN_DRAW_PRIMITIVE_QUADS || prim==NVN_DRAW_PRIMITIVE_TRIANGLE_FAN;
    int indexed = di || convert;
    uint32_t count=(uint32_t)(dw ? dw->count : di->count);
    int32_t first=(int32_t)((dw ? dw->first : di->base_vertex)-vlo);
    if (G.phase==2) {
        vkCmdBindIndexBuffer(cb,G.indices.buffer,G.indices.offset,VK_INDEX_TYPE_UINT32);
    } else if (convert || (isz==1 && !g_has_u8)) {
        int converted_prim=convert ? prim : NVN_DRAW_PRIMITIVE_POINTS;
        uint64_t needed=replay_indices(NULL,isrc,isz,count,converted_prim,restart,S.restart.index);
        if (!needed) return;
        if (needed>UINT32_MAX) { RED_ONCE(31,"expanded primitive exceeds the Vulkan draw count"); return; }
        VkDeviceSize offset=ring_alloc(needed*4,16);
        if (offset==VK_WHOLE_SIZE) return;
        replay_indices((uint32_t *)(C->ringp+offset),isrc,isz,count,converted_prim,restart,S.restart.index);
        vkCmdBindIndexBuffer(cb,C->ring,offset,VK_INDEX_TYPE_UINT32);
        count=(uint32_t)needed;
    } else if (di) {
        VkDeviceSize offset=copy_in(di->ibuf,(uint64_t)count*isz,16);
        if (offset==VK_WHOLE_SIZE) return;
        vkCmdBindIndexBuffer(cb,C->ring,offset,isz==1 ? VK_INDEX_TYPE_UINT8_EXT : isz==2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }
    if (!draw_begin(k.chmask.mask)) SKIP(2);
    if (G.phase==2) vkCmdDrawIndexedIndirect(cb,G.arguments.buffer,G.arguments.offset,1,sizeof(VkDrawIndexedIndirectCommand));
    else emit_instances(cb,indexed,count,first,g_draw_base_instance,inst,split,nsplit);
    for (int i = 0; i < g_nwb; i++) {
        gw_t *w = &g_wb[i];
        buffer_written(w->addr, w->size, w->buf, w->off, w->p);
    }
    g_pass_shader_writes |= g_nwb != 0;
    g_pass_shader_writes |= global_memory_written();
    g_pass_shader_writes |= mark_image_writes(0);
    if (fs) g_pass_shader_writes |= mark_image_writes(1);
    if (capture.layout) feedback_finish(&capture);
    g_stat_draws++; C->raster_draws++;
}
#include "geometry_replay.inc"
// CPU range staging and primitive conversion need the actual vertex/index
// bounds. Resolve only the bytes those operations consume, in submission order.
static int read_draw_args(uint64_t address, void *out, size_t size) {
    if (!address || address > UINT64_MAX-size || !mtl_addr(address,size,NULL,NULL)) {
        RED_ONCE(30,"indirect draw arguments are outside a registered buffer"); return 0;
    }
    cpu_ready(address,size);
    memcpy(out,(const void *)(uintptr_t)address,size);
    return 1;
}
#include "mesh_replay.inc"
static void draw(const p_draw *dw, const p_draw_idx *di) {
    if (dw && dw->prim == DM_PRIM_MESH_TASKS) { mesh_draw(dw); return; }
    uint64_t args = dw ? dw->ind : di->ind;
    uint64_t counter = dw ? dw->mdi_cnt : di->mdi_cnt;
    uint32_t index = (uint32_t)(dw ? dw->mdi_i : di->mdi_i);
    if (counter) {
        uint32_t live;
        if (!read_draw_args(counter,&live,sizeof live) || index >= live) return;
    }
    if (!args) { draw_direct(dw,di,index); return; }
    if (dw) {
        VkDrawIndirectCommand a;
        if (!read_draw_args(args,&a,sizeof a) || !a.vertexCount || !a.instanceCount) return;
        p_draw direct={.prim=dw->prim,.first=(int32_t)a.firstVertex,.count=(int32_t)a.vertexCount,
            .instances=(int32_t)a.instanceCount,.base_instance=(int32_t)a.firstInstance};
        draw_direct(&direct,NULL,index);
    } else {
        VkDrawIndexedIndirectCommand a;
        if (!read_draw_args(args,&a,sizeof a) || !a.indexCount || !a.instanceCount) return;
        if (di->itype < 0 || di->itype > 2) return;
        uint64_t offset = (uint64_t)a.firstIndex << di->itype;
        uint64_t bytes = (uint64_t)a.indexCount << di->itype;
        if (!di->ibuf || offset > UINT64_MAX-di->ibuf || !mtl_addr(di->ibuf+offset,bytes,NULL,NULL)) {
            RED_ONCE(30,"indirect index range is outside a registered buffer"); return;
        }
        p_draw_idx direct={.prim=di->prim,.itype=di->itype,.ibuf=di->ibuf+offset,
            .count=(int32_t)a.indexCount,.instances=(int32_t)a.instanceCount,
            .base_vertex=a.vertexOffset,.base_instance=(int32_t)a.firstInstance};
        draw_direct(NULL,&direct,index);
    }
}
static void draw_multi(const p_multi_draw *m) {
    uint32_t live;
    if (m->max <= 0 || !m->ind || !read_draw_args(m->cnt,&live,sizeof live)) return;
    if (live > (uint32_t)m->max) live=(uint32_t)m->max;
    uint64_t args=m->ind;
    for (uint32_t i=0; i<live; i++) {
        if (m->itype < 0) {
            p_draw item={.prim=m->prim,.ind=args,.mdi_i=(int32_t)i};
            draw(&item,NULL);
        } else {
            p_draw_idx item={.prim=m->prim,.itype=m->itype,.ibuf=m->ibuf,.ind=args,.mdi_i=(int32_t)i};
            draw(NULL,&item);
        }
        uint64_t next=args+(uint64_t)m->stride;
        if ((m->stride >= 0 && next < args) || (m->stride < 0 && next > args)) break;
        args=next;
    }
}
// Resolve the 12 argument bytes at execution time. A shader may supply just X,
// with Y and Z initialized by the CPU. A current submission's ring is live
// through its fence. Older rings must finish copying back before taking CPU bytes.
static VkDeviceSize prepare_dispatch_indirect(uint64_t addr, VkBuffer *buffer) {
    enum { BYTES = sizeof(VkDispatchIndirectCommand) };
    struct { VkBuffer buf; VkDeviceSize off; } source[BYTES] = {0};
    uint64_t wait = 0;
    pthread_mutex_lock(&g_gwmu);
    for (size_t i = 0; i < g_ngw; ++i) {
        const gw_t *w = &g_gw[i];
        if (addr >= w->addr + w->size || w->addr >= addr + BYTES) continue;
        if (w->fence != g_fence + 1) {
            if (w->fence > wait) wait = w->fence;
            continue;
        }
        for (int j = 0; j < BYTES; ++j) if (addr + j >= w->addr && addr + j - w->addr < w->size) {
            source[j].buf = w->buf;
            source[j].off = w->off + (addr + j - w->addr);
        }
    }
    pthread_mutex_unlock(&g_gwmu);
    if (wait) mtl_wait(wait, UINT64_MAX);
    VkDeviceSize dst = ring_alloc(BYTES, 4);
    if (dst == VK_WHOLE_SIZE) return dst;
    int copied = 0;
    for (int first = 0; first < BYTES;) {
        VkBuffer src = source[first].buf;
        int end = first + 1;
        while (end < BYTES && source[end].buf == src &&
               (!src || source[end].off == source[first].off + (end - first))) ++end;
        if (src) {
            VkBufferCopy region = {source[first].off, dst + first, (VkDeviceSize)(end - first)};
            vkCmdCopyBuffer(C->cb, src, C->ring, 1, &region);
            copied = 1;
        } else {
            memcpy(C->ringp + dst + first, (const void *)(uintptr_t)(addr + first), (size_t)(end - first));
        }
        first = end;
    }
    if (copied) barrier_all(C->cb);
    *buffer = C->ring;
    return dst;
}
static int compute_pipeline(shader_t *cs) {
    if (!cs->cpp) {
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &cs->dsl, 0, NULL}; VKCHECK(vkCreatePipelineLayout(g_dev, &li, NULL, &cs->cpl));
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs->mod, "main", NULL}, cs->cpl, VK_NULL_HANDLE, -1};
        if (cs->subgroup.requiredSubgroupSize) ci.stage.pNext = &cs->subgroup;
        if (vkCreateComputePipelines(g_dev, g_pcache, 1, &ci, NULL, &cs->cpp) != VK_SUCCESS) { LOG("[RED] vk: compute pipeline for %s not created", cs->key); cs->mod = VK_NULL_HANDLE; return 0; }
    }
    return 1;
}
// Compute: the bound compute program's shader, its own pipeline and layout (one set), recorded outside render passes; afterwards every storage buffer it had is
// GPU-written: written back to guest memory at completion and read from the ring by the work recorded until then (gw_add)
static void dispatch(const p_dispatch *d) {
    if (vk_skip() & SK_DRAW) return;
    const prog_t *p = S.cprog; if (!p) return;
    if (!d->ind && (d->x <= 0 || d->y <= 0 || d->z <= 0)) return;
    static struct { const prog_t *p; uint64_t ser; shader_t *cs; } pc[256];
    unsigned h = (unsigned)(((uintptr_t)p >> 4) ^ p->serial) % 256;
    if (pc[h].p != p || pc[h].ser != p->serial) {
        pc[h].p = p; pc[h].ser = p->serial; pc[h].cs = NULL;
        for (unsigned i = 0; i < p->nstage; i++) { uint8_t md[8]; if (!prog_key(p, i, md)) continue; shader_t *sh = shader_get(md); if (sh->guest_stage == 5) pc[h].cs = sh; }
    }
    shader_t *cs = pc[h].cs; if (!cs || !cs->mod) { RED_ONCE(64, "compute program without a usable compute shader: dispatch skipped"); return; }
    pass_end();
    shader_cpu_ready(cs, 5);
    if (!prepare_storage(&cs, NULL)) return;
    if (!compute_pipeline(cs)) return;
    pass_end();
    g_dispatch_args = VK_NULL_HANDLE;
    g_dispatch_dim[0] = (uint32_t)d->x; g_dispatch_dim[1] = (uint32_t)d->y; g_dispatch_dim[2] = (uint32_t)d->z;
    if (d->ind) {
        g_dispatch_offset = prepare_dispatch_indirect(d->ind, &g_dispatch_args);
        if (g_dispatch_offset == VK_WHOLE_SIZE) return;
    }
    g_nwb = 0;
    VkDescriptorSet ds = write_set(cs, 5, NULL); if (!ds) return;
    if (g_plog) LOG("plog:  dispatch cs %s %dx%dx%d, %d storage buffers", cs->key, d->x, d->y, d->z, g_nwb);
    vkCmdBindPipeline(C->cb, VK_PIPELINE_BIND_POINT_COMPUTE, cs->cpp);
    vkCmdBindDescriptorSets(C->cb, VK_PIPELINE_BIND_POINT_COMPUTE, cs->cpl, 0, 1, &ds, 0, NULL);
    if (d->ind) vkCmdDispatchIndirect(C->cb, g_dispatch_args, g_dispatch_offset);
    else vkCmdDispatch(C->cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    mark_image_writes(5);
    barrier_all(C->cb);
    for (int i = 0; i < g_nwb; i++) {
        gw_t *w = &g_wb[i];
        buffer_written(w->addr, w->size, w->buf, w->off, w->p);
    }
    global_memory_written();
    g_stat_draws++;
}
static shader_t *clear_shader(shader_t *shader, const uint32_t *code, size_t bytes) {
    if (!shader->mod) {
        VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, bytes, code};
        VKCHECK(vkCreateShaderModule(g_dev, &info, NULL, &shader->mod));
    }
    return shader;
}
static shader_t *clear_vertex(void) {
    static const uint32_t code[] =
#include "clear.vert.inc"
    ;
    static shader_t shader = {.key = "clear-vertex"};
    return clear_shader(&shader, code, sizeof code);
}
static void internal_draw_state(VkCommandBuffer cb, VkPipeline pipeline, VkRect2D rect, VkExtent2D size, float depth) {
    VkViewport viewport = {0, 0, (float)size.width, (float)size.height, depth, depth};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &rect);
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetBlendConstants(cb, (const float[4]){0});
}
static void clear_draw_state(VkPipeline pipeline, VkRect2D rect, VkExtent2D size, float depth) {
    visibility_end(&C->visibility, C->cb);  // Internal clear/resolve triangles do not contribute guest samples.
    internal_draw_state(C->cb, pipeline, rect, size, depth);
}
// NVN records numeric clear values as floats. Convert them according to the
// attachment view, saturating an integer conversion as in the Metal renderer.
static VkClearColorValue clear_value(const float rgba[4], enum clear_type type) {
    VkClearColorValue value;
    for (int i = 0; i < 4; i++) {
        float f = rgba[i];
        if (type == CLEAR_UINT) value.uint32[i] = f > 0 ? (f < 4294967040.f ? (uint32_t)f : UINT32_MAX) : 0;
        else if (type == CLEAR_SINT) value.int32[i] = isnan(f) ? 0 : f >= 2147483520.f ? INT32_MAX : f <= -2147483648.f ? INT32_MIN : (int32_t)f;
        else value.float32[i] = f;
    }
    return value;
}
static shader_t *clear_fragment(enum clear_type type) {
    static const uint32_t floating[] =
#include "clear_color.vec4.inc"
    ;
    static const uint32_t unsigned_int[] =
#include "clear_color.uvec4.inc"
    ;
    static const uint32_t signed_int[] =
#include "clear_color.ivec4.inc"
    ;
    static shader_t shaders[] = {{.key="clear-float", .stage=1}, {.key="clear-uint", .stage=1}, {.key="clear-sint", .stage=1}};
    static const struct { const uint32_t *code; size_t bytes; } modules[] = {
        {floating, sizeof floating}, {unsigned_int, sizeof unsigned_int}, {signed_int, sizeof signed_int}};
    return clear_shader(&shaders[type], modules[type].code, modules[type].bytes);
}
static VkPipelineLayout clear_layout(void) {
    static VkPipelineLayout layout;
    if (!layout) {
        VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VkClearColorValue)};
        VkPipelineLayoutCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &push};
        VKCHECK(vkCreatePipelineLayout(g_dev, &info, NULL, &layout));
    }
    return layout;
}
static void draw_clear_color(VkFormat color, VkFormat depth, int stencil, VkSampleCountFlagBits samples, unsigned mask,
                             VkClearColorValue value, VkRect2D rect, VkExtent2D size) {
    VkPipelineLayout layout = clear_layout();
    pso_key k = {.vs = clear_vertex(), .fs = clear_fragment(clear_color_type(color)), .prim = NVN_DRAW_PRIMITIVE_TRIANGLES,
                 .nc = 1, .cf = {color}, .df = depth, .stencil = stencil, .samples = samples, .chmask = {.mask = mask}};
    VkPipeline pipeline = pso_get(&k, layout);
    if (!pipeline) return;
    clear_draw_state(pipeline, rect, size, 0);
    vkCmdPushConstants(C->cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof value, &value);
    vkCmdDraw(C->cb, 3, 1, 0, 0);
}
// A single-color pass can address any MRT slot without independentBlend, or
// any renderable texture slice without disturbing the game's attachment state.
static void clear_color_view(VkImageView view, VkFormat format, VkSampleCountFlagBits samples, VkExtent2D size, VkRect2D rect,
                             unsigned mask, VkClearColorValue value) {
    int load_clear = mask == 15 && rect.extent.width == size.width && rect.extent.height == size.height;
    rp_desc d = {.n = 1, .cv = {view}, .cf = {format}, .cl = {load_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD},
                 .cc = {value}, .w = size.width, .h = size.height, .samples = samples, .layout = VK_IMAGE_LAYOUT_GENERAL};
    begin_tracked_pass(&d);
    if (!load_clear) {
        if (mask == 15) {
            VkClearRect region = {.rect = rect, .layerCount = 1};
            VkClearAttachment attachment = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = value}};
            vkCmdClearAttachments(C->cb, 1, &attachment, 1, &region);
        } else draw_clear_color(format, VK_FORMAT_UNDEFINED, 0, samples, mask, value, rect, size);
    }
    rp_end(C->cb);
    barrier_all(C->cb);
}
static void clear_masked_color(int slot, unsigned mask, VkClearColorValue value, VkRect2D rect) {
    VkExtent2D size = {P.w, P.h};
    if (P.n != 1) {
        pass_end();
        clear_color_view(P.cv[slot], P.cf[slot], P.ct[slot]->samples, size, rect, mask, value);
        if (P.ct[slot]->linrow && !P.clv[slot] && !P.cly[slot]) { lin_writeback(P.ct[slot]); barrier_all(C->cb); }
    } else if (pass_begin()) {
        draw_clear_color(P.cf[slot], P.dt ? P.dt->f.f : VK_FORMAT_UNDEFINED, P.dt && P.dt->f.stencil, P.samples, mask, value, rect, size);
        P.cdrawn[slot] = 1;
    }
}
static VkExtent2D attachment_size(const vtex *v, image_view_key key) {
    uint32_t w=v->w>>key.range.baseMipLevel, h=v->h>>key.range.baseMipLevel;
    return (VkExtent2D){w ? w : 1,h ? h : 1};
}
static VkImageView attachment_slice(vtex *v, image_view_key key, uint32_t layer) {
    tex_t texture={.d={.target=v->target}};
    view_t view={.set_levels=1,.base_level=key.range.baseMipLevel,.nlevels=1,
        .set_layers=1,.min_layer=key.range.baseArrayLayer+layer,.nlayers=1};
    fmt_t format=v->f;format.f=key.format;
    return tex_view_format(v,&texture,&view,1,-1,VIEW_ATTACHMENT,format);
}
static void clear_color(const p_clear_color *c) {
    if (P.incompatible_samples || c->index < 0 || c->index >= P.n || !P.cv[c->index] || !(c->mask & 15)) return;
    vtex *texture=P.ct[c->index];
    image_view_key key=attachment_key(texture,P.cv[c->index]);
    VkExtent2D size=attachment_size(texture,key);
    VkClearRect r = {.rect = S.has_sc ? clipped_rect(S.sc,size.width,size.height) : (VkRect2D){{0,0},size}, .layerCount = 1};
    if (!r.rect.extent.width || !r.rect.extent.height) return;
    P.cwr[c->index] = 1;
    enum clear_type type = clear_color_type(P.cf[c->index]);
    VkClearColorValue value = clear_value(c->rgba, type);
    if (key.range.layerCount>1 || size.width!=P.w || size.height!=P.h) {
        pass_end();
        for (uint32_t layer=0;layer<key.range.layerCount;layer++) {
            VkImageView view=attachment_slice(texture,key,layer);
            if (view) clear_color_view(view,key.format,texture->samples,size,r.rect,c->mask&15,value);
        }
        if (texture->linrow && !P.clv[c->index] && !P.cly[c->index]) { lin_writeback(texture); barrier_all(C->cb); }
        return;
    }
    if ((c->mask & 15) != 15) {
        clear_masked_color(c->index, c->mask & 15, value, r.rect);
        return;
    }
    if (!P.open && r.rect.extent.width == P.w && r.rect.extent.height == P.h) {
        P.clr_c[c->index] = 1; P.cc[c->index] = value; return;
    }
    if (!pass_begin()) return;
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, {.color = value}};
    vkCmdClearAttachments(C->cb, 1, &a, 1, &r);
    P.cdrawn[c->index] = 1;
}
// vkCmdClearAttachments replaces all eight stencil bits. A rasterized
// REPLACE applies the guest's write mask and leaves every color target alone.
static void draw_clear_stencil(const p_clear_ds *c, VkRect2D rect, float depth, const rp_desc *pass) {
    static const uint32_t code[] =
#include "clear_stencil.frag.inc"
    ;
    static shader_t fragment = {.key = "clear-stencil", .stage = 1};
    pso_key k = {.vs = clear_vertex(), .fs = clear_shader(&fragment, code, sizeof code),
        .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .nc = pass->n, .df = pass->df, .stencil = 1, .samples = pass->samples};
    memcpy(k.cf, pass->cf, sizeof k.cf);
    k.ds = (ds_st){.dt = !!c->depth_mask, .dw = !!c->depth_mask, .df = NVN_COMPARE_FUNC_ALWAYS,
        .st = 1, .sf_f = NVN_COMPARE_FUNC_ALWAYS, .sf_b = NVN_COMPARE_FUNC_ALWAYS,
        .zpass_f = NVN_STENCIL_OP_REPLACE, .zpass_b = NVN_STENCIL_OP_REPLACE};
    VkPipeline pipeline = pso_get(&k, clear_layout());
    if (!pipeline) return;
    VkCommandBuffer cb = C->cb;
    clear_draw_state(pipeline, rect, (VkExtent2D){pass->w, pass->h}, depth);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)c->stencil_mask & 0xff);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)c->stencil & 0xff);
    vkCmdDraw(cb, 3, 1, 0, 0);
}
static void clear_ds(const p_clear_ds *c) {
    if (!P.dt) return;
    image_view_key key=attachment_key(P.dt,P.dv);
    VkExtent2D size=attachment_size(P.dt,key);
    VkClearRect r = {.rect = S.has_sc ? clipped_rect(S.sc,size.width,size.height) : (VkRect2D){{0,0},size}, .layerCount = 1};
    VkImageAspectFlags asp = (c->depth_mask ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (P.dt->f.stencil && (c->stencil_mask & 0xff) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!asp || !r.rect.extent.width || !r.rect.extent.height) return;
    float depth = c->depth > 0 ? fminf(c->depth, 1) : 0;
    if (key.range.layerCount>1 || size.width!=P.w || size.height!=P.h) {
        pass_end();
        for (uint32_t layer=0;layer<key.range.layerCount;layer++) {
            VkImageView view=attachment_slice(P.dt,key,layer);
            if (!view) continue;
            rp_desc pass={.dv=view,.df=key.format,.stencil=P.dt->f.stencil,.dl=VK_ATTACHMENT_LOAD_OP_LOAD,
                .sl=VK_ATTACHMENT_LOAD_OP_LOAD,.w=size.width,.h=size.height,.samples=P.dt->samples,.layout=VK_IMAGE_LAYOUT_GENERAL};
            begin_tracked_pass(&pass);
            if ((asp & VK_IMAGE_ASPECT_STENCIL_BIT) && (c->stencil_mask & 0xff)!=0xff) draw_clear_stencil(c,r.rect,depth,&pass);
            else {
                VkClearAttachment a={asp,0,{.depthStencil={depth,(uint32_t)c->stencil}}};
                vkCmdClearAttachments(C->cb,1,&a,1,&r);
            }
            rp_end(C->cb);barrier_all(C->cb);
        }
        return;
    }
    if ((asp & VK_IMAGE_ASPECT_STENCIL_BIT) && (c->stencil_mask & 0xff) != 0xff) {
        if (pass_begin()) {
            rp_desc pass={.n=P.n,.df=P.dt->f.f,.w=P.w,.h=P.h,.samples=P.samples};
            memcpy(pass.cf,P.cf,sizeof pass.cf);
            draw_clear_stencil(c,r.rect,depth,&pass);
        }
        return;
    }
    if (!P.open && r.rect.extent.width == P.w && r.rect.extent.height == P.h) {
        if (asp & VK_IMAGE_ASPECT_DEPTH_BIT) { P.clr_d = 1; P.cd = depth; }
        if (asp & VK_IMAGE_ASPECT_STENCIL_BIT) { P.clr_s = 1; P.cs = (uint32_t)c->stencil; }
        return;
    }
    if (!pass_begin()) return;
    VkClearAttachment a = {asp, 0, {.depthStencil = {depth, (uint32_t)c->stencil}}};
    vkCmdClearAttachments(C->cb, 1, &a, 1, &r);
}
// Copies and blits are checked against the image before they are recorded: a region past a level's extent, a level or layer the image lacks, or a blit the format does not support
// (VK_FORMAT_FEATURE_BLIT_SRC / DST, linear filtering) is invalid usage, and the Mali-G57 answered such work in Super Mario Odyssey with a lost device. Equal-size blits fall back to a copy
static int lvl_fits(const vtex *v, uint32_t lv, uint32_t layer, uint32_t nlayers, int32_t x, int32_t y, int32_t z, uint32_t w, uint32_t h, uint32_t d) {
    if (lv >= v->levels || x < 0 || y < 0 || z < 0) return 0;
    uint32_t lw = v->w >> lv ? v->w >> lv : 1, lh = v->h >> lv ? v->h >> lv : 1, ld = v->d >> lv ? v->d >> lv : 1;
    if (v->target == NVN_TEXTURE_TARGET_1D_ARRAY) lh = 1;
    if ((uint64_t)x + w > lw || (uint64_t)y + h > lh || (uint64_t)z + d > ld) return 0;
    return v->target == NVN_TEXTURE_TARGET_3D || (uint64_t)layer + nlayers <= v->layers;
}
static VkFormatFeatureFlags fmt_feat(VkFormat f) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, f, &fp); return fp.optimalTilingFeatures; }
// A 3D copy advances texels in Z. An array copy advances layers relative
// to the view, while its Vulkan image offset and extent remain two-dimensional.
static int texture_copy_region(const vtex *v, const view_t *view, const NVNcopyRegion *r,
                               VkImageSubresourceLayers *sub, VkOffset3D *off, VkExtent3D *extent) {
    int volume = v->target == NVN_TEXTURE_TARGET_3D;
    uint32_t count = r->depth ? (uint32_t)r->depth : 1;
    uint32_t mip = view && view->set_levels ? view->base_level : 0;
    int layers = !volume && view && view->set_layers;
    if (r->width <= 0 || r->height <= 0 || r->depth < 0 || r->zoffset < 0) return 0;
    if (layers && view->nlayers && (uint64_t)r->zoffset + count > view->nlayers) return 0;
    *sub = (VkImageSubresourceLayers){v->aspect,mip,volume ? 0 : (layers ? view->min_layer : 0)+(uint32_t)r->zoffset,volume ? 1 : count};
    *off = (VkOffset3D){r->xoffset,r->yoffset,volume ? r->zoffset : 0};
    *extent = (VkExtent3D){(uint32_t)r->width,(uint32_t)r->height,volume ? count : 1};
    return lvl_fits(v,mip,sub->baseArrayLayer,sub->layerCount,off->x,off->y,off->z,extent->width,extent->height,extent->depth);
}
#include "buffer_image_transfer.inc"
#include "block_targets_finish.inc"
// LAS Recomp's transfer-only depth scaling: depth -> buffer -> integer colour
// image -> nearest blit -> buffer -> depth. Preserve stencil through the same
// path with R8_UINT. Integer images keep the depth bits exactly, including D24.
// Scratch images grow on demand; old allocations use the normal fence retirement.
static vtex *depth_copy_scratch(int bytes, int side, uint32_t w, uint32_t h) {
    static vtex *cache[3][2]; int k = bytes == 1 ? 0 : bytes == 2 ? 1 : 2;
    vtex *v = cache[k][side];
    if (v && v->w >= w && v->h >= h) return v;
    if (v) { if (v->w > w) w = v->w; if (v->h > h) h = v->h; }
    VkFormat format = bytes == 1 ? VK_FORMAT_R8_UINT : bytes == 2 ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_UINT;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((fmt_feat(format) & need) != need) return NULL;
    vtex *next = calloc(1, sizeof *next); if (!next) return NULL;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, format, {w,h,1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(g_dev,&ii,NULL,&next->img) != VK_SUCCESS) { free(next); return NULL; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev,next->img,&mr);
    next->blk=img_mem(mr,&next->m,&next->moff); next->msize=mr.size; next->w=w; next->h=h;
    VKCHECK(vkBindImageMemory(g_dev,next->img,next->m,next->moff));
    to_general(C->cb,next->img,VK_IMAGE_ASPECT_COLOR_BIT);
    if(v) tex_free(v); cache[k][side]=next; return next;
}
static void mirror_offsets(VkOffset3D offsets[2], int flags) {
    if (flags & NVN_COPY_FLAGS_MIRROR_X_BIT) {
        int32_t x=offsets[0].x; offsets[0].x=offsets[1].x; offsets[1].x=x;
    }
    if (flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) {
        int32_t y=offsets[0].y; offsets[0].y=offsets[1].y; offsets[1].y=y;
    }
}
static int depth_copy_scaled(vtex *s, vtex *d, const p_copy_t2t *c, uint32_t sl, uint32_t sly, uint32_t dl, uint32_t dly) {
    VkImageAspectFlags aspects=s->aspect & d->aspect;
    for (unsigned aspect=VK_IMAGE_ASPECT_DEPTH_BIT; aspect<=VK_IMAGE_ASPECT_STENCIL_BIT; aspect<<=1) {
        if (!(aspects & aspect)) continue;
        int bytes=aspect==VK_IMAGE_ASPECT_STENCIL_BIT ? 1 : s->f.f==VK_FORMAT_D16_UNORM || s->f.f==VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4;
        const NVNcopyRegion *sr=&c->sr, *dr=&c->dr;
        vtex *a=depth_copy_scratch(bytes,0,sr->width,sr->height), *b=depth_copy_scratch(bytes,1,dr->width,dr->height);
        if (!a || !b) return 0;
        VkDeviceSize ns=(VkDeviceSize)sr->width*sr->height*bytes, nd=(VkDeviceSize)dr->width*dr->height*bytes;
        VkDeviceSize off=ring_alloc(ns>nd?ns:nd,16); if(off==VK_WHOLE_SIZE) return 0;
        VkBuffer buffer=C->ring;
        VkBufferImageCopy ic={off,0,0,{aspect,sl,sly,1},{sr->xoffset,sr->yoffset,0},{sr->width,sr->height,1}};
        vkCmdCopyImageToBuffer(C->cb,s->img,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&ic); barrier_all(C->cb);
        ic.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; ic.imageOffset=(VkOffset3D){0,0,0};
        vkCmdCopyBufferToImage(C->cb,buffer,a->img,VK_IMAGE_LAYOUT_GENERAL,1,&ic); barrier_all(C->cb);
        VkImageBlit bl={{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{{0,0,0},{sr->width,sr->height,1}},
                       {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{{0,0,0},{dr->width,dr->height,1}}};
        mirror_offsets(bl.srcOffsets,c->flags);
        vkCmdBlitImage(C->cb,a->img,VK_IMAGE_LAYOUT_GENERAL,b->img,VK_IMAGE_LAYOUT_GENERAL,1,&bl,VK_FILTER_NEAREST); barrier_all(C->cb);
        ic.imageExtent=(VkExtent3D){dr->width,dr->height,1};
        vkCmdCopyImageToBuffer(C->cb,b->img,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&ic); barrier_all(C->cb);
        ic.imageSubresource=(VkImageSubresourceLayers){aspect,dl,dly,1}; ic.imageOffset=(VkOffset3D){dr->xoffset,dr->yoffset,0};
        vkCmdCopyBufferToImage(C->cb,buffer,d->img,VK_IMAGE_LAYOUT_GENERAL,1,&ic); barrier_all(C->cb);
    }
    return 1;
}
#include "image_copy_draw.inc"
#include "draw_texture.inc"
static VkOffset3D copy_end(VkOffset3D start, VkExtent3D size) {
    return (VkOffset3D){start.x+(int32_t)size.width,start.y+(int32_t)size.height,start.z+(int32_t)size.depth};
}
static NVNcopyRegion whole_mip(const vtex *v, const view_t *view) {
    unsigned level = view && view->set_levels ? view->base_level : 0;
    if (level >= v->levels) return (NVNcopyRegion){0};
    unsigned w=v->w >> level, h=v->h >> level;
    return (NVNcopyRegion){.width=w ? (int)w : 1, .height=h && v->target!=NVN_TEXTURE_TARGET_1D_ARRAY ? (int)h : 1, .depth=1};
}
// D32 and D32/S8 expose the same four depth bytes in a buffer transfer.
// Copy that aspect alone, so the destination's stencil remains intact.
static void copy_float_depth_plane(vtex *s, vtex *d, VkImageCopy r) {
    VkDeviceSize bytes=(VkDeviceSize)r.extent.width*r.extent.height*r.extent.depth*r.srcSubresource.layerCount*sizeof(float);
    VkDeviceSize offset=ring_alloc(bytes,16);
    if (offset==VK_WHOLE_SIZE) return;
    r.srcSubresource.aspectMask=r.dstSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    VkBufferImageCopy transfer={offset,0,0,r.srcSubresource,r.srcOffset,r.extent};
    vkCmdCopyImageToBuffer(C->cb,s->img,VK_IMAGE_LAYOUT_GENERAL,C->ring,1,&transfer);
    barrier_all(C->cb);
    transfer.imageSubresource=r.dstSubresource;
    transfer.imageOffset=r.dstOffset;
    vkCmdCopyBufferToImage(C->cb,C->ring,d->img,VK_IMAGE_LAYOUT_GENERAL,1,&transfer);
    barrier_all(C->cb);
}
static void copy_t2t(const p_copy_t2t *c) {
    if (vk_skip() & SK_T2T) return;
    if (g_plog) LOG("plog:  t2t %llu (fmt %#x) %dx%d → %llu (fmt %#x) %dx%d flags %#x", (unsigned long long)c->src->serial, c->src->d.format, c->sr.width, c->sr.height, (unsigned long long)c->dst->serial, c->dst->d.format, c->dr.width, c->dr.height, c->flags);
    vtex *s = tex_get(c->src, 0), *d = tex_get(c->dst, 0); if (!s || !d) return;
    pass_end();
    p_copy_t2t whole;
    if (c->flags & DM_COPY_RESOLVE) {
        whole=*c;
        whole.sr=whole_mip(s,c->has_sv ? &c->sv : NULL);
        whole.dr=whole_mip(d,c->has_dv ? &c->dv : NULL);
        c=&whole;
    }
    VkImageCopy region;
    VkExtent3D dest;
    if (!texture_copy_region(s,c->has_sv ? &c->sv : NULL,&c->sr,&region.srcSubresource,&region.srcOffset,&region.extent) ||
        !texture_copy_region(d,c->has_dv ? &c->dv : NULL,&c->dr,&region.dstSubresource,&region.dstOffset,&dest)) {
        RED_ONCE(52,"texture copy region outside its mip or view layers");
        return;
    }
    VkImageSubresourceLayers src=region.srcSubresource, dst=region.dstSubresource;
    int depth = s->f.depth || d->f.depth;
    int mirror = c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT);
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height;
    if (src.layerCount!=dst.layerCount || region.extent.depth!=dest.depth ||
        (c->flags & ~(DM_COPY_RESOLVE | NVN_COPY_FLAGS_ENGINE_2D_BIT | NVN_COPY_FLAGS_LINEAR_FILTER_BIT |
                      NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT))) {
        RED_ONCE(52,"texture copy with incompatible slices or flags");
        return;
    }
    if (s->f.dec && d->f.dec && same && !mirror && !(c->flags & DM_COPY_RESOLVE) &&
        !s->gpu && !d->gpu && c->src->mem && c->dst->mem) {
        if (copy_stored_blocks(c,region)) return;
        // A dependency inside the storage copy can finish a segment.
        s=tex_get(c->src,0); d=tex_get(c->dst,0);
        if (!s || !d) return;
    }
    d->gpu = 1;
    if (src.aspectMask!=dst.aspectMask) {
        if (same && !mirror && s->samples==1 && d->samples==1 &&
            ((s->f.f==VK_FORMAT_D32_SFLOAT && d->f.f==VK_FORMAT_D32_SFLOAT_S8_UINT) ||
             (s->f.f==VK_FORMAT_D32_SFLOAT_S8_UINT && d->f.f==VK_FORMAT_D32_SFLOAT)))
            copy_float_depth_plane(s,d,region);
        else RED_ONCE(52,"texture copy between incompatible aspects or formats");
        return;
    }
    if (depth && s->f.f!=d->f.f) { RED_ONCE(52,"depth copy between different host formats is unsupported"); return; }
    if (s->samples != d->samples) {
        if (!(c->flags & DM_COPY_RESOLVE) || s->samples <= 1 || d->samples != 1 || !same || mirror || s->f.f != d->f.f) {
            RED_ONCE(96, "unsupported resolve between formats %d/%d with %u/%u samples", s->f.f,d->f.f,s->samples,d->samples);
            return;
        }
        if (depth) {
            for (uint32_t i=0;i<src.layerCount;i++)
                if (!draw_image_slice(c,s,d,src.mipLevel,src.baseArrayLayer+i,dst.mipLevel,dst.baseArrayLayer+i,0)) {
                    RED_ONCE(96,"depth resolve requires a sampled source and renderable destination"); return;
                }
            return;
        }
        VkImageResolve resolve = {src,region.srcOffset,dst,region.dstOffset,region.extent};
        vkCmdResolveImage(C->cb,s->img,VK_IMAGE_LAYOUT_GENERAL,d->img,VK_IMAGE_LAYOUT_GENERAL,1,&resolve);
        barrier_all(C->cb);
        return;
    }
    if (same && !mirror && s->f.f==d->f.f) {
        vkCmdCopyImage(C->cb,s->img,VK_IMAGE_LAYOUT_GENERAL,d->img,VK_IMAGE_LAYOUT_GENERAL,1,&region);
        barrier_all(C->cb);
        return;
    }
    if (s->samples>1) { RED_ONCE(96,"multisample image copies require matching extents and formats"); return; }
    VkFormatFeatureFlags sf=fmt_feat(s->f.f), df=fmt_feat(d->f.f);
    int linear = !depth && !same && ((c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) ||
                 ((c->flags & DM_COPY_RESOLVE) && clear_color_type(s->f.f)==CLEAR_FLOAT));
    int blit = (sf & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (df & VK_FORMAT_FEATURE_BLIT_DST_BIT) &&
               clear_color_type(s->f.f)==clear_color_type(d->f.f) &&
               (!linear || (sf & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT));
    if (depth && getenv("DM_VK_DEPTH_COPY_TRANSFER")) blit=0;
    if (blit) {
        VkImageBlit b = {src,{region.srcOffset,copy_end(region.srcOffset,region.extent)},
                         dst,{region.dstOffset,copy_end(region.dstOffset,dest)}};
        mirror_offsets(b.srcOffsets,c->flags);
        vkCmdBlitImage(C->cb,s->img,VK_IMAGE_LAYOUT_GENERAL,d->img,VK_IMAGE_LAYOUT_GENERAL,1,&b,linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else if (!depth && (d->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
               clear_color_type(s->f.f)==CLEAR_FLOAT && clear_color_type(d->f.f)==CLEAR_FLOAT &&
               s->target!=NVN_TEXTURE_TARGET_3D && d->target!=NVN_TEXTURE_TARGET_3D &&
               (s!=d || src.mipLevel!=dst.mipLevel || src.baseArrayLayer!=dst.baseArrayLayer)) {
        for (uint32_t i=0; i<src.layerCount; ++i)
            if (!draw_image_slice(c,s,d,src.mipLevel,src.baseArrayLayer+i,dst.mipLevel,dst.baseArrayLayer+i,linear)) {
                RED_ONCE(52,"color copy requires a sampled source and renderable destination"); return;
            }
        return;
    } else if (depth && !linear && s->target!=NVN_TEXTURE_TARGET_3D && d->target!=NVN_TEXTURE_TARGET_3D) {
        for (uint32_t i=0;i<src.layerCount;i++)
            if (!depth_copy_scaled(s,d,c,src.mipLevel,src.baseArrayLayer+i,dst.mipLevel,dst.baseArrayLayer+i)) {
                RED_ONCE(52,"depth copy has no supported transfer scratch format"); return;
            }
    } else { RED_ONCE(52,"no supported texture blit between formats %d and %d",s->f.f,d->f.f); return; }
    barrier_all(C->cb);
}
#include "temporal_upscale.inc"
// NVN2's reconstruction command can still produce its spatial output when a
// temporal scaler or its depth/motion inputs are unavailable. Use the sampled
// draw for float/HDR destinations even on drivers with no native linear blit.
static void dlss_eval(const p_dlss *job) {
    if (!job->tex[DLSS_COLOR] || !job->tex[DLSS_OUTPUT]) {
        RED_ONCE(118, "DLSS evaluation needs a color input and output"); return;
    }
    pass_end();
    vtex *src=tex_get(job->tex[DLSS_COLOR],0), *dst=tex_get(job->tex[DLSS_OUTPUT],0);
    if (!src || !dst || src==dst || src->samples!=1 || dst->samples!=1 ||
        src->target!=NVN_TEXTURE_TARGET_2D || dst->target!=NVN_TEXTURE_TARGET_2D ||
        src->f.depth || dst->f.depth || clear_color_type(src->f.f)!=CLEAR_FLOAT || clear_color_type(dst->f.f)!=CLEAR_FLOAT ||
        !(src->usage&VK_IMAGE_USAGE_SAMPLED_BIT) || !(dst->usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        RED_ONCE(118, "DLSS spatial scaling requires distinct sampled and renderable color images"); return;
    }
    p_copy_t2t copy={.src=job->tex[DLSS_COLOR],.dst=job->tex[DLSS_OUTPUT],
        .sr={job->base[DLSS_COLOR][0],job->base[DLSS_COLOR][1],0,job->iw>0 ? job->iw : (int)src->w,
             job->ih>0 ? job->ih : (int)src->h,1},
        .dr={job->base[DLSS_OUTPUT][0],job->base[DLSS_OUTPUT][1],0,job->ow>0 ? job->ow : (int)dst->w,
             job->oh>0 ? job->oh : (int)dst->h,1}};
    VkImageSubresourceLayers sub; VkOffset3D off; VkExtent3D extent;
    if (!texture_copy_region(src,NULL,&copy.sr,&sub,&off,&extent) ||
        !texture_copy_region(dst,NULL,&copy.dr,&sub,&off,&extent)) {
        RED_ONCE(118, "DLSS rectangle lies outside its color image"); return;
    }
    if (!temporal_eval(job,&copy,src,dst)) {
        if (!draw_image_slice(&copy,src,dst,0,0,0,0,1)) {
            RED_ONCE(118, "DLSS spatial scaling failed for the bound color formats"); return;
        }
        RED_ONCE(118, "DLSS temporal reconstruction unavailable; scaled the input color bilinearly");
    }
    dst->gpu=1;
    if (dst->linrow) { lin_writeback(dst); barrier_all(C->cb); }
    for (int i=0;i<P.n;i++) if (P.ct[i]==dst) P.cwr[i]=1;
}

static void clear_tex(const p_clear_tex *c) {
    if (!c->tex || (vk_skip() & SK_CLEAR)) return;
    if (g_plog) LOG("plog:  clear tex %llu fmt %#x", (unsigned long long)c->tex->serial, c->tex->d.format);
    vtex *v = tex_get(c->tex, 0); if (!v) return;
    uint32_t lv = c->has_view && c->view.set_levels ? c->view.base_level : 0;
    if (lv >= v->levels) { static int n; if (n++ < 8) LOG("[RED] vk: clear of level %u of a %u-level texture: skipped", lv, v->levels); return; }
    uint32_t w = v->w >> lv ? v->w >> lv : 1, h = v->h >> lv ? v->h >> lv : 1;
    if (v->target == NVN_TEXTURE_TARGET_1D_ARRAY) h = 1;
    VkRect2D rect = clipped_rect((p_scissor){c->r.xoffset,c->r.yoffset,c->r.width,c->r.height}, w, h);
    unsigned mask = c->mask & 15;
    if (!mask || !rect.extent.width || !rect.extent.height) return;
    int volume = v->target == NVN_TEXTURE_TARGET_3D;
    uint32_t layers = volume ? (v->d >> lv ? v->d >> lv : 1) : v->layers;
    uint32_t base = !volume && c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    uint32_t count = c->r.depth ? (uint32_t)c->r.depth : 1;
    uint64_t first = (uint64_t)base + (uint32_t)c->r.zoffset;
    if (c->r.zoffset < 0 || c->r.depth < 0 || first + count > layers ||
        (!volume && c->has_view && c->view.set_layers && c->view.nlayers && (uint64_t)c->r.zoffset + count > c->view.nlayers)) {
        RED_ONCE(84, "clear outside texture layers"); return;
    }
    fmt_t fmt = view_format(v, &c->view, c->has_view);
    enum clear_type type = clear_color_type(fmt.f);
    VkClearColorValue value;
    if (c->is_uint && type != CLEAR_FLOAT) memcpy(&value, &c->c, sizeof value);
    else if (c->is_uint) for (int i = 0; i < 4; i++) value.float32[i] = (float)c->c.u[i];
    else value = clear_value(c->c.f, type);
    int whole = rect.extent.width == w && rect.extent.height == h && mask == 15;
    pass_end();
    if (whole && fmt.f == v->f.f && (!volume || count == layers)) {
        VkImageSubresourceRange range = {v->aspect, lv, 1, volume ? 0 : (uint32_t)first, volume ? 1 : count};
        if (v->f.depth) {
            VkClearDepthStencilValue ds = {c->c.f[0], c->tex->d.format == NVN_FORMAT_STENCIL8 ? c->c.u[0] : c->c.u[1]};
            vkCmdClearDepthStencilImage(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, &ds, 1, &range);
        } else vkCmdClearColorImage(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
        barrier_all(C->cb);
    } else {
        if (fmt.depth || !(v->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            RED_ONCE(84, "texture region requires a color-renderable view"); return;
        }
        view_t slice = c->has_view ? c->view : (view_t){0};
        slice.set_levels = slice.set_layers = 1; slice.base_level = lv; slice.nlevels = slice.nlayers = 1;
        for (uint32_t i = 0; i < count; i++) {
            slice.min_layer = (uint32_t)first + i;
            VkImageView view = tex_view(v, c->tex, &slice, 1, -1, VIEW_ATTACHMENT);
            if (!view) return;
            clear_color_view(view, fmt.f, v->samples, (VkExtent2D){w,h}, rect, mask, value);
        }
    }
    v->gpu = 1;
    if (v->linrow && !lv && !first) { lin_writeback(v); barrier_all(C->cb); }
    for (int i = 0; i < P.n; i++) if (P.ct[i] == v) P.cwr[i] = 1;
}

// ---------- Submission ----------
static void begin_segment(void) {
    vk_init();
    g_ci = (g_ci + 1) % NCTX; ctx_t *c = &g_ctx[g_ci];
    uint64_t w0 = now_ns();
    pthread_mutex_lock(&g_mu); while (c->fence_val > g_done) pthread_cond_wait(&g_cv, &g_mu); pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_wait_ns, now_ns() - w0);
    C = c; C->used = 0; C->capacity = RING; C->up_used = 0; g_ndc = 0; g_wgen++; C->ring = C->ring0; C->ringp = C->ringp0;
    C->raster_draws=0; C->cut_copy=0;
    C->dp = C->dp0; VKCHECK(vkResetDescriptorPool(g_dev, C->dp, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VKCHECK(vkBeginCommandBuffer(C->cb, &bi)); VKCHECK(vkBeginCommandBuffer(C->up, &bi));
    if (C->qp) { vkCmdResetQueryPool(C->up, C->qp, 0, 2); vkCmdWriteTimestamp(C->up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, C->qp, 0); }
}
void mtl_sub_begin(void) {
    condition_begin();
    begin_segment();
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (!n) return;
    pass_end();
    g_wgen++; // A copy destination must not alias a cached source snapshot.
    VkDeviceSize offset = copy_in(src, n, 16);
    if (offset != VK_WHOLE_SIZE) buffer_written(dst, n, C->ring, offset, C->ringp + offset);
}
static void feedback_state(const p_tf_state *op) {
    S.feedback_active = op->what == 0 || op->what == 3;
    if (op->what != 0) {
        if (!op->ctl) return;
        if (!mtl_addr(op->ctl, FEEDBACK_BYTES, NULL, NULL)) {
            RED_ONCE(103, "transform feedback control is outside a registered memory pool");
            return;
        }
    }
    if (!S.feedback) {
        S.feedback = calloc(1, sizeof *S.feedback);
        if (!S.feedback) die("Vulkan: cannot allocate feedback control");
        atomic_init(&S.feedback->refs, 1);
        S.feedback->buffer = buf_new(FEEDBACK_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &S.feedback->memory, NULL);
    }
    feedback_control *control = S.feedback;
    feedback_hold(control);
    pass_end();
    barrier_all(C->cb);
    if (op->what == 0) {
        const uint32_t initial[8] = {0, 0, 0, 0, 0, 1, 0, 0};
        vkCmdUpdateBuffer(C->cb, control->buffer, 0, sizeof initial, initial);
    } else if (op->what == 3) {
        VkDeviceSize offset = copy_in(op->ctl, FEEDBACK_BYTES, 4);
        VkBufferCopy region = {offset, 0, FEEDBACK_BYTES};
        vkCmdCopyBuffer(C->cb, C->ring, control->buffer, 1, &region);
    } else {
        VkDeviceSize offset = ring_alloc(FEEDBACK_BYTES, 4);
        VkBufferCopy region = {0, offset, FEEDBACK_BYTES};
        vkCmdCopyBuffer(C->cb, control->buffer, C->ring, 1, &region);
        buffer_written(op->ctl, FEEDBACK_BYTES, C->ring, offset, C->ringp + offset);
    }
    barrier_all(C->cb);
}
static void buffer_update(uint64_t addr, const void *data, uint64_t n) {
    if (!n) return;
    gw_wait_prior(addr, n);
    VkDeviceSize offset = ring_put(data, n, 16);
    if (offset == VK_WHOLE_SIZE) return;
    // Preserve CPU-side descriptor-handle decoding for inline updates. The
    // ordered completion copy also overrides older GPU writes to these bytes.
    memcpy((void *)(uintptr_t)addr, data, n);
    buffer_publish(addr, n, C->ring, offset, C->ringp + offset, 1);
}
static void buffer_clear(const p_clear_buf *clear) {
    uint64_t n = clear->size & ~3ull;
    if (!n) return;
    VkDeviceSize offset = ring_alloc(n, 16);
    if (offset == VK_WHOLE_SIZE) return;
    for (uint64_t i = 0; i < n; i += 4) memcpy(C->ringp + offset + i, &clear->value, 4);
    buffer_publish(clear->addr, n, C->ring, offset, C->ringp + offset, 1);
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post_add((post_t){1, (void *)val, mode, v}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    visibility_counter *counter = NULL;
    C->timed_reports = g_segment_timestamps;
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
        visibility_end(&C->visibility, C->cb);
        counter = visibility_retain(S.visibility);
    }
    post_add((post_t){.kind=2,.addr=addr,.type=type,.visibility=counter});
}
static void mtl_exec_(const uint8_t *p);
void mtl_exec(const uint8_t *p) { uint64_t t0 = now_ns(); mtl_exec_(p); atomic_fetch_add(&g_rec_ns, now_ns() - t0); }
// NVN's initial command buffer state, as the Metal backend's state_reset: games set some of it only by state objects or never (Metroid Dread never sets the stencil value mask,
// which starts at 0xff; left at 0 its G-buffer draws, stencil test "not equal 2", failed everywhere and the level's opaque geometry stayed black)
static void state_defaults(void) {
    memset(&S, 0, sizeof S);
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
}
// The shared recorder serializes submission and finalization. Preserve the same
// queue/program lifetime rules as Metal: no bindings may outlive their program.
typedef struct queue_state { uint64_t serial; typeof(S) s; typeof(P) p; struct queue_state *next; } queue_state;
static queue_state *g_queue_states, *g_active_queue;
static int g_state_ready;
void mtl_queue_activate(uint64_t serial) {
    if (g_active_queue && g_active_queue->serial == serial) return;
    if (P.open) die("Vulkan: queue switch inside a render pass");
    if (g_active_queue) { g_active_queue->s = S; g_active_queue->p = P; }
    queue_state *q = g_queue_states;
    while (q && q->serial != serial) q = q->next;
    if (q) { S = q->s; P = q->p; }
    else {
        q = calloc(1, sizeof *q); if (!q) die("Vulkan: queue state allocation failed");
        q->serial = serial; q->next = g_queue_states; g_queue_states = q;
        state_defaults(); memset(&P, 0, sizeof P);
    }
    g_active_queue = q; g_state_ready = 1;
}
void mtl_queue_forget(uint64_t serial) {
    queue_state **at = &g_queue_states;
    while (*at && (*at)->serial != serial) at = &(*at)->next;
    if (!*at) return;
    queue_state *q = *at; *at = q->next;
    visibility_release(g_active_queue == q ? S.visibility : q->s.visibility);
    feedback_release(g_active_queue == q ? S.feedback : q->s.feedback);
    if (g_active_queue == q) { g_active_queue = NULL; state_defaults(); memset(&P, 0, sizeof P); }
    free(q);
}
static void prog_unbind(typeof(S) *s, const prog_t *p) {
    if (s->cprog == p) s->cprog = NULL;
    for (int st = 0; st < 5; st++) if (s->bst[st] == p) s->bst[st] = NULL;
}
void mtl_prog_forget(const prog_t *p) {
    prog_unbind(&S, p);
    for (queue_state *q = g_queue_states; q; q = q->next) if (q != g_active_queue) prog_unbind(&q->s, p);
}
static void mtl_exec_(const uint8_t *p) {
    if (!g_state_ready) { g_state_ready = 1; state_defaults(); }
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        int block_command=h.op==OP_DRAW || h.op==OP_DRAW_IDX || h.op==OP_DRAW_TEX || h.op==OP_CLEAR_COLOR;
        if (block_command) block_targets_reload();
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); mtl_exec_(t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: {
                uint64_t type, addr; memcpy(&type, a, 8); memcpy(&addr, a + 8, 8);
                if (DM_GAME_RENDER_COND) {
                    int timestamp = type == NVN_COUNTER_TYPE_TIMESTAMP || type == NVN_COUNTER_TYPE_TIMESTAMP_TOP;
                    report_insert(timestamp ? &pending_timestamps : &gpu_reports, addr);
                }
                mtl_sub_counter(addr, (uint32_t)type);
                break;
            }
            case OP_TF_BIND: {
                const p_tf_bind *b = (const p_tf_bind *)a;
                if ((unsigned)b->index < 4) S.feedback_streams[b->index] = *b;
                break;
            }
            case OP_TF_STATE: feedback_state((const p_tf_state *)a); break;
            case OP_RENDER_COND: condition_set((const p_render_cond *)a); break;
            case OP_RESET_COUNTER: {
                uint64_t type; memcpy(&type, a, sizeof type);
                if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
                    visibility_end(&C->visibility, C->cb);
                    if (!S.visibility) {
                        S.visibility = calloc(1, sizeof *S.visibility);
                        if (!S.visibility) die("Vulkan: cannot allocate queue visibility counter");
                        atomic_init(&S.visibility->refs, 1);
                        if (!g_precise_visibility) RED_ONCE(97, "GPU reports conservative visibility; exact sample counts unavailable");
                    }
                    post_add((post_t){.kind=POST_VISIBILITY_RESET,.visibility=visibility_retain(S.visibility)});
                }
                break;
            }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1; break; }
            case OP_CLEAR_BUF: buffer_clear((const p_clear_buf *)a); break;
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); buffer_update(d, a + 16, n); break; }
            case OP_SET_RT: set_rt((const p_set_rt *)a); break;
            case OP_SET_TEX_POOL: S.tex_base = nvn_dpool_base(((const p_set_pool *)a)->pool, 1); break;
            case OP_SET_SAMP_POOL: S.samp_base = nvn_dpool_base(((const p_set_pool *)a)->pool, 0); break;
            case OP_CLEAR_COLOR: if (!conditional_skip) clear_color((const p_clear_color *)a); break;
            case OP_CLEAR_DS: if (!conditional_skip) clear_ds((const p_clear_ds *)a); break;
            case OP_MULTI_DRAW: if (!conditional_skip) draw_multi((const p_multi_draw *)a); break;
            case OP_DRAW_TEX: if (!conditional_skip) draw_texture((const p_draw_tex *)a); break;
            case OP_DRAW: { p_draw t; if (!conditional_skip) draw(draw_rec(a, h.size, &t), NULL); break; }
            case OP_DRAW_IDX: { p_draw_idx t; if (!conditional_skip) draw(NULL, draw_idx_rec(a, h.size, &t)); break; }
            case OP_DISPATCH: dispatch((const p_dispatch *)a); break;
            case OP_COPY_B2T: copy_b2t((const p_copy_b2t *)a, h.size); break;
            case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
            case OP_COPY_T2B: copy_t2b((const p_copy_b2t *)a, h.size); break;
            case OP_DLSS: dlss_eval((const p_dlss *)a); break;
            case OP_CLEAR_TEX: clear_tex((const p_clear_tex *)a); break;
            case OP_BARRIER: {
                // Early replay fixtures used a header-only pass boundary.
                // A public NVN record always carries the guest's barrier bits.
                uint32_t bits = UINT32_MAX;
                if (h.size >= sizeof h + sizeof bits) memcpy(&bits, a, sizeof bits);
                if (bits) {
                    pass_end();
                    // The ordinary end-of-pass dependency targets the next
                    // graphics/transfer consumer. A guest barrier also orders
                    // compute and indirect reads, even between render passes.
                    barrier_all(C->cb);
                    for (int i = 0; i < P.n; ++i) P.cwr[i] = 1;
                }
                break;
            }
            case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.has_sc = 1; break;
            case OP_BIND_PROGRAM: { const p_bind_program *b = (const p_bind_program *)a; if (b->stages & 0x20) S.cprog = b->prog; for (int st = 0; st < 5; st++) if (b->stages >> st & 1) S.bst[st] = b->prog; break; }
            case OP_BIND_STATE: {
                const p_bind_state *b = (const p_bind_state *)a; blend_st bs;
                switch (b->kind) {
                    case 0: memcpy(&bs, b->bytes, sizeof bs); S.blend[bs.target] = bs; break;
                    case 1: memcpy(&S.color, b->bytes, sizeof S.color); break; case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
                    case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break; case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break; case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break;
                }
                break;
            }
            case OP_BIND_VATTR: { const p_bind_vattr *b = (const p_bind_vattr *)a; memcpy(S.attr, b->st, 4 * b->n); S.nattr = b->n; break; }
            case OP_BIND_VSTREAM: { const p_bind_vstream *b = (const p_bind_vstream *)a; memcpy(S.stream, b->st, 8 * b->n); S.nstream = b->n; break; }
            case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(h.op, a, &tmp); memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * b->n); break; }
            case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(h.op, a, &tmp); if (b->stage < 6 && b->index < 16) S.ubo[b->stage][b->index] = *b; break; }
            case OP_BIND_SSBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage < 6 && b->index < 16) S.ssbo[b->stage][b->index] = *b; break; }
            case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: {
                p_bind_handles tmp; const p_bind_handles *b = bind_rec(h.op, a, &tmp);
                if ((unsigned)b->stage >= 6 || (unsigned)b->n > 32) die("Vulkan: invalid NVN binding record");
                if (h.op != OP_BIND_TEX1 && h.size < sizeof h + bind_payload_bytes(b->n, b->defer))
                    die("Vulkan: truncated NVN binding record");
                if (b->defer && b->src) { cpu_ready(b->src, 8 * b->n); b = bind_deferred(b, &tmp); }
                uint64_t *dst = (h.op == OP_BIND_TEX || h.op == OP_BIND_TEX1) ? S.tex[b->stage] : h.op == OP_BIND_SAMP ? S.samp[b->stage] : S.img[b->stage];
                int lim = (h.op == OP_BIND_TEX || h.op == OP_BIND_TEX1) ? NTEX : h.op == OP_BIND_SAMP ? 128 : 32;
                if (b->first >= 0 && b->first <= lim - b->n) memcpy(dst + b->first, b->h, 8 * b->n);
                break;
            }
            case OP_STENCIL: { const p_stencil *b = (const p_stencil *)a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; } break; }
            case OP_PRIM_RESTART: S.restart = *(const p_prim_restart *)a; break;
            case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
            case OP_RAST_DISCARD: S.rasterizer_discard = ((const p_rast_discard *)a)->on != 0; break;
            case OP_VIEWPORTS: { const p_viewports *b = (const p_viewports *)a; for (int i = 0; i < b->n; i++) memcpy(S.vp[b->first + i], b->v[i], 16); S.has_vp = 1; break; }
            case OP_VP_SWIZZLE: {
                const p_vp_swizzle *b = (const p_vp_swizzle *)a;
                if (h.size < sizeof h + offsetof(p_vp_swizzle, sw) || (unsigned)b->first > 16 ||
                    (unsigned)b->n > 16u - (unsigned)b->first || h.size < sizeof h + offsetof(p_vp_swizzle, sw) + 4u * b->n)
                    die("Vulkan: invalid viewport swizzle record");
                if (b->n) memcpy(S.vp_sw[b->first], b->sw, 4u * b->n);
                break;
            }
            case OP_DEPTH_RANGE: { const p_depth_range *b = (const p_depth_range *)a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
            case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_depth_clamp *)a)->on != 0; break;
            case OP_PUSH_CONST: {
                const p_push_const *b = (const p_push_const *)a;
                if (h.size < sizeof h + sizeof *b || b->size > h.size - sizeof h - sizeof *b ||
                    b->offset > DMSH_PUSH_SIZE || b->size > DMSH_PUSH_SIZE - (uint32_t)b->offset)
                    die("Vulkan: invalid NVN push-constant range");
                memcpy(S.push[b->compute != 0] + b->offset, b->data, b->size);
                break;
            }
            case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
            case OP_ALPHA_REF: S.alpha_ref=((const p_alpha_ref *)a)->ref; break;
            case OP_COPY_STRIDE: { const p_copy_stride *cs = (const p_copy_stride *)a; if (cs->image) S.copy_img_stride = cs->stride; else S.copy_stride = cs->stride; break; }
            default: {
                static uint8_t warned[256];
                if (h.op >= sizeof warned || !warned[h.op]) {
                    LOG("[RED] vk: unimplemented NVN command record %u (size %u)", h.op, h.size);
                    if (h.op < sizeof warned) warned[h.op] = 1;
                }
                break;
            }
        }
        if (block_command) block_targets_finish();
        if (h.size < sizeof h || h.size % 8) die("Vulkan: invalid NVN command size %u", h.size);
        p += h.size;
    }
}
uint64_t mtl_sub_commit(void) {
    pass_end();
    if (C->qp) vkCmdWriteTimestamp(C->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, C->qp, 1);
    VKCHECK(vkEndCommandBuffer(C->cb)); VKCHECK(vkEndCommandBuffer(C->up));
    uint64_t fence = ++g_fence; C->fence_val = fence;
    VkCommandBuffer cbs[2] = {C->up, C->cb};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 2, cbs, 0, NULL};
    VKCHECK(vkResetFences(g_dev, 1, &C->fence));
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO}; uint64_t wv, sv; VkPipelineStageFlags wst = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    pthread_mutex_lock(&g_qmu);
    if (g_tl) { wv = g_tlv; sv = ++g_tlv; ts.waitSemaphoreValueCount = 1; ts.pWaitSemaphoreValues = &wv; ts.signalSemaphoreValueCount = 1; ts.pSignalSemaphoreValues = &sv;
                si.pNext = &ts; si.waitSemaphoreCount = 1; si.pWaitSemaphores = &g_tl; si.pWaitDstStageMask = &wst; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &g_tl; }
    if (C->timed_reports) C->submitted_ns = guest_time_ns();
    VkResult r = vkQueueSubmit(g_q, 1, &si, C->fence); pthread_mutex_unlock(&g_qmu);
    if (r != VK_SUCCESS) die("Vulkan: submission failed (%d)", (int)r);
    pthread_mutex_lock(&g_mu); g_pend[g_npend++] = g_ci; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    return fence;
}
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (!fence) return 1;   // no submission yet
    pthread_mutex_lock(&g_mu);
    if (timeout_ns == UINT64_MAX) { while (g_done < fence) pthread_cond_wait(&g_cv, &g_mu); }
    else if (timeout_ns && g_done < fence) {
        struct timespec t;
        clock_gettime(g_cv_clock, &t);
        // Split before adding: even the largest finite NVN interval fits a
        // 64-bit time_t, but adding nanoseconds to it directly can wrap.
        const uint64_t second = 1000000000;
        uint64_t ns = t.tv_nsec + timeout_ns % second;
        t.tv_sec += (time_t)(timeout_ns / second + ns / second);
        t.tv_nsec = (long)(ns % second);
        while (g_done < fence) if (pthread_cond_timedwait(&g_cv, &g_mu, &t)) break;
    }
    int ok = g_done >= fence; pthread_mutex_unlock(&g_mu); return ok;
}
void mtl_finish(void) { mtl_wait(g_fence, UINT64_MAX); }
void mtl_drain(void) { mtl_finish(); }

// ---------- Presentation: counted; DM_VK_SHOTS writes chosen frames as BMP files ----------
static void shot(const tex_t *t, uint64_t n) {
    static char *list; static const char *dir; static int init;
    if (!init) { init = 1; const char *e = getenv("DM_VK_SHOTS"); list = e ? strdup(e) : NULL; dir = getenv("DM_VK_SHOT_DIR"); if (!dir) dir = "."; }
    if (!list) return;
    char key[24]; snprintf(key, sizeof key, ",%llu,", (unsigned long long)n); char hay[1024]; snprintf(hay, sizeof hay, ",%s,", list);
    if (!strstr(hay, key)) return;
    mtl_finish();
    pthread_mutex_lock(&g_rmu); vtex *v = tex_find(t->serial); pthread_mutex_unlock(&g_rmu);
    if (!v || (v->f.f != VK_FORMAT_R8G8B8A8_UNORM && v->f.f != VK_FORMAT_R8G8B8A8_SRGB && v->f.f != VK_FORMAT_B8G8R8A8_UNORM)) { LOG("[RED] vk: frame %llu: the presented texture is not an 8-bit RGBA image (%s)", (unsigned long long)n, v ? "format" : "never drawn"); return; }
    uint32_t w = v->w, h = v->h; VkDeviceMemory m; void *px;
    VkBuffer b = buf_new((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m, &px);
    VkCommandBuffer cb = once_begin(); barrier_all(cb);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c); once_end(cb);
    char path[1024]; snprintf(path, sizeof path, "%s/frame%05llu.bmp", dir, (unsigned long long)n);
    FILE *f = fopen(path, "wb");
    if (f) {   // 32-bit BMP, bottom-up rows, BGRA
        uint32_t sz = 54 + w * h * 4; uint8_t hd[54] = {'B', 'M'}; memcpy(hd + 2, &sz, 4); uint32_t o = 54, hs = 40; memcpy(hd + 10, &o, 4); memcpy(hd + 14, &hs, 4);
        memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4); hd[26] = 1; hd[28] = 32;
        fwrite(hd, 1, 54, f);
        uint8_t *row = malloc((size_t)w * 4);
        for (uint32_t y = h; y-- > 0;) {
            const uint8_t *s = (const uint8_t *)px + (size_t)(g_origin_ll ? h - 1 - y : y) * w * 4;
            for (uint32_t x = 0; x < w; x++) { int bgr = v->f.f == VK_FORMAT_B8G8R8A8_UNORM; row[4 * x] = s[4 * x + (bgr ? 0 : 2)]; row[4 * x + 1] = s[4 * x + 1]; row[4 * x + 2] = s[4 * x + (bgr ? 2 : 0)]; row[4 * x + 3] = 255; }
            fwrite(row, 1, (size_t)w * 4, f);
        }
        free(row); fclose(f); LOG("vk: frame %llu written to %s (%ux%u; %llu draws so far, %llu dropped)", (unsigned long long)n, path, w, h, (unsigned long long)g_stat_draws, (unsigned long long)g_stat_skip);
    }
    vkDestroyBuffer(g_dev, b, NULL); vkFreeMemory(g_dev, m, NULL);
}
// Diagnostics: DM_VK_RTDUMP=<frame> writes every color image the GPU rendered into, as it stands at that frame's present, to DM_VK_SHOT_DIR as
// rt_<serial>_<w>x<h>_<VkFormat>.raw (level 0, layer 0, tightly packed), for comparing a frame's passes with the Mac's; DM_VK_RTDUMP_DEC=1 adds the textures decoded
// from BC6H on upload. DM_VK_PASSLOG=<frame> logs that frame's passes (targets, clears) and draws (shaders, depth / stencil / blend state)
static int rt_bpp(VkFormat format) {
    // Core Vulkan numeric families have consecutive format enums, with each
    // family's normalized, scaled, integer and floating variants the same size.
    static const struct { VkFormat first,last; int bytes; } families[]={
        {VK_FORMAT_R8_UNORM,VK_FORMAT_R8_SRGB,1},
        {VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8_SRGB,2},
        {VK_FORMAT_R8G8B8_UNORM,VK_FORMAT_B8G8R8_SRGB,3},
        {VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_A2B10G10R10_SINT_PACK32,4},
        {VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SFLOAT,2},
        {VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16_SFLOAT,4},
        {VK_FORMAT_R16G16B16_UNORM,VK_FORMAT_R16G16B16_SFLOAT,6},
        {VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT,8},
        {VK_FORMAT_R32_UINT,VK_FORMAT_R32_SFLOAT,4},
        {VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32_SFLOAT,8},
        {VK_FORMAT_R32G32B32_UINT,VK_FORMAT_R32G32B32_SFLOAT,12},
        {VK_FORMAT_R32G32B32A32_UINT,VK_FORMAT_R32G32B32A32_SFLOAT,16},
    };
    for (unsigned i=0;i<sizeof families/sizeof *families;i++)
        if (format>=families[i].first && format<=families[i].last) return families[i].bytes;
    switch (format) {
        case VK_FORMAT_R4G4_UNORM_PACK8: case VK_FORMAT_S8_UINT: return 1;
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
        case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_A4B4G4R4_UNORM_PACK16:
        case VK_FORMAT_D16_UNORM: return 2;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return 4;
        default: return 0;
    }
}
static void rt_dump(uint64_t n) {
    static int64_t at = -2, cnt = 1; if (at == -2) { const char *e = getenv("DM_VK_RTDUMP"); at = e ? atoll(e) : -1; const char *c = e ? strchr(e, ',') : NULL; if (c) cnt = atoll(c + 1); }   // <frame>[,<frames>]
    if ((int64_t)n < at || (int64_t)n >= at + cnt) return;
    const char *base = getenv("DM_VK_SHOT_DIR"); if (!base) base = ".";
    char dir[512]; if (cnt > 1) { snprintf(dir, sizeof dir, "%s/f%llu", base, (unsigned long long)n); mkdir(dir, 0777); chmod(dir, 0777); } else snprintf(dir, sizeof dir, "%s", base);
    mtl_finish(); int nd = 0;
    pthread_mutex_lock(&g_rmu);
    for (int hsh = 0; hsh < NHASH; hsh++) for (vtex *v = g_tex[hsh]; v; v = v->next) {
        int bpp = rt_bpp(v->f.f); if (!(v->gpu || (getenv("DM_VK_RTDUMP_DEC") && v->f.dec >= DEC_BC4)) || !bpp || !(v->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) continue;
        VkDeviceMemory m; void *px; VkDeviceSize sz = (VkDeviceSize)v->w * v->h * bpp;
        VkBuffer b = buf_new(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m, &px);
        VkCommandBuffer cb = once_begin(); barrier_all(cb);
        VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {v->w, v->h, 1}};
        vkCmdCopyImageToBuffer(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c); once_end(cb);
        char path[1024]; snprintf(path, sizeof path, "%s/rt_%llu_%ux%u_%d.raw", dir, (unsigned long long)v->serial, v->w, v->h, (int)v->f.f);
        FILE *f = fopen(path, "wb"); if (f) { fwrite(px, 1, (size_t)sz, f); fclose(f); chmod(path, 0644); nd++; }   // readable by adb (an app's files are 0600)
        vkDestroyBuffer(g_dev, b, NULL); vkFreeMemory(g_dev, m, NULL);
    }
    pthread_mutex_unlock(&g_rmu);
    LOG("vk: frame %llu: %d render targets written to %s", (unsigned long long)n, nd, dir);
}
static double g_fps_shown;
static pthread_mutex_t g_present_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_presents, g_present_done, g_dropped_ticket[64];
#include "diagnostics.inc"

static _Atomic uint64_t g_layer_shape, g_screen_shape;
void mtl_present_layer(uint32_t w, uint32_t h) { atomic_store(&g_layer_shape, vk_present_shape(w, h)); }
void mtl_present_screen(uint32_t w, uint32_t h) { atomic_store(&g_screen_shape, vk_present_shape(w, h)); }

// ---------- Android window ----------
#ifdef __ANDROID__
__attribute__((weak)) void *front_native_window(int *gen);   // app/front_android.c; absent in the headless front end
__attribute__((weak)) void front_window_lock(int on) { (void)on; }
typedef struct { VkImage image; VkImageView view; VkSemaphore ready; } window_image;
static struct {
    VkSurfaceKHR surf; VkSwapchainKHR sc; window_image *images; uint32_t n;
    VkFormat fmt; VkExtent2D ext; int gen; VkPipeline pipeline;
} W;
#define NPRES 3
typedef struct {
    VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkSemaphore acq;
    VkFramebuffer fb; VkDescriptorSet set; vtex *source;
} present_slot;
static present_slot g_pres[NPRES]; static int g_pi;
#include "window_draw.inc"
static void win_drop(void) {
    if (W.sc) {
        pthread_mutex_lock(&g_qmu); vkDeviceWaitIdle(g_dev); pthread_mutex_unlock(&g_qmu);
        for (int i = 0; i < NPRES; i++) window_slot_retire(&g_pres[i]);
        vkDestroyPipeline(g_dev, W.pipeline, NULL); W.pipeline = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < W.n; i++) {
            vkDestroyImageView(g_dev, W.images[i].view, NULL);
            vkDestroySemaphore(g_dev, W.images[i].ready, NULL);
        }
        free(W.images); W.images = NULL; W.n = 0;
        vkDestroySwapchainKHR(g_dev, W.sc, NULL); W.sc = VK_NULL_HANDLE;
    }
    if (W.surf) { vkDestroySurfaceKHR(g_inst, W.surf, NULL); W.surf = VK_NULL_HANDLE; }
}
static int win_make(ANativeWindow *aw) {
    VkAndroidSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, aw};
    if (vkCreateAndroidSurfaceKHR(g_inst, &si, NULL, &W.surf) != VK_SUCCESS) { RED_ONCE(90, "no Vulkan surface for the window"); return 0; }
    VkBool32 ok = 0;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qf, W.surf, &ok) != VK_SUCCESS || !ok) {
        RED_ONCE(91, "the queue cannot present to the window"); return 0;
    }
    VkSurfaceCapabilitiesKHR cap;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, W.surf, &cap) != VK_SUCCESS ||
        !(cap.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || !cap.supportedCompositeAlpha) return 0;
    uint32_t nf = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, W.surf, &nf, NULL) != VK_SUCCESS || !nf) return 0;
    VkSurfaceFormatKHR *formats = calloc(nf, sizeof *formats);
    if (!formats) return 0;
    VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, W.surf, &nf, formats);
    if (result != VK_SUCCESS || !nf) { free(formats); return 0; }
    VkSurfaceFormatKHR pick = formats[0];
    for (uint32_t i = 0; i < nf; i++) {
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = formats[i]; break; }
    }
    free(formats);
    if (pick.format == VK_FORMAT_UNDEFINED) pick.format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t npm = 0;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, W.surf, &npm, NULL) != VK_SUCCESS || !npm) return 0;
    VkPresentModeKHR *modes = calloc(npm, sizeof *modes);
    if (!modes) return 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, W.surf, &npm, modes);
    if (result != VK_SUCCESS || !npm) { free(modes); return 0; }
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < npm; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    free(modes);
    // The swapchain is made the window's way up and Android's compositor turns it (a phone whose screen is portrait by nature, playing in landscape, reports its
    // current extent and transform rotated); the window's own size is that way up
    VkSurfaceTransformFlagBitsKHR pre = cap.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : cap.currentTransform;
    W.ext = (VkExtent2D){(uint32_t)ANativeWindow_getWidth(aw), (uint32_t)ANativeWindow_getHeight(aw)};
    if (pre == cap.currentTransform && cap.currentExtent.width != UINT32_MAX) W.ext = cap.currentExtent;
    if (W.ext.width < cap.minImageExtent.width) W.ext.width = cap.minImageExtent.width; if (W.ext.width > cap.maxImageExtent.width) W.ext.width = cap.maxImageExtent.width;
    if (W.ext.height < cap.minImageExtent.height) W.ext.height = cap.minImageExtent.height; if (W.ext.height > cap.maxImageExtent.height) W.ext.height = cap.maxImageExtent.height;
    uint32_t nimg = cap.minImageCount + 1; if (cap.maxImageCount && nimg > cap.maxImageCount) nimg = cap.maxImageCount;
    VkCompositeAlphaFlagBitsKHR ca = cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR :
                                      cap.supportedCompositeAlpha & -cap.supportedCompositeAlpha;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, W.surf, nimg, pick.format, pick.colorSpace, W.ext, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, pre, ca, mode, VK_TRUE, VK_NULL_HANDLE};
    if (vkCreateSwapchainKHR(g_dev, &ci, NULL, &W.sc) != VK_SUCCESS) { RED_ONCE(92, "no swapchain for the window"); return 0; }
    W.fmt = pick.format;
    uint32_t count = 0;
    if (vkGetSwapchainImagesKHR(g_dev, W.sc, &count, NULL) != VK_SUCCESS || !count) return 0;
    VkImage *handles = calloc(count, sizeof *handles);
    if (!handles) return 0;
    result = vkGetSwapchainImagesKHR(g_dev, W.sc, &count, handles);
    if (result != VK_SUCCESS || !count) { free(handles); return 0; }
    W.images = calloc(count, sizeof *W.images);
    if (!W.images) { free(handles); return 0; }
    W.n = count;
    for (uint32_t i = 0; i < count; i++) W.images[i].image = handles[i];
    free(handles);
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, W.images[i].image, VK_IMAGE_VIEW_TYPE_2D,
                                    W.fmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        // A graphics fence can signal before the presentation engine consumes
        // its semaphore. Acquiring this same image again orders that reuse.
        if (vkCreateImageView(g_dev, &vi, NULL, &W.images[i].view) != VK_SUCCESS ||
            vkCreateSemaphore(g_dev, &sem, NULL, &W.images[i].ready) != VK_SUCCESS) return 0;
    }
    if (!g_pres[0].pool) for (int i = 0; i < NPRES; i++) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qf}; VKCHECK(vkCreateCommandPool(g_dev, &pi, NULL, &g_pres[i].pool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_pres[i].pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1}; VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, &g_pres[i].cb));
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT}; VKCHECK(vkCreateFence(g_dev, &fi, NULL, &g_pres[i].fence));
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKCHECK(vkCreateSemaphore(g_dev, &sci, NULL, &g_pres[i].acq));
    }
    LOG("vk: swapchain %ux%u, %u images, format %d, %s", W.ext.width, W.ext.height, W.n, W.fmt, mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    return 1;
}
// Over the picture: the frame-rate counter and the on-screen controls (front_overlay), drawn as cleared rectangles -- a 3x5 font, button outlines, a filled button while pressed
typedef struct { float x, y, w, h; int on; char label[3]; } dm_ovl;
__attribute__((weak)) int front_overlay(dm_ovl *out, int max) { (void)out; (void)max; return 0; }
static uint16_t glyph(char c) {   // rows top to bottom, 3 bits each
    static const char *const G[] = {"0111101101101111", "1010010010010010", "2111001111100111", "3111001111001111", "4101101111001001", "5111100111001111", "6111100111101111",
                                    "7111001001001001", "8111101111101111", "9111101111001111", "A010101111101101", "B110101110101110", "X101101010101101", "Y101101010010010",
                                    "L100100100100111", "R110101110101101", "Z111001010100111", "+000010111010000", "-000000111000000"};
    for (size_t i = 0; i < sizeof G / sizeof *G; i++) if (G[i][0] == c) { uint16_t g = 0; for (int k = 1; k <= 15; k++) g = (uint16_t)(g << 1 | (G[i][k] == '1')); return g; }
    return 0;
}
static uint32_t text_rects(VkClearRect *r, uint32_t nr, uint32_t max, const char *t, int32_t x0, int32_t y0, uint32_t cell) {
    for (size_t d = 0; t[d] && nr + 15 <= max; d++) { uint16_t g = glyph(t[d]);
        for (int row = 0; row < 5; row++) for (int c = 0; c < 3; c++) if (g >> (14 - 3 * row - c) & 1)
            r[nr++] = (VkClearRect){{{x0 + (int32_t)((d * 4 + c) * cell), y0 + (int32_t)(row * cell)}, {cell, cell}}, 0, 1}; }
    return nr;
}
static void overlay_draw(VkCommandBuffer cb, int show_fps, const dm_ovl *ov, int nov) {
    enum { MAXR = 1024 }; static VkClearRect lines[MAXR], fills[MAXR], fg[MAXR], bg[64]; uint32_t nl = 0, nf = 0, ng = 0, nb = 0;
    #define CLIP(x0, y0, w_, h_) do { int32_t X = (int32_t)(x0), Y = (int32_t)(y0), XW = X + (int32_t)(w_), YH = Y + (int32_t)(h_); if (X < 0) X = 0; if (Y < 0) Y = 0; \
        if (XW > (int32_t)W.ext.width) XW = (int32_t)W.ext.width; if (YH > (int32_t)W.ext.height) YH = (int32_t)W.ext.height; \
        if (XW > X && YH > Y) rect = (VkClearRect){{{X, Y}, {(uint32_t)(XW - X), (uint32_t)(YH - Y)}}, 0, 1}; else rect.rect.extent.width = 0; } while (0)
    VkClearRect rect;
    if (show_fps) {
        char t[8]; snprintf(t, sizeof t, "%.0f", g_fps_shown); uint32_t cell = W.ext.height / 120 ? W.ext.height / 120 : 2;
        CLIP(cell * 2, cell * 2, (strlen(t) * 4 + 1) * cell, 7 * cell); if (rect.rect.extent.width) bg[nb++] = rect;
        ng = text_rects(fg, ng, MAXR, t, (int32_t)(cell * 3), (int32_t)(cell * 3), cell);
    }
    for (int i = 0; i < nov && nl + 4 <= MAXR; i++) {
        const dm_ovl *o = &ov[i]; float t = o->w / 18 > 2 ? o->w / 18 : 2;
        if (o->on) { CLIP(o->x, o->y, o->w, o->h); if (rect.rect.extent.width && nf < MAXR) fills[nf++] = rect; }
        CLIP(o->x, o->y, o->w, t); if (rect.rect.extent.width) lines[nl++] = rect;
        CLIP(o->x, o->y + o->h - t, o->w, t); if (rect.rect.extent.width) lines[nl++] = rect;
        CLIP(o->x, o->y, t, o->h); if (rect.rect.extent.width) lines[nl++] = rect;
        CLIP(o->x + o->w - t, o->y, t, o->h); if (rect.rect.extent.width) lines[nl++] = rect;
        if (o->label[0]) { uint32_t cell = (uint32_t)(o->h / 9) ? (uint32_t)(o->h / 9) : 1, tw = (uint32_t)strlen(o->label) * 4 * cell - cell;
                           ng = text_rects(fg, ng, MAXR, o->label, (int32_t)(o->x + (o->w - tw) / 2), (int32_t)(o->y + (o->h - 5 * cell) / 2), cell); }
    }
    #undef CLIP
    VkClearAttachment black = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0, 0, 0, 1}}}}, white = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.85f, 0.85f, 0.9f, 1}}}},
                      grey = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.35f, 0.35f, 0.45f, 1}}}}, green = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.3f, 1, 0.3f, 1}}}};
    if (nb) vkCmdClearAttachments(cb, 1, &black, nb, bg);
    if (nf) vkCmdClearAttachments(cb, 1, &grey, nf, fills);
    if (nl) vkCmdClearAttachments(cb, 1, &white, nl, lines);
    if (ng) vkCmdClearAttachments(cb, 1, nov ? &white : &green, ng, fg);
}
static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static int win_present(const tex_t *t, const int crop[4]) {
    int gen;
    front_window_lock(1);
    ANativeWindow *aw = front_native_window(&gen);
    if (!aw || !W.sc || gen != W.gen) {
        win_drop(); W.gen = gen;
        if (aw && !win_make(aw)) win_drop();  // Retire partial setup and retry on the next frame.
    }
    if (!aw || !W.sc) { front_window_lock(0); return 0; }
    present_slot *P_ = &g_pres[g_pi]; g_pi = (g_pi + 1) % NPRES;
    { uint64_t w0 = now_ns(); vkWaitForFences(g_dev, 1, &P_->fence, VK_TRUE, UINT64_MAX); atomic_fetch_add(&g_pwait_ns, now_ns() - w0); }
    window_slot_retire(P_);
    copy_draw_params region = {.linear = 2};  // Clamp the linear footprint to the VI crop.
    if (!window_pipeline() || !window_prepare(P_, t, crop, &region)) { front_window_lock(0); return 0; }
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(g_dev, W.sc, 1000000000ull, P_->acq, VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS && r != VK_TIMEOUT && r != VK_NOT_READY) W.gen = -1;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { window_slot_retire(P_); front_window_lock(0); return 0; }
    vkResetFences(g_dev, 1, &P_->fence);
    VkCommandBuffer cb = P_->cb; vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL}; vkBeginCommandBuffer(cb, &bi);
    barrier_all(cb);   // the game's submissions before this one wrote the image
    img_barrier(cb, W.images[idx].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rp_desc pass = {.n = 1, .cv = {W.images[idx].view}, .cf = {W.fmt}, .cl = {VK_ATTACHMENT_LOAD_OP_CLEAR},
                    .cc[0].float32 = {0, 0, 0, 1}, .w = W.ext.width, .h = W.ext.height, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    P_->fb = rp_begin(cb, &pass);
    VkRect2D rect = {{region.dst[0], region.dst[1]}, {region.dst[2], region.dst[3]}};
    internal_draw_state(cb, W.pipeline, rect, W.ext, 0);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, window_layout, 0, 1, &P_->set, 0, NULL);
    vkCmdPushConstants(cb, window_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof region, &region);
    vkCmdDraw(cb, 3, 1, 0, 0);
    static int show = -1; if (show < 0) { const char *e = getenv("DM_SHOW_FPS"); show = e && atoi(e); }
    static dm_ovl ov[32]; int nov = front_overlay(ov, 32);
    if (show || nov) overlay_draw(cb, show, ov, nov);
    rp_end(cb);
    img_barrier(cb, W.images[idx].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags ws[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkSemaphore wsem[2] = {P_->acq, g_tl}, ssem[2] = {W.images[idx].ready, g_tl}; uint64_t wv[2] = {0, 0}, sv[2] = {0, 0};
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 2, wv, 2, sv};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, g_tl ? &ts : NULL, g_tl ? 2 : 1, wsem, ws, 1, &cb, g_tl ? 2 : 1, ssem};
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &W.images[idx].ready, 1, &W.sc, &idx, NULL};
    pthread_mutex_lock(&g_qmu);
    if (g_tl) { wv[1] = g_tlv; sv[1] = ++g_tlv; }
    VKCHECK(vkQueueSubmit(g_q, 1, &si, P_->fence)); r = vkQueuePresentKHR(g_q, &pi); pthread_mutex_unlock(&g_qmu);
    if (r != VK_SUCCESS) W.gen = -1;
    front_window_lock(0);
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}
#endif

uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    pthread_mutex_lock(&g_present_mu);
    uint64_t n = atomic_fetch_add(&g_presents, 1) + 1;
    {   // pacing for DM_FPS=30 (every frame two vsyncs) and the frame-rate counter's reading
        static uint64_t last, t0, n0; uint64_t now = now_ns();
        if (mtl_fps_target() == 30 && last && now - last < 33333333ull) { usleep((useconds_t)((33333333ull - (now - last)) / 1000)); now = now_ns(); }
        last = now; if (!t0) { t0 = now; n0 = n; } else if (now - t0 >= 1000000000ull) { g_fps_shown = (double)(n - n0) * 1e9 / (double)(now - t0); t0 = now; n0 = n; }
    }
    if (t && g_has_surface && !g_tl) mtl_finish();   // no timeline semaphore: the frame's work is done before its present reads the window texture
    int handed_on = 1;   // The headless front end completes the handoff here.
#ifdef __ANDROID__
    if (front_native_window) handed_on = t && g_has_surface && win_present(t, crop);
#endif
    atomic_store(&g_dropped_ticket[n % 64], handed_on ? 0 : n);
    atomic_store(&g_present_done, n);
    if (n == 1 || n % 600 == 0) LOG("vk: %llu frames presented, %llu draws, %llu dropped (no program %llu, shader %llu, no target %llu, indirect %llu, primitive %llu, pipeline %llu, descriptors %llu), %d pipelines, %d shaders",
                                    (unsigned long long)n, (unsigned long long)g_stat_draws, (unsigned long long)g_stat_skip, (unsigned long long)g_skip_why[0], (unsigned long long)g_skip_why[1], (unsigned long long)g_skip_why[2],
                                    (unsigned long long)g_skip_why[3], (unsigned long long)g_skip_why[4], (unsigned long long)g_skip_why[5], (unsigned long long)g_skip_why[6], g_npso, g_nsh);
    if (n % 600 == 0 && getenv("DM_VK_TEXCOUNT")) {   // diagnostics: live textures by kind, and how many share their storage start with an older one
        int ng = 0, nu = 0, nn = 0, dup = 0; pthread_mutex_lock(&g_rmu);
        for (int h = 0; h < NHASH; h++) for (vtex *v = g_tex[h]; v; v = v->next) {
            if (v->gpu) ng++; else if (v->uploaded) nu++; else nn++;
            for (int h2 = 0; h2 < NHASH && v->mem; h2++) for (vtex *o = g_tex[h2]; o; o = o->next) if (o != v && o->mem == v->mem && o->serial < v->serial && o->w == v->w && o->nvn_fmt == v->nvn_fmt) { dup++; h2 = NHASH; break; }
        }
        pthread_mutex_unlock(&g_rmu);
        LOG("vk: live textures: %d rendered to, %d uploaded, %d neither; %d on the storage of an older one of the same size and format", ng, nu, nn, dup);
    }
    if (n % 600 == 0) { uint64_t live = 0, held = 0; int nb = 0; pthread_mutex_lock(&g_blkmu); for (int i = 0; i < g_nblk; i++) if (g_blk[i].m) { nb++; held += g_blk[i].size; live += g_blk[i].live; } pthread_mutex_unlock(&g_blkmu);
                        LOG("vk: image memory: %d blocks of 64 MB (%.0f MB, %llu live images in them), big images %.0f MB now; rings %.0f MB extra", nb, held / 1048576.0, (unsigned long long)live, atomic_load(&g_big_bytes) / 1048576.0, atomic_load(&g_extra_ring_bytes) / 1048576.0); }
    if (n % 600 == 0 && g_fb_copies) LOG("vk: same-pass feedback snapshots so far: %llu", (unsigned long long)g_fb_copies);
    if (n % 600 == 0) LOG("vk: texture slots read as black so far: no handle %llu, handle not registered %llu, texture not created %llu, no view %llu", (unsigned long long)g_tex_miss[0],
                          (unsigned long long)g_tex_miss[1], (unsigned long long)g_tex_miss[2], (unsigned long long)g_tex_miss[3]);
    if (n % 600 == 0) { LOG("vk: per frame over the last 600: GPU %.2f ms, recording %.2f ms, waiting for the GPU %.2f ms, present waiting %.2f ms", atomic_exchange(&g_gpu_ns, 0) / 600e6,
                            atomic_exchange(&g_rec_ns, 0) / 600e6, atomic_exchange(&g_wait_ns, 0) / 600e6, atomic_exchange(&g_pwait_ns, 0) / 600e6); }
    if (t) shot(t, n);
    rt_dump(n);
    diagnostic_present(t,n,crop);
    if (n % 1800 == 0) pcache_save();
    { static int64_t pl = -2; static int on; if (pl == -2) { const char *e = getenv("DM_VK_PASSLOG"); pl = e ? atoll(e) : -1; on = e && strchr(e, '+'); }   // <frame>+: every frame from there on
      long request = atomic_exchange(&g_passdump_request, 0);
      if (request) g_passdump_left = request;
      g_plog = (on ? (int64_t)n + 1 >= pl && pl >= 0 : (int64_t)n + 1 == pl) || g_passdump_left > 0;
      if (g_plog && (on || g_passdump_left)) LOG("plog: frame %llu", (unsigned long long)n + 1);
      if (g_passdump_left > 0) g_passdump_left--; }
    pthread_mutex_unlock(&g_present_mu);
    return n;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
void mtl_present_progress(uint64_t *queued, uint64_t *done) {
    *done = atomic_load(&g_present_done);
    *queued = atomic_load(&g_presents);
}
double mtl_presented_time(uint64_t n) {
    // VI supplies the first-observed completion tick when WSI has no display timestamp.
    return n && atomic_load(&g_dropped_ticket[n % 64]) == n ? -1 : 0;
}
int mtl_has_window(void) {
#ifdef __ANDROID__
    if (front_native_window) {
        int generation;
        front_window_lock(1);
        int live = front_native_window(&generation) != NULL;
        front_window_lock(0);
        return live;
    }
#endif
    return 0;
}
int mtl_display_hz(void) { return 0; }
int mtl_fps_target(void) { static int f = -1; if (f < 0) { const char *e = getenv("DM_FPS"); f = e && atoi(e) == 30 ? 30 : 60; } return f; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
// Android uses a Vulkan mailbox swapchain when available, otherwise FIFO. There is no Apple display-link clock
// or Metal snapshot ring; the recorder uses its existing fallback pacing path.
int mtl_link_stalled(void) { return 0; }
int mtl_link_rate(void) { return 0; }
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) { (void)ticks; (void)timeout_ns; return 0; }
int mtl_link_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) {
    (void)n; (void)target; (void)lead_ms; (void)ticks; (void)gap_ms; return 0;
}
int mtl_snap_on(void) { return 0; }
uint64_t mtl_snap_acquire_wait(void) { return 0; }
int mtl_probe_ramp_on(void) { return 0; }
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
void mtl_prepare(void) { mtl_init(); }
// Shader/pipeline loading remains demand-driven, under the submission lock.
void mtl_prog_prefetch(const prog_t *p) { (void)p; }
uint32_t mtl_present_qos(void) { return 0; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_set_depth_zero(int on) {   // vertex shaders loaded from here on follow it (spv_gl_depth); the games set it once at device init, before their first shader
    if (on != g_depth_zero && g_nsh) LOG("[RED] vk: depth mode changed to %s after %d shaders were loaded: those keep the other convention", on ? "near = 0" : "GL (near = -w)", g_nsh);
    g_depth_zero = on;
}
void mtl_set_origin_lower_left(int on) { g_origin_ll = on; }
void mtl_set_queue_origin_lower_left(int on) { g_queue_origin_ll = on != 0; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    texel_extent ext;
    if (!texel_region(t, level, layer0, r, row, img, &ext)) {
        RED_ONCE(80, "invalid region for CPU texture read"); return;
    }
    pthread_mutex_lock(&g_rmu);
    vtex *v = tex_find(t->serial);
    int gpu = v && v->gpu;
    int ok = gpu && gpu_texels(v, level, r, NULL, dst, row, img, ext);
    pthread_mutex_unlock(&g_rmu);
    if (!gpu) ok = storage_texels(t, level, dst, row, img, ext, 0);
    if (!ok) RED_ONCE(80, "unsupported CPU texture read (format %#x, samples %u)", t->d.format, t->d.samples);
}

// Guest extents for checked address lookup and open-ended storage bindings.

// Raw shader addresses can reach any pool from which the guest took an address.
// Keep that intent when the address is taken before the pool is registered.
void mtl_pool_addressed(const void *mem, uint64_t size) {
    if (!size || size>UINT64_MAX-(uintptr_t)mem) return;
    pthread_mutex_lock(&g_pool_mu);
    int known=0;
    for (size_t i=0;i<g_naddressed;i++) if (g_addressed[i].mem==mem && g_addressed[i].size==size) known=1;
    if (!known) {
        pool_range *next=realloc(g_addressed,(g_naddressed+1)*sizeof *next);
        if (!next) die("Vulkan: cannot track addressed pools");
        g_addressed=next;g_addressed[g_naddressed++]=(pool_range){mem,size,1};
    }
    for (size_t i=0;i<g_npool;i++) if (overlaps((uintptr_t)mem,size,(uintptr_t)g_pool[i].mem,g_pool[i].size)) g_pool[i].addressed=1;
    pthread_mutex_unlock(&g_pool_mu);
}
void mtl_pool_add(const void *mem, uint64_t size) {
    if (size > UINT64_MAX - (uintptr_t)mem) die("Vulkan: memory pool address overflow");
    pthread_mutex_lock(&g_pool_mu);
    if (g_npool == g_cpool) {
        if (g_cpool > SIZE_MAX / 2 / sizeof *g_pool) die("Vulkan: memory pool table size overflow");
        size_t capacity = g_cpool ? g_cpool * 2 : 64;
        pool_range *next = realloc(g_pool, capacity * sizeof *next);
        if (!next) die("Vulkan: cannot track memory pools");
        g_pool = next; g_cpool = capacity;
    }
    int addressed=0;
    for (size_t i=0;i<g_naddressed;i++) if (overlaps((uintptr_t)mem,size,(uintptr_t)g_addressed[i].mem,g_addressed[i].size)) addressed=1;
    g_pool[g_npool++] = (pool_range){mem,size,addressed};
    pthread_mutex_unlock(&g_pool_mu);
}
void mtl_pool_del(const void *mem) { pthread_mutex_lock(&g_pool_mu); for (size_t i = 0; i < g_npool; i++) if (g_pool[i].mem == mem) { g_pool[i] = g_pool[--g_npool]; break; } pthread_mutex_unlock(&g_pool_mu); }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int hit = 0; pthread_mutex_lock(&g_pool_mu);
    for (size_t i = 0; i < g_npool && !hit; i++) { uint64_t b = (uintptr_t)g_pool[i].mem; if (addr >= b && addr - b <= g_pool[i].size && len <= g_pool[i].size - (addr - b)) { if (buf) *buf = (void *)g_pool[i].mem; if (off) *off = addr - b; hit = 1; } }
    pthread_mutex_unlock(&g_pool_mu); return hit;
}
static uint64_t pool_remaining(uint64_t addr) {
    uint64_t size = 0; pthread_mutex_lock(&g_pool_mu);
    for (size_t i = 0; i < g_npool && !size; i++) {
        uint64_t b = (uintptr_t)g_pool[i].mem;
        if (addr >= b && addr - b < g_pool[i].size) size = g_pool[i].size - (addr - b);
    }
    pthread_mutex_unlock(&g_pool_mu); return size;
}
void mtl_alias_drop_pool(const void *pool) {
    if (!pool) return;
    pthread_mutex_lock(&g_rmu);
    // Match Metal's treg_drop_pool_views: a copied ID must never dereference
    // this retired pool, even after its object address is reused. RegisterTexture
    // or RegisterImage explicitly makes the ID live again. Encoded Vulkan views
    // retain their normal deferred resource lifetime through the submit fence.
    for (int i = 0; i < g_ntreg; i++) if (g_treg[i].ok && g_treg[i].t.pool == pool) g_treg[i].ok = 0;
    g_registration_generation++;
    pthread_mutex_unlock(&g_rmu);
}
// This backend has no shared sparse-tile heap to release. Sparse residency and
// physical-tile aliasing remain open parity items; ordinary images own storage.
void mtl_sp_phys_gone(const void *mem, uint64_t size) { (void)mem; (void)size; }
// Vulkan transfers image ownership when each recorded write executes.
// Recording a future write must not suppress uploads before that write.
void mtl_tex_gpu_written(const tex_t *t) { (void)t; }

// statistics the NVN layer prints
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = 0; *draws = g_stat_draws; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) {
    *resolve = *reuse = *set = *skip = 0;
    *copies = atomic_load(&g_vbuf_copies); *copy_bytes = atomic_load(&g_vbuf_copy_bytes);
}
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_ubp_why(uint64_t w[5]) { memset(w, 0, 5 * sizeof *w); }
void mtl_ubp_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_ubd_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_res_stats(char *out, size_t n) { if (n) snprintf(out, n, "vulkan"); }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_mem_stat(unsigned long long a, unsigned long long b) { (void)a; (void)b; }
void mtl_lc_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; (void)t0; (void)t1; return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_cut_count(void) { return 0; }
uint64_t mtl_blit_splits(void) { return 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = 0; *pso = (uint64_t)g_npso; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = *full = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_alloc_stats(uint64_t *w, uint64_t *g, int *n) { *w = *g = 0; *n = 0; }

// These counters describe Metal-only optimizations, never used by this backend.
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { *draws = *made = *none = 0; }
void mtl_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { (void)f; (void)gpu_ms; (void)frame_ms; }
uint64_t mtl_pkg_edge_skips(void) { return 0; }
void mtl_pso_line(char *out, size_t n) { snprintf(out, n, "Vulkan graphics %d, shaders %d", g_npso, g_nsh); }
unsigned mtl_cpu_tex_count(void) {
    unsigned n = 0; pthread_mutex_lock(&g_rmu);
    for (int h = 0; h < NHASH; h++) for (vtex *v = g_tex[h]; v; v = v->next) if (v->uploaded && !v->gpu) n++;
    pthread_mutex_unlock(&g_rmu); return n;
}

#include "capture.inc"
void dm_gfx_passdump_next(long n) { if (n > 0) atomic_store(&g_passdump_request, n); }
void dm_gfx_restop(void) { LOG("vk: no Metal residency set; Vulkan image-memory totals are logged every 600 presents"); }
