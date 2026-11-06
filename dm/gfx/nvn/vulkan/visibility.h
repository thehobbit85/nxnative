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
enum { QUERY_PAGE_SIZE = 256 };
typedef struct query_page {
    VkQueryPool pool;
    uint32_t used;
    uint64_t samples[QUERY_PAGE_SIZE];
    struct query_page *next;
} query_page;
typedef struct { query_page *first, *last; } query_pages;
typedef struct { query_pages pages; int active; } visibility_queries;

// The upload command buffer executes before the render commands. Reset each
// page there once per reuse, including pages allocated inside a render pass.
static query_page *query_slot(query_pages *queries, VkQueryType type, VkCommandBuffer upload, uint32_t *slot) {
    query_page *page = queries->last;
    if (!page || page->used == QUERY_PAGE_SIZE) {
        query_page **next = page ? &page->next : &queries->first;
        if (!*next) {
            *next = calloc(1, sizeof **next);
            if (!*next) die("Vulkan: cannot allocate query page");
            VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, type, QUERY_PAGE_SIZE};
            VKCHECK(vkCreateQueryPool(g_dev, &info, NULL, &(*next)->pool));
        }
        page = queries->last = *next;
        page->used = 0;
        vkCmdResetQueryPool(upload, page->pool, 0, QUERY_PAGE_SIZE);
    }
    *slot=page->used++;
    return page;
}
static const uint64_t *visibility_begin(visibility_queries *queries, VkCommandBuffer cb, VkCommandBuffer upload, int precise) {
    if (queries->active) return NULL;
    uint32_t slot;
    query_page *page=query_slot(&queries->pages,VK_QUERY_TYPE_OCCLUSION,upload,&slot);
    vkCmdBeginQuery(cb, page->pool, slot, precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    queries->active = 1;
    return &page->samples[slot];
}
static void visibility_end(visibility_queries *queries, VkCommandBuffer cb) {
    if (!queries->active) return;
    vkCmdEndQuery(cb, queries->pages.last->pool, queries->pages.last->used - 1);
    queries->active = 0;
}
static void query_read(query_pages *queries) {
    if (!queries->last) return;
    for (query_page *page = queries->first; ; page = page->next) {
        VKCHECK(vkGetQueryPoolResults(g_dev, page->pool, 0, page->used, sizeof page->samples, page->samples,
                                      sizeof *page->samples, VK_QUERY_RESULT_64_BIT));
        if (page == queries->last) break;
    }
    queries->last = NULL;
}
