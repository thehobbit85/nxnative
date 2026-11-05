// Exercise the diagnostic's executor with real Linux fault contexts and stores.
// No handler logging, allocation or host locks; every writer owns its altstack.
#include "../watch_store.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static dm_watch_page page = {.fd = -1};
static _Atomic unsigned faults;
static uint64_t dczid;
static _Thread_local unsigned char signal_stack[65536] __attribute__((aligned(16)));
static void altstack(void) {
    stack_t s = {.ss_sp = signal_stack, .ss_size = sizeof signal_stack};
    assert(!sigaltstack(&s, NULL));
}
static void handler(int sig, siginfo_t *info, void *context) {
    (void)sig;
    ucontext_t *uc = context;
    uintptr_t a = (uintptr_t)info->si_addr;
    if (a < (uintptr_t)page.guest || a - (uintptr_t)page.guest >= page.size ||
        !dm_watch_page_acquire(&page)) _exit(90);
    uint32_t in; memcpy(&in, (void *)UC_PC(uc), 4);
    int ok = dm_watch_store(&page, uc, in, dczid, NULL);
    dm_watch_page_release(&page);
    if (!ok) _exit(91);
    atomic_fetch_add_explicit(&faults, 1, memory_order_relaxed);
}
static void check_readonly(void) {
    FILE *f = fopen("/proc/self/maps", "r"); assert(f);
    char line[512], perm[5]; unsigned long lo, hi; int found = 0;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perm) == 3 &&
        (uintptr_t)page.guest >= lo && (uintptr_t)page.guest < hi) {
        assert(perm[0] == 'r' && perm[1] == '-' && perm[2] == '-'); found = 1;
    }
    fclose(f); assert(found);
}
static void *writer(void *arg) {
    altstack(); unsigned n = (unsigned)(uintptr_t)arg;
    uint64_t *p = (uint64_t *)(page.guest + 512 + 16 * n);
    for (uint64_t i = 1; i <= 4096; i++) __asm__ volatile("str %1, [%0]" :: "r"(p), "r"(i) : "memory");
    return NULL;
}
static const uint64_t patterns[] = {UINT64_C(0x1122334455667788), UINT64_C(0x99aabbccddeeff00)};
static _Atomic unsigned ready, done, samples;
static void *contended_writer(void *arg) {
    altstack(); uint64_t value = patterns[(uintptr_t)arg]; uint8_t *p = page.guest + 768;
    atomic_fetch_add(&ready, 1);
    while (atomic_load(&ready) != 3) sched_yield();
    for (unsigned i = 0; i < 8192; i++) __asm__ volatile("str %1, [%0]" :: "r"(p), "r"(value) : "memory");
    atomic_fetch_add(&done, 1); return NULL;
}
static void *observer(void *arg) {
    (void)arg; uint8_t *p = page.guest + 768;
    atomic_fetch_add(&ready, 1);
    while (atomic_load(&ready) != 3) sched_yield();
    do {
        uint64_t value; __asm__ volatile("ldr %0, [%1]" : "=r"(value) : "r"(p) : "memory");
        assert(value == patterns[0] || value == patterns[1]); atomic_fetch_add(&samples, 1);
    } while (atomic_load(&done) != 2);
    return NULL;
}
extern const uint32_t reject_instructions[];
__asm__(".text\n.p2align 2\n.globl reject_instructions\nreject_instructions:\n"
        "str x0, [x1], #8\n" "stp x0, x2, [x1], #16\n" "str q0, [x1]\n"
        "stlr x0, [x1]\n" "stlxr w2, x0, [x1]\n" "dc zva, x1\n" "nop\n");
