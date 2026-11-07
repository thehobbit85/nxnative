// Keep guest 1D textures on the same one-row 2D allocation used by translated
// normalized samples. Adapt older native-1D modules at load time, including
// storage, atomics and queries, so both interfaces share GPU writes directly.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t type, scalar, lanes, image, alias;
    unsigned is_type:1, floating:1, constant:1, specialization:1;
} row_id;
typedef struct { uint32_t *words; size_t count, capacity; } row_words;
typedef struct {
    row_id *ids;
    uint32_t bound, next;
    row_words globals, extra, body, types;
} row_module;

static int row_append(row_words *out, const uint32_t *words, size_t count) {
    if (count > SIZE_MAX / sizeof *words - out->count) return 0;
    size_t end = out->count + count;
    if (end > out->capacity) {
        size_t capacity = end;
        if (out->capacity <= SIZE_MAX / (2 * sizeof *words) && capacity < 2 * out->capacity)
            capacity = 2 * out->capacity;
        uint32_t *grown = realloc(out->words, capacity * sizeof *words);
        if (!grown) return 0;
        out->words = grown; out->capacity = capacity;
    }
    memcpy(out->words + out->count, words, count * sizeof *words);
    out->count = end;
    return 1;
}
#define ROW_OP(dst, op, ...) do { \
    uint32_t ins[] = {0, __VA_ARGS__}; size_t len = sizeof ins / sizeof *ins; \
    ins[0] = (uint32_t)(len << 16) | (op); if (!row_append(dst, ins, len)) goto fail; \
} while (0)

static uint32_t row_vector(row_module *m, uint32_t scalar, unsigned lanes) {
    for (uint32_t id = 1; id < m->bound; id++)
        if (m->ids[id].scalar == scalar && m->ids[id].lanes == lanes) return id;
    for (size_t i = 0; i < m->extra.count; i += m->extra.words[i] >> 16) {
        uint32_t *p = m->extra.words + i;
        if ((p[0] & 0xffff) == 23 && p[2] == scalar && p[3] == lanes) return p[1];
    }
    uint32_t id = m->next++;
    ROW_OP(&m->extra, 23, id, scalar, lanes);
    return id;
fail:
    return 0;
}
static uint32_t row_constant(row_module *m, uint32_t type, uint32_t value) {
    for (size_t i = 0; i < m->extra.count; i += m->extra.words[i] >> 16) {
        uint32_t *p = m->extra.words + i;
        if ((p[0] & 0xffff) == 43 && p[1] == type && p[3] == value) return p[2];
    }
    uint32_t id = m->next++;
    ROW_OP(&m->extra, 43, type, id, value);
    return id;
fail:
    return 0;
}
static uint32_t row_component(row_module *m, uint32_t value, row_id type, unsigned lane) {
    if (type.lanes == 1) return lane == 0 ? value : 0;
    uint32_t id = m->next++;
    ROW_OP(&m->body, 81, type.scalar, id, value, lane); // OpCompositeExtract
    return id;
fail:
    return 0;
}

// Insert Y between X and the optional array layer. Normalized samples use
// the row centre; integer coordinates, offsets and derivatives use zero.
static uint32_t row_coordinate(row_module *m, uint32_t value, unsigned lanes, int centre, int projected, int constant) {
    if (value >= m->bound || m->ids[value].type >= m->bound) return 0;
    row_id type = m->ids[m->ids[value].type];
    if (!type.scalar || type.lanes < lanes || lanes > 2) return 0;
    uint32_t vector = row_vector(m, type.scalar, lanes + 1);
    uint32_t y = row_constant(m, type.scalar, centre ? 0x3f000000 : 0);
    if (!vector || !y || (centre && !m->ids[type.scalar].floating)) return 0;
    uint32_t result = m->next++;
    if (constant) {
        if (lanes != 1 || type.lanes != 1 || !m->ids[value].constant) return 0;
        ROW_OP(&m->extra, m->ids[value].specialization ? 51 : 44, vector, result, value, y);
        return result;
    }
    uint32_t x = row_component(m, value, type, 0);
    if (!x) return 0;
    if (lanes == 1) {
        ROW_OP(&m->body, 80, vector, result, x, y);
    } else {
        uint32_t last = row_component(m, value, type, projected ? type.lanes - 1 : 1);
        if (!last) return 0;
        if (projected) {
            uint32_t scaled = m->next++;
            ROW_OP(&m->body, 133, type.scalar, scaled, y, last); // OpFMul
            y = scaled;
        }
        ROW_OP(&m->body, 80, vector, result, x, y, last);
    }
    return result;
fail:
    return 0;
}

