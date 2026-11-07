// Android guest Vulkan entry points are distinct from the native loader symbols.
#pragma once
#include "../../host/hle.h"
#include <vulkan/vulkan_core.h>

enum { GVK_GLOBAL, GVK_INSTANCE, GVK_DEVICE };
typedef struct { const char *name; PFN_vkVoidFunction fn; unsigned scope; } guest_vk_proc;
#define GVK_RECORD(name, target, scope_) \
    __attribute__((used, DM_SECTION(dmguestvk))) static const guest_vk_proc gp_##name = {#name, (PFN_vkVoidFunction)target, scope_}; \
    __attribute__((used, DM_SECTION(swhle))) static const hle_ent ge_##name = {#name, (void *)target}
// All parameters must fit the eight ARM64 argument registers. Longer signatures
// need an argument packet before entering the guarded host stack (not a raw stub).
#define GVK_THUNK(entry, body) \
    __asm__(".text\n.p2align 2\n.globl " DM_ASM_SYM(entry) "\n" DM_ASM_HIDDEN " " DM_ASM_SYM(entry) "\n" DM_ASM_SYM(entry) ":\n" \
            DM_ASM_ADDR("x16", body) "    b " DM_ASM_SYM(dm_bigstack_enter) "\n")
#define GVK(name, scope_, ret, ...) \
    static ret impl_##name(__VA_ARGS__) __attribute__((used)); \
    extern ret guest_##name(__VA_ARGS__); \
    static const PFN_##name check_##name __attribute__((unused)) = guest_##name; \
    GVK_THUNK(guest_##name, impl_##name); \
    GVK_RECORD(name, guest_##name, scope_); \
    static ret impl_##name(__VA_ARGS__)
// The short C entry gathers register and stack arguments into a packet before
// the stack switch. Only one pointer crosses the shared register-only thunk.
#define GVK_PACKED(name, scope_, ret, packet, ...) \
    static ret packed_impl_##name(const packet *args) __attribute__((used)); \
    extern ret packed_enter_##name(const packet *args); \
    GVK_THUNK(packed_enter_##name, packed_impl_##name); \
    static ret guest_##name(__VA_ARGS__); \
    static const PFN_##name check_##name __attribute__((unused)) = guest_##name; \
    GVK_RECORD(name, guest_##name, scope_); \
    static ret guest_##name(__VA_ARGS__)
#define GVK_ALIAS(alias, core, scope_) \
    static const PFN_##alias check_##alias __attribute__((unused)) = guest_##core; \
    GVK_RECORD(alias, guest_##core, scope_)
