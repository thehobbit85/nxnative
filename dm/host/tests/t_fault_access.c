// Production SIGSEGV/SIGBUS hooks must distinguish a GPU-tracked write from
// a read fault, resume the exact instruction, and use a signal-safe stack.
#include "../dm.h"
#include <assert.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

extern void crash_install(void);
static uint8_t *page;
static size_t bytes;
static volatile sig_atomic_t expected, seen;

static int fault(void *address, int write_access, void *context) {
    (void)context;
    if (address != page) return 0;
    stack_t active;
    if (sigaltstack(NULL, &active) || !(active.ss_flags & SS_ONSTACK)) _exit(2);
    if (write_access != expected) _exit(3);
    if (mprotect(page, bytes, expected ? PROT_READ | PROT_WRITE : PROT_READ)) _exit(4);
    ++seen;
    return 1;
}

int main(void) {
    // Embedders can already own a large enough signal stack.
    stack_t existing = {.ss_sp = malloc(128 * 1024), .ss_size = 128 * 1024}, active;
    assert(existing.ss_sp && !sigaltstack(&existing, NULL));
    crash_install();
    assert(!sigaltstack(NULL, &active) && active.ss_sp == existing.ss_sp);
    dm_fault_hook_register(fault);
    bytes = (size_t)getpagesize();
    page = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(page != MAP_FAILED);
    volatile uint8_t *pixel = page;
    assert(*pixel == 0 && seen == 1);
    expected = 1;
    *pixel = 42;
    assert(*pixel == 42 && seen == 2);
    assert(!munmap(page, bytes));
    existing.ss_flags = SS_DISABLE;
    assert(!sigaltstack(&existing, NULL));
    free(existing.ss_sp);
    puts("Recovered read and write faults on the existing alternate signal stack");
}
