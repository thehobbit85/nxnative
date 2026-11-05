// Execute a decoded ARM64 diagnostic store through a leased shared alias. The
// guest view stays read-only. Validate the whole extent before changing memory
// or context; a rejected instruction can be retried by the caller unchanged.
#pragma once
#include "watch_decode.h"
#include "watch_page.h"

typedef struct { uint64_t address; unsigned size; uint8_t data[64]; } dm_watch_store_result;
static inline int dm_watch_store(dm_watch_page *p, ucontext_t *uc, uint32_t in,
                                 uint64_t dczid, dm_watch_store_result *result) {
    uint64_t address, wbvalue; unsigned size; int wb;
    uint8_t data[64];
    int zva = (in & 0xffffffe0) == 0xd50b7420;
    // The reference decoder describes 64-byte ZVA. DZP and any other host block
    // size require retrying the original instruction, never zeroing a subset.
    if (zva && (dczid & 31) != 4) return 0;
    if (!dm_watch_decode(uc, in, &address, &size, data, &wb, &wbvalue) ||
        address < (uintptr_t)p->guest || size > p->size ||
        address - (uintptr_t)p->guest > p->size - size) return 0;
    uint8_t *dst = p->alias + (address - (uintptr_t)p->guest);
    int pair = (in & 0x3a000000) == 0x28000000;
    // Match each native element width instead of byte-copying a shared scalar.
    // Alignment is unchanged by the page-aligned alias. Pair instructions retain
    // pair semantics; they do not promise a stronger atomicity than the guest.
    if (zva) __asm__ volatile("dc zva, %0" :: "r"(dst) : "memory");
    else if (pair && size == 32) __asm__ volatile("ldp q0, q1, [%1]\nstp q0, q1, [%0]" :: "r"(dst), "r"(data) : "v0", "v1", "memory");
    else if (!pair && size == 16) __asm__ volatile("ldr q0, [%1]\nstr q0, [%0]" :: "r"(dst), "r"(data) : "v0", "memory");
    else if (pair) {
        if (size == 16) {
            uint64_t a, b; memcpy(&a, data, 8); memcpy(&b, data + 8, 8);
            __asm__ volatile("stp %1, %2, [%0]" :: "r"(dst), "r"(a), "r"(b) : "memory");
        } else {
            uint32_t a, b; memcpy(&a, data, 4); memcpy(&b, data + 4, 4);
            __asm__ volatile("stp %w1, %w2, [%0]" :: "r"(dst), "r"(a), "r"(b) : "memory");
        }
    } else {
        uint64_t a = 0; memcpy(&a, data, size);
        switch (size) {
        case 1: __asm__ volatile("strb %w1, [%0]" :: "r"(dst), "r"(a) : "memory"); break;
        case 2: __asm__ volatile("strh %w1, [%0]" :: "r"(dst), "r"(a) : "memory"); break;
        case 4: __asm__ volatile("str %w1, [%0]" :: "r"(dst), "r"(a) : "memory"); break;
        case 8: __asm__ volatile("str %1, [%0]" :: "r"(dst), "r"(a) : "memory"); break;
        default: return 0; // all accepted decoder forms are handled above
        }
    }
    if (wb == 31) UC_SP(uc) = wbvalue;
    else if (wb == 29) UC_FP(uc) = wbvalue;
    else if (wb == 30) UC_LR(uc) = wbvalue;
    else if (wb >= 0) UC_X(uc, wb) = wbvalue;
    UC_PC(uc) += 4;
    if (result) { result->address = address; result->size = size; memcpy(result->data, data, size); }
    return 1;
}
