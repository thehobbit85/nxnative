// Probes DM_PROBE: when the debugger needs a system authorization prompt on the Mac, use this to print registers and a backtrace at a guest address.
#include "dm.h"
#include "host_memory.h"
#include <TargetConditionals.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#if !TARGET_OS_IPHONE
#include <mach/mach_vm.h>   // the write watch (DM_PROBE_WATCH): iOS's SDK has no mach_vm.h
#endif
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
void crash_set_trap_hook(int (*fn)(ucontext_t *uc));
#if !TARGET_OS_IPHONE
#ifdef __APPLE__
// DM_PROBE_WATCH=<address>[+<length>] (hex guest address, length 8 by default): who writes these bytes, without a debugger. Their host page is write-protected for good once it is writable,
// and every store that faults on it is carried out by the fault handler through a writable alias of the page (STR / STUR / STP of general and SIMD registers in their immediate, pre- and
// post-index forms, register-offset STR, DC ZVA), so a burst of stores to a busy page cannot slip past. A store reaching the watched bytes prints the thread, pc, lr, the store address,
// the old and new values and a backtrace. Any other instruction (atomics, exclusives) is logged once per pc and the page is unprotected for it, then protected again a millisecond later
// (a store in that millisecond goes unseen). STEINS;GATE RE:BOOT: the engine object's page takes bursts of stores every frame, and a watch that unprotected the page for each fault missed
// the one that matters; this one found it at once (main+0x389e90, a setter its start-up code calls with 0: the game renders 1280x720 docked by its own choice)
static uint64_t g_wat, g_wlen, g_wpg, g_wpsz; static uint8_t *g_walias; static _Atomic int g_warmed;
static void watch_where(const char *tag, uint64_t a) {
    module_t *m = module_of((void *)a); Dl_info di;
    if (m) fprintf(stderr, " %s=%s+%#llx", tag, m->name, (unsigned long long)(a - (uint64_t)m->base));
    else if (dladdr((void *)a, &di) && di.dli_sname) fprintf(stderr, " %s=%s+%#lx", tag, di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr));
    else fprintf(stderr, " %s=%#llx", tag, (unsigned long long)a);
}
static uint64_t watch_val(void) { uint64_t v = 0; memcpy(&v, (void *)g_wat, g_wlen < 8 ? g_wlen : 8); return v; }
// Decode the store at pc: its address, size and data (up to 64 bytes); the base register write-back (wb: register number, value) for pre- and post-index forms. 0 when not a decodable store
static int watch_decode(ucontext_t *uc, uint32_t in, uint64_t *ea, unsigned *len, uint8_t *data, int *wb, uint64_t *wbv) {
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss; _STRUCT_ARM_NEON_STATE64 *ns = &uc->uc_mcontext->__ns;
#define XN(r) ((r) == 31 ? ss->__sp : (r) == 29 ? ss->__fp : (r) == 30 ? ss->__lr : ss->__x[r])        // base register: 31 = sp
#define XT(r) ((r) == 31 ? 0 : (r) == 29 ? ss->__fp : (r) == 30 ? ss->__lr : ss->__x[r])                // data register: 31 = zero
    unsigned rt = in & 31, rn = (in >> 5) & 31, v = (in >> 26) & 1; *wb = -1;
    if ((in & 0xffffffe0) == 0xd50b7420) {                        // DC ZVA: zero the 64-byte block
        *ea = XT(rt) & ~63ull; *len = 64; memset(data, 0, 64); return 1; }
    if ((in & 0x3a000000) == 0x28000000) {                                                               // STP / STNP: opc 101 V 0 type L imm7
        if (in & (1u << 22)) return 0;                                                                   // LDP
        unsigned opc = in >> 30, type = (in >> 23) & 3, sz = v ? 4u << opc : (opc & 2 ? 8 : 4);
        if ((!v && opc == 1) || opc == 3) return 0;                                                      // STGP / LDPSW, reserved
        int64_t imm = ((int64_t)(int32_t)(in << 10) >> 25) * (int64_t)sz; uint64_t base = XN(rn);
        *ea = type == 1 ? base : base + imm; *len = 2 * sz;
        unsigned r2 = (in >> 10) & 31;
        if (v) { memcpy(data, &ns->__v[rt], sz); memcpy(data + sz, &ns->__v[r2], sz); }
        else { uint64_t a = XT(rt), b = XT(r2); memcpy(data, &a, sz); memcpy(data + sz, &b, sz); }
        if (type == 1 || type == 3) { *wb = (int)rn; *wbv = base + imm; }
        return 1;
    }
    if ((in & 0x3b000000) == 0x39000000 || (in & 0x3b200000) == 0x38000000 || (in & 0x3b200c00) == 0x38200800) {   // STR (unsigned offset) / STUR, STR pre / post / STR (register)
        unsigned size = in >> 30, opc = (in >> 22) & 3, sz;
        if (v) { if (opc == 0) sz = 1u << size; else if (opc == 2 && size == 0) sz = 16; else return 0; }
        else { if (opc != 0) return 0; sz = 1u << size; }
        uint64_t base = XN(rn), addr;
        if ((in & 0x3b000000) == 0x39000000) addr = base + (uint64_t)((in >> 10) & 0xfff) * sz;          // unsigned offset
        else if (!(in & (1u << 21))) {                                                                   // imm9: unscaled, post (01), pre (11)
            int64_t imm = (int64_t)(int32_t)(in << 11) >> 23; unsigned k = (in >> 10) & 3;
            if (k == 2) return 0;                                                                        // STTR
            addr = k == 1 ? base : base + imm;
            if (k == 1 || k == 3) { *wb = (int)rn; *wbv = base + imm; }
        } else {                                                                                         // register offset: option (15:13), S (12)
            unsigned rm = (in >> 16) & 31, opt = (in >> 13) & 7, s = (in >> 12) & 1; uint64_t x = XT(rm);
            if (opt == 2) x = (uint32_t)x; else if (opt == 6) x = (uint64_t)(int64_t)(int32_t)x; else if (opt != 3 && opt != 7) return 0;
            addr = base + (s ? x << __builtin_ctz(sz) : x);
        }
        *ea = addr; *len = sz;
        if (v) memcpy(data, &ns->__v[rt], sz); else { uint64_t a = XT(rt); memcpy(data, &a, sz); }
        return 1;
    }
    return 0;
#undef XN
#undef XT
}
static int watch_fault(void *addr, int is_write, void *ucv) {
    uint64_t a = (uint64_t)addr;
    if (!is_write || !g_wat || a < g_wpg || a >= g_wpg + g_wpsz) return 0;
    ucontext_t *uc = ucv; uint64_t pc = uc->uc_mcontext->__ss.__pc, ea = 0, wbv = 0; unsigned len = 0; int wb = -1; uint8_t data[64];
    if (g_walias && watch_decode(uc, *(uint32_t *)pc, &ea, &len, data, &wb, &wbv)) {
        int hit = ea < g_wat + g_wlen && ea + len > g_wat; uint64_t old = hit ? watch_val() : 0;
        for (unsigned i = 0; i < len; i++) {   // bytes in the watched page go through the alias; a store crossing into the next page writes the rest directly
            uint64_t b = ea + i;
            if (b >= g_wpg && b < g_wpg + g_wpsz) g_walias[b - g_wpg] = data[i]; else *(volatile uint8_t *)b = data[i];
        }
        if (wb >= 0) { if (wb == 31) uc->uc_mcontext->__ss.__sp = wbv; else if (wb == 29) uc->uc_mcontext->__ss.__fp = wbv; else if (wb == 30) uc->uc_mcontext->__ss.__lr = wbv; else uc->uc_mcontext->__ss.__x[wb] = wbv; }
        uc->uc_mcontext->__ss.__pc = pc + 4;
        if (hit) {
            flockfile(stderr);
            { extern uint64_t g_t0_ns; uint64_t now_ns(void); uint32_t thr_cur_handle(void);
              fprintf(stderr, "[dm] WATCH %#llx [%.4fs] T=%x store of %u bytes at %#llx: %#llx → %#llx", (unsigned long long)g_wat, (now_ns() - g_t0_ns) / 1e9, thr_cur_handle(), len, (unsigned long long)ea, (unsigned long long)old, (unsigned long long)watch_val()); }
            watch_where("pc", pc); watch_where("lr", uc->uc_mcontext->__ss.__lr); fputc('\n', stderr);
            dm_backtrace(uc->uc_mcontext->__ss.__fp);
            funlockfile(stderr);
        }
        return 1;
    }
    // Exclusive and atomic stores (STXR / STLXR / STLR / STXP, CAS, SWP, LD<op>) address [Xn] with no offset: one that reaches the watched bytes is reported like a store, every time
    // (its new value lands when it runs on the unprotected page). Tears of the Kingdom's memory-shortage flag is set by such a store, and the page it lies on takes many of them
    { uint32_t in = *(uint32_t *)pc; int ex = (in & 0x3f000000) == 0x08000000, at = (in & 0x3f200c00) == 0x38200000;   // exclusive class (CAS / CASP among them); LD<op> / SWP
      if (ex || at) {
          _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss; unsigned rn = (in >> 5) & 31, sz = 1u << (in >> 30);
          uint64_t base = rn == 31 ? ss->__sp : rn == 29 ? ss->__fp : rn == 30 ? ss->__lr : ss->__x[rn];
          if ((in & 0xbfa00000) == 0x88200000) sz *= 2;   // STXP / STLXP: a register pair
          if (base < g_wat + g_wlen && base + sz > g_wat) {
              flockfile(stderr);
              { extern uint64_t g_t0_ns; uint64_t now_ns(void); uint32_t thr_cur_handle(void);
                fprintf(stderr, "[dm] WATCH %#llx [%.4fs] T=%x %s store of %u bytes at %#llx (instruction %08x, not emulated), value before %#llx", (unsigned long long)g_wat, (now_ns() - g_t0_ns) / 1e9, thr_cur_handle(),
                        ex ? "exclusive" : "atomic", sz, (unsigned long long)base, in, (unsigned long long)watch_val()); }
              watch_where("pc", pc); watch_where("lr", ss->__lr); fputc('\n', stderr);
              dm_backtrace(ss->__fp);
              funlockfile(stderr);
          }
      } }
    { static uint64_t seen[16]; static _Atomic int ns; int k = 0, n = atomic_load(&ns);   // not a store form the handler carries out: let it run on an unprotected page
      while (k < n && k < 16 && seen[k] != pc) k++;
      if (k == n && n < 16) { seen[atomic_fetch_add(&ns, 1) & 15] = pc; flockfile(stderr); fprintf(stderr, "[dm] WATCH: instruction %08x not emulated, page unprotected for it:", *(uint32_t *)pc); watch_where("pc", pc); fputc('\n', stderr); funlockfile(stderr); } }
    mprotect((void *)g_wpg, g_wpsz, PROT_READ | PROT_WRITE); atomic_store(&g_warmed, 0);
    return 1;
}
static uint64_t g_wrel; static int g_wheap;   // heap+<offset>: the address is that far past the guest heap's base, resolved once the game opens its heap
static void *watch_arm(void *p) {
    (void)p;
    for (;;) {
        if (g_wheap && !g_wat) {
            uint64_t hb = 0; dm_guest_heap_range(&hb, NULL); if (!hb) { usleep(1000); continue; }
            g_wpg = (hb + g_wrel) & ~(g_wpsz - 1); g_wat = hb + g_wrel;
            LOG("write watch on heap %#llx + %#llx = %#llx", (unsigned long long)hb, (unsigned long long)g_wrel, (unsigned long long)g_wat);
        }
        if (g_wat && !atomic_load(&g_warmed)) {
            mach_vm_address_t ra = g_wpg; mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
            if (!mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj) && ra <= g_wpg && (bi.protection & VM_PROT_WRITE)) {
                if (!g_walias) {   // a second, writable mapping of the same page, for the handler's stores
                    mach_vm_address_t al = 0; vm_prot_t cur, max;
                    if (mach_vm_remap(mach_task_self(), &al, g_wpsz, 0, VM_FLAGS_ANYWHERE, mach_task_self(), g_wpg, FALSE, &cur, &max, VM_INHERIT_NONE) == KERN_SUCCESS) g_walias = (uint8_t *)al;
                    LOG("WATCH %#llx armed, value %#llx%s", (unsigned long long)g_wat, (unsigned long long)watch_val(), g_walias ? "" : " ([RED] no alias mapping: every store unprotects the page)");
                }
                atomic_store(&g_warmed, 1); mprotect((void *)g_wpg, g_wpsz, PROT_READ); }
        }
        usleep(1000);
    }
    return NULL;
}
#endif
// DM_PROBE_POLL=<address>[+<length>] (hex guest address, up to 8 bytes, 4 by default): when even the watch above misses a write (a page that atomics keep unprotected), a host thread spins on
// the bytes and, on each change, prints old → new and dumps every guest thread with its backtrace (svc_thread_dump, the first 8 changes): the writer is still inside the code that wrote.
// Tears of the Kingdom's memory-shortage flag (a word its frame check compares with 1 and clears) lies on such a page. A suffix =<value> (hex) snapshots and dumps only the changes to that value,
// for a word that changes thousands of times before the bad store (Mario vs. Donkey Kong: a heap block's list link, rewritten by every allocation beside it, until 0x3f landed in it)
static uint64_t g_poll, g_plen, g_pval; static int g_pwant, g_pheap;
#ifdef __APPLE__
// All threads but this one stopped at once, then their pc, lr and 8 frame-pointer frames read and printed after they run again: the writer is caught a few microseconds after its store
static void poll_snap(void) {
    enum { MT = 256, NF = 8 }; static uint64_t fr[MT][NF + 2]; static thread_act_t keep[MT];
    thread_act_array_t ths; mach_msg_type_number_t nth = 0, ns = 0; thread_act_t self = mach_thread_self();
    if (task_threads(mach_task_self(), &ths, &nth) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < nth && ns < MT; i++) if (ths[i] != self && thread_suspend(ths[i]) == KERN_SUCCESS) keep[ns++] = ths[i];
    for (mach_msg_type_number_t i = 0; i < ns; i++) {
        arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT; memset(fr[i], 0, sizeof fr[i]);
        if (thread_get_state(keep[i], ARM_THREAD_STATE64, (thread_state_t)&st, &n) != KERN_SUCCESS) continue;
        fr[i][0] = __darwin_arm_thread_state64_get_pc(st); fr[i][1] = __darwin_arm_thread_state64_get_lr(st);
        uint64_t fp = __darwin_arm_thread_state64_get_fp(st), w[2]; vm_size_t g;
        for (int k = 0; k < NF && fp > 0x100000000ull && !vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)w, &g) && g == 16; k++) { fr[i][2 + k] = w[1]; fp = w[0]; }
    }
    for (mach_msg_type_number_t i = 0; i < ns; i++) thread_resume(keep[i]);
    for (mach_msg_type_number_t i = 0; i < nth; i++) mach_port_deallocate(mach_task_self(), ths[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)ths, nth * sizeof *ths); mach_port_deallocate(mach_task_self(), self);
    flockfile(stderr);
    for (mach_msg_type_number_t i = 0; i < ns; i++) {   // threads with guest code anywhere in their first frames
        int guest = 0; for (int k = 0; k < NF + 2; k++) guest |= module_of((void *)fr[i][k]) != NULL;
        if (!guest) continue;
        fprintf(stderr, "[dm] POLL thread %u:", i); watch_where("pc", fr[i][0]); watch_where("lr", fr[i][1]);
        for (int k = 2; k < NF + 2 && fr[i][k]; k++) watch_where("<", fr[i][k]);
        fputc('\n', stderr);
    }
    funlockfile(stderr);
}
#endif
static int poll_read(uint64_t *out) {
    uint64_t value = 0;
    if (!dm_host_read(g_poll, &value, (size_t)g_plen)) return 0;
    *out = value; return 1;
}
static void *poll_run(void *p) {
    (void)p; void svc_thread_dump(void); extern uint64_t g_t0_ns; uint64_t now_ns(void);
    uint64_t old = 0;   // read through the kernel: the address is not readable until the game maps it (the heap before SetHeapSize)
    if (g_pheap) { uint64_t hb = 0; while (dm_guest_heap_range(&hb, NULL), !hb) usleep(1000); g_poll += hb; }   // heap+<offset>
    while (!poll_read(&old)) usleep(1000);
    LOG("POLL %#llx + %llu armed, value %#llx", (unsigned long long)g_poll, (unsigned long long)g_plen, (unsigned long long)old);
    for (int n = 0;;) {
        uint64_t v = 0;
        if (!poll_read(&v) || v == old) continue;
        int hit = !g_pwant || v == g_pval;
#ifdef __APPLE__
        if (hit && n < 8) poll_snap();
#endif
        fprintf(stderr, "[dm] POLL %#llx [%.4fs] %#llx → %#llx\n", (unsigned long long)g_poll, (now_ns() - g_t0_ns) / 1e9, (unsigned long long)old, (unsigned long long)v);
        old = v;
        if (hit && n++ < 2) svc_thread_dump();
    }
    return NULL;
}
static void watch_install(void) {
    { const char *e = getenv("DM_PROBE_POLL");
      if (e) { char *end; g_pheap = !strncmp(e, "heap+", 5); if (g_pheap) e += 5; g_poll = strtoull(e, &end, 16); g_plen = *end == '+' ? strtoull(end + 1, &end, 16) : 4;
               if (*end == '=') { g_pwant = 1; g_pval = strtoull(end + 1, 0, 16); }
               if ((!g_poll && !g_pheap) || !g_plen || g_plen > 8) die("DM_PROBE_POLL format: <address>[+<length up to 8>][=<value>] (hex)");
               pthread_t t; int rc = pthread_create(&t, NULL, poll_run, NULL);
               if (rc) die("DM_PROBE_POLL thread: %s", strerror(rc));
               pthread_detach(t);
#ifndef __APPLE__
               LOG("[RED] POLL live thread snapshots are unavailable on this target; memory changes and service-state dumps are enabled");
#endif
      } }
    const char *e = getenv("DM_PROBE_WATCH"); if (!e) return;
#ifdef __APPLE__
    char *end; g_wheap = !strncmp(e, "heap+", 5);
    uint64_t at = strtoull(e + (g_wheap ? 5 : 0), &end, 16); g_wlen = *end == '+' ? strtoull(end + 1, 0, 16) : 8;
    if ((!at && !g_wheap) || !g_wlen) die("DM_PROBE_WATCH format: [heap+]<address>[+<length>] (hex)");
    g_wpsz = (uint64_t)getpagesize();
    if ((at & (g_wpsz - 1)) + g_wlen > g_wpsz) die("DM_PROBE_WATCH: the watched bytes must lie in one host page");
    if (g_wheap) { g_wrel = at; g_wpg = 0; } else { g_wat = at; g_wpg = g_wat & ~(g_wpsz - 1); }
    dm_fault_hook_register(watch_fault);
    pthread_t t; pthread_create(&t, NULL, watch_arm, NULL); pthread_detach(t);
    if (g_wheap) LOG("write watch on heap + %#llx + %#llx (resolved once the game opens its heap)", (unsigned long long)g_wrel, (unsigned long long)g_wlen);
    else LOG("write watch on %#llx + %#llx (host page %#llx)", (unsigned long long)g_wat, (unsigned long long)g_wlen, (unsigned long long)g_wpg);
#else
    LOG("[RED] DM_PROBE_WATCH requires the Apple host");
#endif
}

#endif

void probe_install(void) {
#if !TARGET_OS_IPHONE
    void dm_probe_text_install(void);
    dm_probe_text_install();
    watch_install();
#endif
}
