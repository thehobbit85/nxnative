// SPIR-V helper emitted only for 32-bit integer-as-float vertex formats. It returns
// the IEEE binary32 word for one component, matching Metal's (float)(u)int32_t.
// Plain OpConvert*ToF may round an inexact result in either direction in Vulkan.
// Both conversions below take exact integers; all tie handling is integer arithmetic.
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t function, fn_type, u32, f32;
    uint32_t zero, one, eight, twenty_three, thirty_one, exponent_bias;
} scaled32_spv;

// The helper takes (word, signMask), signMask = 0 for uint and ~0u for int.
static size_t scaled32_function(uint32_t *out, uint32_t *bound, const scaled32_spv *s) {
    enum { Function = 54, Parameter = 55, End = 56, Label = 248, ReturnValue = 254,
           ConvertUToF = 112, Bitcast = 124, Add = 128, Sub = 130, Shr = 194, AShr = 195,
           Shl = 196, Or = 197, Xor = 198, And = 199 };
    uint32_t *p = out, u = s->u32, f = s->f32;
#define EMIT(...) do { const uint32_t ins[] = {__VA_ARGS__}; memcpy(p, ins, sizeof ins); p += sizeof ins / 4; } while (0)
#define BINARY(name, op, a, b) uint32_t name = (*bound)++; EMIT(5u << 16 | op, u, name, a, b)
#define UNARY(name, op, t, a) uint32_t name = (*bound)++; EMIT(4u << 16 | op, t, name, a)
    EMIT(5u << 16 | Function, u, s->function, 0, s->fn_type);
    uint32_t word = (*bound)++, sign_mask = (*bound)++;
    EMIT(3u << 16 | Parameter, u, word); EMIT(3u << 16 | Parameter, u, sign_mask);
    EMIT(2u << 16 | Label, (*bound)++);
    BINARY(sign_extended, AShr, word, s->thirty_one);
    BINARY(sign, And, sign_extended, sign_mask);
    BINARY(flipped, Xor, word, sign);
    BINARY(mag, Sub, flipped, sign);
    // The top 24 bits are exactly representable. Their float exponent tells how
    // many of the bottom eight bits must be discarded. For zero/small inputs k=0.
    BINARY(top, Shr, mag, s->eight);
    UNARY(top_float, ConvertUToF, f, top);
    UNARY(top_bits, Bitcast, u, top_float);
    BINARY(exponent, Shr, top_bits, s->twenty_three);
    BINARY(delta, Sub, exponent, s->exponent_bias); // exponent - (127 + 23 - 8)
    BINARY(negative, Shr, delta, s->thirty_one);
    BINARY(nonnegative_mask, Sub, negative, s->one);
    BINARY(k, And, delta, nonnegative_mask);
    // q is the upper 24 significant bits. Round the remainder to nearest, ties
    // toward even. For k=0 mask, remainder and bias are all zero.
    BINARY(step, Shl, s->one, k);
    BINARY(mask, Sub, step, s->one);
    BINARY(q, Shr, mag, k);
    BINARY(remainder, And, mag, mask);
    BINARY(odd, And, q, s->one);
    BINARY(mask_plus_odd, Add, mask, odd);
    BINARY(bias, Shr, mask_plus_odd, s->one);
    BINARY(biased, Add, remainder, bias);
    BINARY(increment, Shr, biased, k);
    BINARY(rounded, Add, q, increment);
    // rounded <= 2^24. Scaling by 2^k just adds k to the exponent (k=0 at zero).
    UNARY(exact, ConvertUToF, f, rounded);
    UNARY(bits, Bitcast, u, exact);
    BINARY(scale, Shl, k, s->twenty_three);
    BINARY(scaled, Add, bits, scale);
    BINARY(sign_bit, Shl, sign, s->thirty_one);
    BINARY(result, Or, scaled, sign_bit);
    EMIT(2u << 16 | ReturnValue, result); EMIT(1u << 16 | End);
#undef UNARY
#undef BINARY
#undef EMIT
    return (size_t)(p - out);
}