static int row_image_operands(row_module *m, uint32_t *p, unsigned len, unsigned mask_at) {
    if (mask_at >= len) return 1;
    uint32_t mask = p[mask_at];
    if (mask & ~0x7fdfu) return 0; // ConstOffsets/Offsets are gather-only, hence never 1D.
    unsigned arg = mask_at + 1;
    for (unsigned bit = 1; bit <= 0x4000; bit <<= 1) {
        if (!(mask & bit)) continue;
        unsigned count = bit == 4 ? 2 : bit <= 0x200 ? 1 : 0;
        if (count > len - arg) return 0;
        if (bit == 4 || bit == 8 || bit == 16) {
            for (unsigned i = 0; i < count; i++) {
                p[arg + i] = row_coordinate(m, p[arg + i], 1, 0, 0, bit == 8);
                if (!p[arg + i]) return 0;
            }
        }
        arg += count;
    }
    return arg == len;
}

// Image, sampled-image and pointer types can become identical to an existing
// 2D declaration. Canonicalize those type IDs and their function signatures.
static int row_global(row_module *m, const uint32_t *input, unsigned len, unsigned op) {
    unsigned minimum = op == 25 ? 9 : op == 28 || op == 32 ? 4 :
        op == 27 || op == 29 || op == 33 ? 3 : 2;
    if (op >= 19 && op <= 33 && (len < minimum || input[1] >= m->bound)) return 0;
    uint32_t *p = malloc(len * sizeof *p);
    if (!p) return 0;
    memcpy(p, input, len * sizeof *p);
    int type = op >= 19 && op <= 33;
    if (op == 25 && p[3] == 0) p[3] = 1; // Dim1D -> Dim2D
    unsigned first = op == 32 ? 3 : 2;
    unsigned end = op == 30 || op == 33 ? len : op == 27 || op == 28 || op == 29 || op == 32 ? first + 1 : first;
    for (unsigned i = first; i < end; i++) {
        if (p[i] >= m->bound) { free(p); return 0; }
        p[i] = m->ids[p[i]].alias;
    }
    if (!type && len >= 3 && p[1] < m->bound && m->ids[p[1]].is_type) p[1] = m->ids[p[1]].alias;
    int canonical = op == 25 || op == 27 || op == 32 || op == 33;
    if (canonical) {
        for (size_t i = 0; i < m->types.count; i += m->types.words[i] >> 16) {
            uint32_t *old = m->types.words + i;
            if (old[0] == p[0] && !memcmp(old + 2, p + 2, (len - 2) * sizeof *p)) {
                m->ids[p[1]].alias = old[1]; free(p); return 1;
            }
        }
        if (!row_append(&m->types, p, len)) { free(p); return 0; }
    }
    int ok = row_append(&m->globals, p, len);
    free(p); return ok;
}

