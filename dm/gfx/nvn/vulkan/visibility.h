// Query pools belong to an in-flight Vulkan submission. The accumulated
// samples belong to an NVN queue and can outlive its last submitted draw.
#pragma once

typedef struct { _Atomic unsigned refs; uint64_t samples; } visibility_counter;
static visibility_counter *visibility_retain(visibility_counter *counter) {
    if (counter) atomic_fetch_add(&counter->refs, 1);
    return counter;
}
static void visibility_release(visibility_counter *counter) {
    if (counter && atomic_fetch_sub(&counter->refs, 1) == 1) free(counter);
}
enum { VIS_PAGE_SIZE = 256 };
typedef struct visibility_page {
    VkQueryPool pool;
    uint32_t used;
    uint64_t samples[VIS_PAGE_SIZE];
    struct visibility_page *next;
} visibility_page;
typedef struct { visibility_page *first, *last; int active; } visibility_queries;

// The upload command buffer executes before the render commands. Reset each
// page there once per reuse, including pages allocated inside a render pass.
static const uint64_t *visibility_begin(visibility_queries *queries, VkCommandBuffer cb, VkCommandBuffer upload, int precise) {
    if (queries->active) return NULL;
    visibility_page *page = queries->last;
    if (!page || page->used == VIS_PAGE_SIZE) {
        visibility_page **next = page ? &page->next : &queries->first;
        if (!*next) {
            *next = calloc(1, sizeof **next);
            if (!*next) die("Vulkan: cannot allocate visibility query page");
            VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_OCCLUSION, VIS_PAGE_SIZE};
            VKCHECK(vkCreateQueryPool(g_dev, &info, NULL, &(*next)->pool));
        }
        page = queries->last = *next;
        page->used = 0;
        vkCmdResetQueryPool(upload, page->pool, 0, VIS_PAGE_SIZE);
    }
    uint32_t slot = page->used++;
    vkCmdBeginQuery(cb, page->pool, slot, precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    queries->active = 1;
    return &page->samples[slot];
}
static void visibility_end(visibility_queries *queries, VkCommandBuffer cb) {
    if (!queries->active) return;
    vkCmdEndQuery(cb, queries->last->pool, queries->last->used - 1);
    queries->active = 0;
}
static void visibility_read(visibility_queries *queries) {
    if (!queries->last) return;
    for (visibility_page *page = queries->first; ; page = page->next) {
        VKCHECK(vkGetQueryPoolResults(g_dev, page->pool, 0, page->used, sizeof page->samples, page->samples,
                                      sizeof *page->samples, VK_QUERY_RESULT_64_BIT));
        if (page == queries->last) break;
    }
    queries->last = NULL;
}
