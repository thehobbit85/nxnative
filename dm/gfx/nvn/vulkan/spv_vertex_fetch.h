// A vertex pipeline whose NVN formats expose a different numeric class from the
// translated shader fetches in the format's own class and bitcasts each load.
// No change to locations, component selection, descriptor sets or guest buffers.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../nvn_refl.h"

typedef struct {
    uint32_t pointee;           // OpTypePointer Input's original value type
    uint8_t cls, width;         // scalar/vector value types: class (NVN_CT_* + 1), width
    uint8_t location, fetch;    // input variables: location + 1; converted pointers: new class
} vin_id;

static uint32_t *spv_vertex_fetch(const uint32_t *words, size_t count, uint32_t mask,
                                  const uint8_t reflected[32], size_t *out_count, const char **reason) {
    enum { TypeInt = 21, TypeFloat = 22, TypeVector = 23, TypePointer = 32, Function = 54,
           Variable = 59, Load = 61, AccessChain = 65, InBoundsAccessChain = 66, Decorate = 71,
           Bitcast = 124, Input = 1, Location = 30 };
    *reason = "invalid SPIR-V";
    if (count < 5 || words[0] != 0x07230203 || words[3] == 0 || words[3] > 0x3fffff ||
        count > (SIZE_MAX / sizeof(uint32_t) - 160) / 2) return NULL;
    uint32_t bound = words[3];
    vin_id *ids = calloc(bound, sizeof *ids);
    // Every rewritten load adds four words to an instruction of at least four.
    // The three numeric classes need at most four type and four pointer declarations each.
    uint32_t *out = malloc((2 * count + 160) * sizeof *out);
    if (!ids || !out) { *reason = "out of memory"; goto fail; }
    uint32_t type[4][5] = {{0}}, ptr[4][5] = {{0}}, variables[16] = {0}, pending = mask;
    size_t pos = 5;
    memcpy(out, words, pos * sizeof *out);
    int in_functions = 0;
    for (size_t i = 5; i < count;) {
        const uint32_t *w = words + i;
        uint32_t n = w[0] >> 16, op = w[0] & 0xffff;
        if (!n || n > count - i) goto fail;
        if (!in_functions && op == Function) {
            if (pending) { *reason = "location has no standalone input variable"; goto fail; }
            // All the module's type IDs are now known. Reuse existing scalar, vector
            // and pointer types; declare any missing ones before the moved variables.
            for (int loc = 0; loc < 16; loc++) if (variables[loc]) {
                uint32_t var = variables[loc], c = ids[var].fetch, width = ids[var].width;
                if (!type[c][1]) {
                    if (c == NVN_CT_FLOAT + 1) {
                        uint32_t v[] = {3u << 16 | TypeFloat, type[c][1] = bound++, 32};
                        memcpy(out + pos, v, sizeof v); pos += 3;
                    } else {
                        uint32_t v[] = {4u << 16 | TypeInt, type[c][1] = bound++, 32, c == NVN_CT_INT + 1};
                        memcpy(out + pos, v, sizeof v); pos += 4;
                    }
                }
                if (!type[c][width]) {
                    uint32_t v[] = {4u << 16 | TypeVector, type[c][width] = bound++, type[c][1], width};
                    memcpy(out + pos, v, sizeof v); pos += 4;
                }
                for (uint32_t k = 0; k < 2; k++) {
                    uint32_t size = k ? width : 1;  // pointer to the vector, and to an accessed component
                    if (!ptr[c][size]) {
                        uint32_t v[] = {4u << 16 | TypePointer, ptr[c][size] = bound++, Input, type[c][size]};
                        memcpy(out + pos, v, sizeof v); pos += 4;
                    }
                }
                uint32_t v[] = {4u << 16 | Variable, ptr[c][width], var, Input};
                memcpy(out + pos, v, sizeof v); pos += 4;
            }
            in_functions = 1;
        }
        if (!in_functions) {
            if (((op == TypeInt && n == 4) || (op == TypeFloat && n == 3)) && w[2] == 32 && w[1] < words[3]) {
                uint8_t c = (op == TypeFloat ? NVN_CT_FLOAT : w[3] ? NVN_CT_INT : NVN_CT_UINT) + 1;
                ids[w[1]].cls = c; ids[w[1]].width = 1; type[c][1] = w[1];
            } else if (op == TypeVector && n == 4 && w[1] < words[3] && w[2] < words[3] &&
                       ids[w[2]].cls && w[3] >= 2 && w[3] <= 4) {
                ids[w[1]].cls = ids[w[2]].cls; ids[w[1]].width = w[3];
                type[ids[w[1]].cls][w[3]] = w[1];
            } else if (op == TypePointer && n == 4 && w[2] == Input && w[1] < words[3] && w[3] < words[3]) {
                ids[w[1]].pointee = w[3];
                if (ids[w[3]].cls) ptr[ids[w[3]].cls][ids[w[3]].width] = w[1];
            } else if (op == Decorate && n == 4 && w[2] == Location && w[1] < words[3] && w[3] < 16) {
                ids[w[1]].location = w[3] + 1;
            } else if (op == Variable && n >= 4 && w[3] == Input && w[1] < words[3] && w[2] < words[3] && ids[w[2]].location) {
                unsigned loc = ids[w[2]].location - 1, c = (mask >> (2 * loc)) & 3;
                if (c) {
                    vin_id value = ids[ids[w[1]].pointee];
                    if (!value.width || value.cls != reflected[loc] + 1 || n != 4 || variables[loc]) {
                        *reason = "expected one reflected 32-bit scalar or vector at the location"; goto fail;
                    }
                    ids[w[2]].fetch = c; ids[w[2]].width = value.width; variables[loc] = w[2];
                    pending &= ~(3u << (2 * loc));
                    i += n; continue;
                }
            }
        } else if ((op == AccessChain || op == InBoundsAccessChain) && n >= 4 && w[3] < words[3] && ids[w[3]].fetch) {
            if (n != 5 || ids[w[3]].width == 1 || w[2] >= words[3]) { *reason = "unsupported vertex component access"; goto fail; }
            uint8_t c = ids[w[3]].fetch; ids[w[2]].fetch = c; ids[w[2]].width = 1;
            memcpy(out + pos, w, n * 4); out[pos + 1] = ptr[c][1]; pos += n; i += n; continue;
        } else if (op == Load && n >= 4 && w[3] < words[3] && ids[w[3]].fetch) {
            vin_id p = ids[w[3]];
            uint32_t raw = bound++;
            memcpy(out + pos, w, n * 4); out[pos + 1] = type[p.fetch][p.width]; out[pos + 2] = raw; pos += n;
            uint32_t cast[] = {4u << 16 | Bitcast, w[1], w[2], raw};
            memcpy(out + pos, cast, sizeof cast); pos += 4; i += n; continue;
        } else {
            // A converted pointer may only feed the loads and component accesses above.
            // This conservative check can also reject a literal equal to a pointer ID.
            for (uint32_t j = 1; j < n; j++) if (w[j] < words[3] && ids[w[j]].fetch) {
                *reason = "unrecognized use of a vertex input pointer"; goto fail;
            }
        }
        memcpy(out + pos, w, n * 4); pos += n; i += n;
    }
    if (!in_functions || bound > 0x3fffff) goto fail;
    free(ids); out[3] = bound; *out_count = pos; return out;
fail:
    free(ids); free(out); return NULL;
}