// NULL with no reason means no native 1D interface was present. Other failures
// reject the module instead of submitting mismatched image declarations.
static uint32_t *spv_one_row(const uint32_t *words, size_t count, size_t *out_count, const char **reason) {
    *reason = "invalid one-row image module";
    if (count < 5 || words[0] != 0x07230203 || !words[3] || words[3] > 0x3fffff) return NULL;
    int affected = 0;
    for (size_t i = 5; i < count;) {
        unsigned len = words[i] >> 16, op = words[i] & 0xffff;
        if (!len || len > count - i) return NULL;
        if (op == 25 && len >= 9 && words[i + 3] == 0) affected = 1;
        i += len;
    }
    if (!affected) { *reason = NULL; return NULL; }
    row_module m = {.bound=words[3], .next=words[3]};
    row_words out = {0}; size_t functions = count;
    m.ids = calloc(m.bound, sizeof *m.ids);
    if (!m.ids) goto fail;
    for (uint32_t id = 0; id < m.bound; id++) m.ids[id].alias = id;
    for (size_t i = 5; i < count;) {
        const uint32_t *p = words + i; unsigned len = p[0] >> 16, op = p[0] & 0xffff;
        if (op >= 19 && op <= 33 && len >= 2 && p[1] < m.bound) {
            row_id *id = m.ids + p[1]; id->is_type = 1;
            if ((op == 21 || op == 22) && len >= 3 && p[2] == 32) {
                id->scalar = p[1]; id->lanes = 1; id->floating = op == 22;
            } else if (op == 23 && len == 4 && p[2] < m.bound) {
                id->scalar = p[2]; id->lanes = p[3];
            } else if (op == 25 && len >= 9 && p[3] == 0) {
                if (p[5] > 1 || p[6]) goto fail;
                id->image = 1 + p[5];
            } else if ((op == 27 || op == 28 || op == 29 || op == 32) && len >= 3) {
                unsigned element = op == 32 ? 3 : 2;
                if (element >= len || p[element] >= m.bound) goto fail;
                id->image = m.ids[p[element]].image;
            }
        } else if (len >= 3 && p[1] < m.bound && m.ids[p[1]].is_type && p[2] < m.bound) {
            m.ids[p[2]].type = p[1];
            if (op >= 41 && op <= 52) {
                m.ids[p[2]].constant = 1; m.ids[p[2]].specialization = op >= 48;
            }
        }
        if (op == 54 && functions == count) functions = i;
        i += len;
    }
    if (functions == count) goto fail;
    for (size_t i = 5; i < functions;) {
        unsigned len = words[i] >> 16;
        if (!row_global(&m, words + i, len, words[i] & 0xffff)) goto fail;
        i += len;
    }
    // Debug names and decorations precede declarations, so their targets can
    // only be remapped after all equivalent types have been identified.
    for (size_t i = 0; i < m.globals.count; i += m.globals.words[i] >> 16) {
        uint32_t *p = m.globals.words + i; unsigned op = p[0] & 0xffff;
        if ((op == 5 || op == 71 || op == 332) && p[1] < m.bound) p[1] = m.ids[p[1]].alias;
        if (op == 74) // OpGroupDecorate targets, after its decoration group ID
            for (unsigned j = 2; j < p[0] >> 16; j++) if (p[j] < m.bound) p[j] = m.ids[p[j]].alias;
    }
    for (size_t i = functions; i < count;) {
        const uint32_t *input = words + i; unsigned len = input[0] >> 16, op = input[0] & 0xffff;
        uint32_t *p = malloc(len * sizeof *p);
        if (!p) goto fail;
        memcpy(p, input, len * sizeof *p);
        if (len >= 3 && p[1] < m.bound && m.ids[p[1]].is_type) p[1] = m.ids[p[1]].alias;
        if (op == 54 && len == 5 && p[4] < m.bound) p[4] = m.ids[p[4]].alias;
        unsigned basic = op >= 305 && op <= 315 ? op - 218 : op == 320 ? 98 : op;
        int coordinate = (basic >= 87 && basic <= 99) || basic == 60 || basic == 105;
        int size = basic == 103 || basic == 104;
        unsigned image_at = basic == 99 ? 1 : 3;
        uint32_t image = (coordinate || size) && len > image_at && p[image_at] < m.bound
            ? m.ids[m.ids[p[image_at]].type].image : 0;
        int ok = 1;
        if (image && size) {
            row_id type = m.ids[input[1]];
            uint32_t vector = type.scalar ? row_vector(&m, type.scalar, image + 1) : 0;
            uint32_t temporary = m.next++;
            p[1] = vector; p[2] = temporary;
            ok = vector && row_append(&m.body, p, len);
            if (ok) {
                if (image == 1) {
                    uint32_t extract[] = {5u<<16 | 81, input[1], input[2], temporary, 0};
                    ok = row_append(&m.body, extract, 5);
                } else {
                    uint32_t shuffle[] = {7u<<16 | 79, input[1], input[2], temporary, temporary, 0, 2};
                    ok = row_append(&m.body, shuffle, 7);
                }
            }
        } else {
            if (image && coordinate) {
                int projected = basic >= 91 && basic <= 94;
                int normalized = (basic >= 87 && basic <= 94) || basic == 105;
                if (len <= image_at + 1 || (projected && image == 2) || basic == 96 || basic == 97) ok = 0;
                if (ok) {
                    // LOD queries use spatial coordinates only, even on an
                    // array image. Its layer never contributes derivatives.
                    unsigned lanes = basic == 105 ? 1 : image + projected;
                    p[image_at + 1] = row_coordinate(&m, p[image_at + 1], lanes, normalized, projected, 0);
                    ok = p[image_at + 1] != 0;
                }
                unsigned mask_at = basic == 99 ? 4 :
                    basic == 89 || basic == 90 || basic == 93 || basic == 94 || basic == 96 || basic == 97 ? 6 : 5;
                if (ok && basic != 60 && basic != 105) ok = row_image_operands(&m, p, len, mask_at);
            }
            if (ok) ok = row_append(&m.body, p, len);
        }
        free(p);
        if (!ok) { *reason = "unsupported one-row image operand"; goto fail; }
        i += len;
    }
    if (m.next > 0x3fffff || !row_append(&out, words, 5) || !row_append(&out, m.globals.words, m.globals.count) ||
        (m.extra.count && !row_append(&out, m.extra.words, m.extra.count)) || !row_append(&out, m.body.words, m.body.count)) goto fail;
    out.words[3] = m.next; *out_count = out.count; *reason = NULL;
    free(m.ids); free(m.globals.words); free(m.extra.words); free(m.body.words); free(m.types.words);
    return out.words;
fail:
    free(m.ids); free(m.globals.words); free(m.extra.words); free(m.body.words); free(m.types.words); free(out.words);
    return NULL;
}
#undef ROW_OP
