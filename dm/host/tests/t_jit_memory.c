// Generated instructions must be executable through the guest view after each
// update, including on another thread. Android never maps a page writable + executable.
#include "../jit_memory.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void *execute(void *address) {
    return (void *)(uintptr_t)((int (*)(void))address)();
}
#ifndef __APPLE__
static void permissions(void *address, const char *want) {
    FILE *f = fopen("/proc/self/maps", "r");
    assert(f);
    char line[512], mode[5];
    unsigned long lo, hi;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, mode) == 3 &&
            (uintptr_t)address >= lo && (uintptr_t)address < hi) {
            assert(!strncmp(mode, want, 3)); found = 1; break;
        }
    }
    fclose(f);
    assert(found);
}
#endif
int main(void) {
    dm_jit_memory memory = {0};
    assert(!dm_jit_memory_create(&memory, 0x200000));
#ifndef __APPLE__
    assert(memory.rw != memory.rx);
    permissions(memory.rw, "rw-");
    permissions(memory.rx, "r-x");
#endif
    for (unsigned value = 42; value <= 43; value++) {
        uint32_t code[] = {0x52800000u | value << 5, 0xd65f03c0u}; // mov w0,#value; ret
        dm_jit_memory_write(1);
        memcpy(memory.rw, code, sizeof code);
        dm_jit_memory_write(0);
        dm_jit_memory_publish(&memory, 0, sizeof code);
        pthread_t worker;
        void *result;
        assert(!pthread_create(&worker, NULL, execute, memory.rx));
        assert(!pthread_join(worker, &result));
        assert((uintptr_t)result == value);
    }
    dm_jit_memory_destroy(&memory);
    puts("JIT code publication and replacement passed");
}
