// Decode actual assembler-emitted instructions against independent addresses,
// payload bytes and register writebacks on Darwin and Linux signal contexts.
#include "../watch_decode.h"
#include <assert.h>
#include <stdio.h>
#ifdef __APPLE__
#define ASM_NAME "_dm_watch_instructions"
#else
#define ASM_NAME "dm_watch_instructions"
#endif
extern const uint32_t dm_watch_instructions[];
__asm__(".text\n.p2align 2\n.globl " ASM_NAME "\n" ASM_NAME ":\n"
    "str x0, [x1, #24]\n" "str w0, [x1, #12]\n" "strh w0, [x1, #6]\n" "strb w0, [x1, #3]\n"
    "stur x0, [x1, #-9]\n" "str x0, [x1, #-16]!\n" "str x0, [x1], #16\n"
    "str x0, [sp, #8]\n" "str xzr, [x1]\n" "str x29, [x1]\n" "str x30, [x1]\n"
    "str x0, [x1, x3, lsl #3]\n" "str x0, [x1, w4, sxtw #3]\n" "strb w0, [x1, w3, uxtw]\n"
    "stp x0, x2, [x1, #-16]\n" "stp w0, w2, [x1, #8]\n" "stnp x0, x2, [x1, #16]\n"
    "stp x0, x2, [x1, #-16]!\n" "stp x0, x2, [x1], #16\n" "stp xzr, xzr, [x1]\n"
    "str q5, [x1, #32]\n" "str d5, [x1, #16]\n" "str s5, [x1, #8]\n" "str h5, [x1, #4]\n" "str b5, [x1, #2]\n"
    "stur q5, [x1, #-7]\n" "stp q5, q6, [x1, #-32]!\n" "stp d5, d6, [x1], #16\n"
    "stnp s5, s6, [x1, #8]\n" "str q5, [x1, x3, lsl #4]\n" "dc zva, x1\n"
    "ldr x0, [x1]\n" "ldp x0, x2, [x1]\n" "sttr x0, [x1]\n"
    "ldaxr x0, [x1]\n" "stlxr w2, x0, [x1]\n" "stlr x0, [x1]\n" "nop\n");
static uint64_t reg(ucontext_t *uc, unsigned n) {
    return n == 31 ? 0 : n == 29 ? UC_FP(uc) : n == 30 ? UC_LR(uc) : UC_X(uc, n);
}
int main(void) {
    ucontext_t uc = {0}; uint8_t vectors[32][16];
#ifdef __APPLE__
    struct __darwin_mcontext64 mc = {0}; uc.uc_mcontext = &mc;
#endif
    for (unsigned i = 0; i < 29; i++) UC_X(&uc, i) = UINT64_C(0x8877665544332200) + i;
    UC_X(&uc, 1) = 0x100000; UC_X(&uc, 3) = 3; UC_X(&uc, 4) = UINT64_C(0x1234fffffffd);
    UC_FP(&uc) = 0x123456789abcdef0; UC_LR(&uc) = 0xfedcba9876543210; UC_SP(&uc) = 0x200000; UC_PC(&uc) = 0x300000;
    for (unsigned r = 0; r < 32; r++) for (unsigned b = 0; b < 16; b++) vectors[r][b] = r * 7 + b;
#ifdef __APPLE__
    memcpy(mc.__ns.__v, vectors, sizeof vectors);
#else
    uint32_t head[] = {0x46508001, 528, 0, 0};
    memcpy(uc.uc_mcontext.__reserved, head, sizeof head); memcpy(uc.uc_mcontext.__reserved + 16, vectors, sizeof vectors);
#endif
    struct example { int offset; unsigned size, r1, r2; int vector, wb; uint64_t after; };
    const struct example cases[] = {
        {24,8,0,0,0,-1,0}, {12,4,0,0,0,-1,0}, {6,2,0,0,0,-1,0}, {3,1,0,0,0,-1,0},
        {-9,8,0,0,0,-1,0}, {-16,8,0,0,0,1,0xffff0}, {0,8,0,0,0,1,0x100010},
        {0x100008,8,0,0,0,-1,0}, {0,8,31,0,0,-1,0}, {0,8,29,0,0,-1,0}, {0,8,30,0,0,-1,0},
        {24,8,0,0,0,-1,0}, {-24,8,0,0,0,-1,0}, {3,1,0,0,0,-1,0},
        {-16,16,0,2,0,-1,0}, {8,8,0,2,0,-1,0}, {16,16,0,2,0,-1,0},
        {-16,16,0,2,0,1,0xffff0}, {0,16,0,2,0,1,0x100010}, {0,16,31,31,0,-1,0},
        {32,16,5,0,1,-1,0}, {16,8,5,0,1,-1,0}, {8,4,5,0,1,-1,0}, {4,2,5,0,1,-1,0}, {2,1,5,0,1,-1,0},
        {-7,16,5,0,1,-1,0}, {-32,32,5,6,1,1,0xfffe0}, {0,16,5,6,1,1,0x100010},
        {8,8,5,6,1,-1,0}, {48,16,5,0,1,-1,0}, {0,64,0,0,0,-1,0},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        const struct example *t = cases + i; uint64_t ea = 0, wbv = 0; unsigned len = 0; int wb = -2;
        uint8_t data[66], expected[64] = {0}; memset(data, 0xa5, sizeof data);
        int pair = i >= 14 && i <= 19; pair |= i >= 26 && i <= 28;
        if (i != 30) {
            unsigned n = t->size / (pair ? 2 : 1);
            if (t->vector) { memcpy(expected, vectors[t->r1], n); if (pair) memcpy(expected + n, vectors[t->r2], n); }
            else { uint64_t a = reg(&uc, t->r1), b = reg(&uc, t->r2); memcpy(expected, &a, n); if (pair) memcpy(expected + n, &b, n); }
        }
        assert(dm_watch_decode(&uc, dm_watch_instructions[i], &ea, &len, data + 1, &wb, &wbv));
        assert(ea == 0x100000ull + t->offset && len == t->size && wb == t->wb && (wb < 0 || wbv == t->after));
        assert(data[0] == 0xa5 && !memcmp(data + 1, expected, len));
        for (unsigned j = len + 1; j < sizeof data; j++) assert(data[j] == 0xa5);
        assert(UC_PC(&uc) == 0x300000 && UC_X(&uc, 1) == 0x100000 && UC_SP(&uc) == 0x200000);
    }
    for (unsigned i = 31; i < 38; i++) {
        uint64_t ea, wbv; unsigned len; int wb; uint8_t data[64];
        assert(!dm_watch_decode(&uc, dm_watch_instructions[i], &ea, &len, data, &wb, &wbv));
    }
#ifndef __APPLE__
    memset(uc.uc_mcontext.__reserved, 0, sizeof uc.uc_mcontext.__reserved);
    uint64_t ea, wbv; unsigned len; int wb; uint8_t data[64];
    assert(!dm_watch_decode(&uc, dm_watch_instructions[20], &ea, &len, data, &wb, &wbv));
    assert(dm_watch_decode(&uc, dm_watch_instructions[0], &ea, &len, data, &wb, &wbv));
#endif
    puts("PASS: 31 ARM64 store forms, seven rejected loads/atomics/unsupported instructions, guarded payloads and context preservation");
    return 0;
}
