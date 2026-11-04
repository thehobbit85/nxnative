// Stubs for t_condvar: host facilities of hle_thread.c that the unit test does not use
#include "../dm.h"
struct dm_flags g_flag;
uint64_t g_t0_ns;
void crash_thread_init(void) {}
void guest_thread_init(void) {}
void guest_thread_set_id(uint64_t id) { (void)id; }
void guest_gate_stack(int own_stack) { (void)own_stack; }
// The notification scheduler test supplies a C entry for real thread SVCs.
#ifndef DM_TEST_THREAD_ENTER
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) { (void)entry; (void)arg; (void)sp; abort(); }
#endif
void *guest_sym(const char *name) { (void)name; return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
#include <time.h>
uint64_t now_ns(void) { struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t)) abort(); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
__thread _Atomic int dm_host_work;   // referenced by hle_thread.c; in the host it is defined in hle_nvn.c
#ifdef __APPLE__
pthread_key_t g_key18;   // read by hle_thread.c's SVC entry and pause signal; in the host it is defined in loader.c (never created here: those paths do not run)
#else
void *dm_slot_get(unsigned key) { (void)key; return NULL; } // No guest register context is entered by these unit fixtures.
#endif