static void rejected(void) {
    ucontext_t uc = {0};
    UC_PC(&uc) = 0x1234; UC_X(&uc, 0) = 0xabcdef;
    uint8_t *before = malloc(page.size); assert(before); memcpy(before, page.alias, page.size);
    for (unsigned i = 0; i < 7; i++) {
        // First two stores straddle the end; the vector lacks FPSIMD state.
        UC_X(&uc, 1) = (uintptr_t)page.guest + (i < 2 ? page.size - 4 : 0);
        uint8_t saved[sizeof uc]; memcpy(saved, &uc, sizeof saved);
        dm_watch_store_result result; memset(&result, 0xa5, sizeof result);
        uint8_t empty[sizeof result]; memcpy(empty, &result, sizeof empty);
        assert(!dm_watch_store(&page, &uc, reject_instructions[i], i == 5 ? 5 : dczid, &result));
        // Rejection must leave every byte, including padding, untouched.
        assert(!memcmp((const uint8_t *)&uc, saved, sizeof uc) && !memcmp((const uint8_t *)&result, empty, sizeof result));
        assert(!memcmp(before, page.alias, page.size));
    }
    UC_X(&uc, 1) = (uintptr_t)page.guest + 256;
    struct { uint64_t lo; dm_watch_store_result result; uint64_t hi; } out;
    memset(&out, 0xa5, sizeof out);
    assert(dm_watch_store(&page, &uc, reject_instructions[0], dczid, &out.result));
    assert(out.lo == UINT64_C(0xa5a5a5a5a5a5a5a5) && out.hi == out.lo);
    assert(out.result.address == (uintptr_t)page.guest + 256 && out.result.size == 8);
    uint64_t value; memcpy(&value, out.result.data, 8); assert(value == 0xabcdef);
    for (unsigned i = 8; i < sizeof out.result.data; i++) assert(out.result.data[i] == 0xa5);
    assert(UC_PC(&uc) == 0x1238 && UC_X(&uc, 1) == (uintptr_t)page.guest + 264);
    free(before);
}
int main(void) {
    page.size = (size_t)getpagesize();
    uint8_t *p = mmap(NULL, page.size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(p != MAP_FAILED);
    assert(!dm_watch_page_prepare(&page, p, page.size));
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
    altstack(); struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&sa.sa_mask); assert(!sigaction(SIGSEGV, &sa, NULL));
    assert(!dm_watch_page_open(&page));
    const uint64_t a = UINT64_C(0x8877665544332211), b = UINT64_C(0xffeeddccbbaa0099);
    unsigned expected = 0;
#define CHECK(bytes, offset, count) do { expected++; assert(atomic_load(&faults) == expected); \
    assert(!memcmp(p + (offset), (bytes), (count))); check_readonly(); } while (0)
    __asm__ volatile("strb %w1, [%0, #1]" :: "r"(p), "r"(a) : "memory"); CHECK(&a, 1, 1);
    __asm__ volatile("strh %w1, [%0, #2]" :: "r"(p), "r"(a) : "memory"); CHECK(&a, 2, 2);
    __asm__ volatile("str %w1, [%0, #4]" :: "r"(p), "r"(a) : "memory"); CHECK(&a, 4, 4);
    __asm__ volatile("str %1, [%0, #8]" :: "r"(p), "r"(a) : "memory"); CHECK(&a, 8, 8);
    uint8_t *base = p + 32;
    __asm__ volatile("stur %1, [%0, #-1]" :: "r"(base), "r"(a) : "memory"); CHECK(&a, 31, 8);
    __asm__ volatile("str %1, [%0, #-16]!" : "+&r"(base) : "r"(a) : "memory");
    CHECK(&a, 16, 8); assert(base == p + 16);
    __asm__ volatile("str %1, [%0], #16" : "+&r"(base) : "r"(b) : "memory");
    CHECK(&b, 16, 8); assert(base == p + 32);
    uint32_t wp[] = {(uint32_t)a, (uint32_t)b}; uint64_t xp[] = {a, b};
    __asm__ volatile("stp %w1, %w2, [%0]" :: "r"(base), "r"(a), "r"(b) : "memory"); CHECK(wp, 32, 8);
    __asm__ volatile("stnp %1, %2, [%0, #16]" :: "r"(base), "r"(a), "r"(b) : "memory"); CHECK(xp, 48, 16);
    __asm__ volatile("stp %1, %2, [%0, #32]!" : "+&r"(base) : "r"(a), "r"(b) : "memory");
    CHECK(xp, 64, 16); assert(base == p + 64);
    __asm__ volatile("stp %1, %2, [%0], #16" : "+&r"(base) : "r"(a), "r"(b) : "memory");
    CHECK(xp, 64, 16); assert(base == p + 80);
    uint8_t vectors[32]; for (unsigned i = 0; i < sizeof vectors; i++) vectors[i] = 0x50 + i;
    __asm__ volatile("ldr q0, [%1]\nstr q0, [%0]" :: "r"(base), "r"(vectors) : "v0", "memory"); CHECK(vectors, 80, 16);
    __asm__ volatile("ldp q0, q1, [%1]\nstp q0, q1, [%0, #32]!" : "+&r"(base) : "r"(vectors) : "v0", "v1", "memory");
    CHECK(vectors, 112, 32); assert(base == p + 112);
    __asm__ volatile("ldp d0, d1, [%1]\nstp d0, d1, [%0], #16" : "+&r"(base) : "r"(vectors) : "v0", "v1", "memory");
    CHECK(vectors, 112, 16); assert(base == p + 128);
    __asm__ volatile("ldp s0, s1, [%1]\nstnp s0, s1, [%0]" :: "r"(base), "r"(vectors) : "v0", "v1", "memory"); CHECK(vectors, 128, 8);
    if ((dczid & 31) == 4) {
        memset(page.alias + 192, 0xa5, 64); uint8_t zero[64] = {0};
        __asm__ volatile("dc zva, %0" :: "r"(p + 211) : "memory"); CHECK(zero, 192, 64);
    }
    rejected();
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, writer, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 4; i++) { assert(!pthread_join(threads[i], NULL)); assert(*(uint64_t *)(p + 512 + 16 * i) == 4096); }
    assert(atomic_load(&faults) == expected + 4 * 4096); check_readonly();
    memcpy(page.alias + 768, patterns, 8);
    assert(!pthread_create(&threads[0], NULL, contended_writer, NULL));
    assert(!pthread_create(&threads[1], NULL, contended_writer, (void *)1));
    assert(!pthread_create(&threads[2], NULL, observer, NULL));
    for (unsigned i = 0; i < 3; i++) assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&samples) && atomic_load(&faults) == expected + 4 * 4096 + 2 * 8192);
    check_readonly();
    assert(!dm_watch_page_retire(&page)); assert(!munmap(p, page.size));
    printf("PASS: %u actual ARM64 store faults, native widths/writeback, concurrent writers, readonly guest and rejected-store preservation\n", atomic_load(&faults));
    return 0;
}
