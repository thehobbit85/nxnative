// CPU replay can determine timestamp counters in the current submission.
// Visibility and other GPU query addresses stay unknown, as in rc_value on Metal.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t *slots; size_t count, capacity; } report_addresses;
static size_t report_slot(const report_addresses *set, uint64_t addr) {
    size_t i = (addr >> 4) & (set->capacity - 1);
    while (set->slots[i] && set->slots[i] != addr) i = (i + 1) & (set->capacity - 1);
    return i;
}
static int report_contains(const report_addresses *set, uint64_t addr) {
    return set->capacity && addr && set->slots[report_slot(set, addr)] == addr;
}
static void report_insert(report_addresses *set, uint64_t addr) {
    if (!addr || report_contains(set, addr)) return;
    if (set->count >= set->capacity / 2) {
        report_addresses old = *set;
        set->capacity = old.capacity ? 2 * old.capacity : 64;
        set->slots = calloc(set->capacity, sizeof *set->slots);
        if (!set->slots) die("Vulkan: cannot track conditional render reports");
        for (size_t i = 0; i < old.capacity; i++)
            if (old.slots[i]) set->slots[report_slot(set, old.slots[i])] = old.slots[i];
        free(old.slots);
    }
    set->slots[report_slot(set, addr)] = addr;
    set->count++;
}
static report_addresses pending_timestamps, gpu_reports;
static int conditional_skip;
static void condition_begin(void) {
    conditional_skip = 0;
    if (pending_timestamps.count) {
        memset(pending_timestamps.slots, 0, pending_timestamps.capacity * sizeof *pending_timestamps.slots);
        pending_timestamps.count = 0;
    }
}
static void condition_set(const p_render_cond *condition) {
    if (condition->mode >= 2) { conditional_skip = condition->mode == 3; return; }
    uint64_t a = condition->addr, b = a + 16;
    if (report_contains(&gpu_reports, a) || report_contains(&gpu_reports, b)) {
        conditional_skip = 0;
        return;
    }
    uint64_t x = report_contains(&pending_timestamps, a) ? 0 : *(const volatile uint64_t *)(uintptr_t)a;
    uint64_t y = report_contains(&pending_timestamps, b) ? 0 : *(const volatile uint64_t *)(uintptr_t)b;
    conditional_skip = condition->mode ? x == y : x != y;
}
