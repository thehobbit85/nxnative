// Reduce the actual contributing texels with ordinary point samplers. This
// keeps min/max filtering independent of optional native reduction support.
#pragma once
#include "spv_texel_bias.h"

typedef struct {
    uint32_t binding, minimum, min_linear, mag_linear, element;
    float min_lod, max_lod, bias, max_bias;
} sample_reduction;
typedef struct {
    uint32_t type, scalar, lanes, image, binding, dimension, array, index;
    unsigned is_type:1, integer:1;
} reduction_id;
typedef struct {
    uint32_t minimum, min_linear, mag_linear, min_lod, max_lod, bias, max_bias, active;
} reduction_filter;
typedef struct {
    texel_module code;
    reduction_id *ids;
    uint32_t bound, glsl, floating, float2, float3, float4, sint, int2, int3, bool2, bool3;
    int failed;
} reduction_module;

static void reduction_append(reduction_module *m, texel_words *out, uint32_t op, const uint32_t *args, size_t count) {
    uint32_t words[16];
    if (count >= sizeof words / sizeof *words) { m->failed = 1; return; }
    words[0] = (uint32_t)((count + 1) << 16) | op;
    memcpy(words + 1, args, count * sizeof *args);
    if (!texel_append(out, words, count + 1)) m->failed = 1;
}
#define REDUCTION_GLOBAL(m, op, ...) reduction_append(m, &(m)->code.globals, op, (uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__})/4)
static uint32_t reduction_value(reduction_module *m, uint32_t op, uint32_t type, const uint32_t *args, size_t count) {
    uint32_t operands[15], result = m->code.next++;
    if (count > 13) { m->failed = 1; return 0; }
    operands[0] = type; operands[1] = result;
    memcpy(operands + 2, args, count * sizeof *args);
    reduction_append(m, &m->code.body, op, operands, count + 2);
    return result;
}
#define REDUCTION_VALUE(m, op, type, ...) reduction_value(m, op, type, (uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__})/4)
#define REDUCTION_EXT(m, type, op, ...) REDUCTION_VALUE(m, 12, type, (m)->glsl, op, __VA_ARGS__)

static uint32_t reduction_float(reduction_module *m, float value) {
    uint32_t bits; memcpy(&bits, &value, sizeof bits);
    uint32_t id = texel_constant(&m->code, m->floating, bits);
    if (!id) m->failed = 1;
    return id;
}
static uint32_t reduction_vector(reduction_module *m, uint32_t scalar, unsigned lanes) {
    for (uint32_t id = 1; id < m->bound; id++)
        if (m->ids[id].scalar == scalar && m->ids[id].lanes == lanes) return id;
    uint32_t id = m->code.next++;
    REDUCTION_GLOBAL(m, 23, id, scalar, lanes);
    return id;
}
static uint32_t reduction_splat(reduction_module *m, uint32_t type, uint32_t value, unsigned lanes) {
    uint32_t components[] = {value, value, value, value};
    return reduction_value(m, 80, type, components, lanes);
}
static uint32_t reduction_mask(reduction_module *m, uint32_t condition, unsigned lanes) {
    uint32_t type = lanes == 2 ? m->bool2 : lanes == 3 ? m->bool3 : m->code.bool4;
    return reduction_splat(m, type, condition, lanes);
}
static uint32_t reduction_component(reduction_module *m, uint32_t value, unsigned lane) {
    return REDUCTION_VALUE(m, 81, m->floating, value, lane);
}

static int reduction_filter_for(reduction_module *m, reduction_id sampled, const sample_reduction *filters,
                                size_t count, reduction_filter *out) {
    const sample_reduction *first = filters + sampled.binding - 1;
    uint32_t index_type = sampled.index < m->bound ? m->ids[sampled.index].type : 0;
    if (sampled.index && (!index_type || index_type >= m->bound || !m->ids[index_type].integer)) return 0;
    uint32_t *fields[] = {&out->minimum, &out->min_linear, &out->mag_linear, &out->min_lod,
                         &out->max_lod, &out->bias, &out->max_bias};
    memset(out, 0, sizeof *out);
    if (sampled.index) {
        out->active = m->code.next++;
        REDUCTION_GLOBAL(m, 42, m->code.boolean, out->active); // OpConstantFalse
    }
    for (size_t i = sampled.binding - 1; i < count; i++) {
        const sample_reduction *f = filters + i;
        if (f->binding != first->binding) continue;
        uint32_t match = 0;
        if (sampled.index) {
            uint32_t element = texel_constant(&m->code, index_type, f->element);
            if (!element) return 0;
            match = REDUCTION_VALUE(m, 170, m->code.boolean, sampled.index, element);
            out->active = REDUCTION_VALUE(m, 166, m->code.boolean, out->active, match); // OpLogicalOr
        }
        const float values[] = {f->minimum, f->min_linear, f->mag_linear, f->min_lod, f->max_lod, f->bias, f->max_bias};
        for (unsigned field = 0; field < sizeof fields / sizeof *fields; field++) {
            uint32_t value = reduction_float(m, values[field]);
            *fields[field] = match && *fields[field] ? REDUCTION_VALUE(m, 169, m->floating, match, value, *fields[field]) : value;
        }
        if (!sampled.index) break;
    }
    return !m->failed;
}

static uint32_t reduction_combine(reduction_module *m, uint32_t a, uint32_t b, uint32_t minimum) {
    uint32_t low = REDUCTION_EXT(m, m->float4, 37, a, b);
    uint32_t high = REDUCTION_EXT(m, m->float4, 40, a, b);
    return REDUCTION_VALUE(m, 169, m->float4, minimum, low, high);
}

static int reduction_finish(reduction_module *m, const uint32_t *p, unsigned len, uint32_t reduced, uint32_t active) {
    if (active) {
        // Weighted-average elements keep their original instruction and native
        // sampler. Only a selected min/max element consumes the reduced value.
        uint32_t original[16];
        if (len > sizeof original / sizeof *original) return 0;
        memcpy(original, p, len * sizeof *p);
        original[2] = m->code.next++;
        if (!texel_append(&m->code.body, original, len)) return 0;
        uint32_t mask = p[1] == m->floating ? active : reduction_mask(m, active, p[1] == m->float4 ? 4 : 2);
        reduced = REDUCTION_VALUE(m, 169, p[1], mask, reduced, original[2]);
    }
    uint32_t copy[] = {p[1], p[2], reduced};
    reduction_append(m, &m->code.body, 83, copy, 3);
    return !m->failed;
}

static uint32_t reduction_level(reduction_module *m, uint32_t sampled, uint32_t image, uint32_t coordinate,
                                uint32_t level, uint32_t linear, unsigned dimensions, unsigned array, uint32_t minimum,
                                uint32_t offset_mask, uint32_t offset, uint32_t reference) {
    uint32_t spatial = dimensions == 3 ? m->float3 : m->float2;
    uint32_t integer_level = REDUCTION_VALUE(m, 110, m->sint, level); // OpConvertFToS
    uint32_t size = REDUCTION_VALUE(m, 103, array || dimensions == 3 ? m->int3 : m->int2, image, integer_level);
    if (array) size = REDUCTION_VALUE(m, 79, m->int2, size, size, 0, 1);
    size = REDUCTION_VALUE(m, 111, spatial, size); // OpConvertSToF
    uint32_t position = array ? REDUCTION_VALUE(m, 79, m->float2, coordinate, coordinate, 0, 1) : coordinate;
    uint32_t layer = array ? reduction_component(m, coordinate, 2) : 0;
    uint32_t zero = reduction_float(m, 0), half = reduction_float(m, .5f), one = reduction_float(m, 1);
    uint32_t halves = reduction_splat(m, spatial, half, dimensions);
    uint32_t zeros = reduction_splat(m, spatial, zero, dimensions);
    uint32_t scaled = REDUCTION_VALUE(m, 133, spatial, position, size);
    uint32_t centred = REDUCTION_VALUE(m, 131, spatial, scaled, halves);
    uint32_t base = REDUCTION_EXT(m, spatial, 8, centred); // GLSL Floor
    uint32_t fraction = REDUCTION_VALUE(m, 131, spatial, centred, base);
    uint32_t nearest = REDUCTION_EXT(m, spatial, 8, scaled);
    uint32_t mask = reduction_mask(m, linear, dimensions);
    base = REDUCTION_VALUE(m, 169, spatial, mask, base, nearest);
    fraction = REDUCTION_VALUE(m, 169, spatial, mask, fraction, zeros);
    uint32_t weights[3];
    for (unsigned axis = 0; axis < dimensions; axis++) {
        uint32_t component = reduction_component(m, fraction, axis);
        weights[axis] = REDUCTION_VALUE(m, 186, m->code.boolean, component, zero);
    }
    base = REDUCTION_VALUE(m, 129, spatial, base, halves);
    uint32_t result = 0;
    for (unsigned tap = 0; tap < (1u << dimensions); tap++) {
        uint32_t components[] = {tap & 1 ? one : zero, tap & 2 ? one : zero, tap & 4 ? one : zero};
        uint32_t neighbor = reduction_value(m, 80, spatial, components, dimensions);
        uint32_t point = REDUCTION_VALUE(m, 129, spatial, base, neighbor);
        point = REDUCTION_VALUE(m, 136, spatial, point, size);
        if (array) {
            uint32_t x = reduction_component(m, point, 0), y = reduction_component(m, point, 1);
            point = REDUCTION_VALUE(m, 80, m->float3, x, y, layer);
        }
        uint32_t operands[6] = {sampled, point};
        unsigned count = 2;
        if (reference) operands[count++] = reference;
        operands[count++] = 2 | offset_mask; operands[count++] = level;
        if (offset) operands[count++] = offset;
        uint32_t value = reduction_value(m, reference ? 90 : 88, reference ? m->floating : m->float4, operands, count);
        // Depth comparisons produce each point's 0/1 value before reduction,
        // as specified by EXT_texture_filter_minmax.
        if (reference) value = reduction_splat(m, m->float4, value, 4);
        if (!tap) { result = value; continue; }
        uint32_t weight = 0;
        for (unsigned axis = 0; axis < dimensions; axis++) if (tap & (1u << axis))
            weight = weight ? REDUCTION_VALUE(m, 167, m->code.boolean, weight, weights[axis]) : weights[axis];
        uint32_t reduced = reduction_combine(m, result, value, minimum);
        result = REDUCTION_VALUE(m, 169, m->float4, reduction_mask(m, weight, 4), reduced, result);
    }
    return result;
}

static uint32_t reduction_gradient_lod(reduction_module *m, uint32_t image, uint32_t dx, uint32_t dy,
                                       unsigned dimensions, unsigned array) {
    uint32_t spatial = dimensions == 3 ? m->float3 : m->float2;
    uint32_t zero = texel_constant(&m->code, m->sint, 0);
    if (!zero) m->failed = 1;
    uint32_t size = REDUCTION_VALUE(m, 103, array || dimensions == 3 ? m->int3 : m->int2, image, zero);
    if (array) size = REDUCTION_VALUE(m, 79, m->int2, size, size, 0, 1);
    size = REDUCTION_VALUE(m, 111, spatial, size);
    dx = REDUCTION_VALUE(m, 133, spatial, dx, size);
    dy = REDUCTION_VALUE(m, 133, spatial, dy, size);
    uint32_t x = REDUCTION_VALUE(m, 148, m->floating, dx, dx); // OpDot
    uint32_t y = REDUCTION_VALUE(m, 148, m->floating, dy, dy);
    uint32_t squared = REDUCTION_EXT(m, m->floating, 40, x, y);
    uint32_t logarithm = REDUCTION_EXT(m, m->floating, 30, squared); // GLSL Log2
    return REDUCTION_VALUE(m, 133, m->floating, logarithm, reduction_float(m, .5f));
}

static int reduction_sample(reduction_module *m, const uint32_t *p, unsigned len, const reduction_filter *filter) {
    // Other sample forms are diagnosed before their sampler can become point
    // filtered. Offsets stay in texel units and use native sampler addressing.
    unsigned opcode = p[0] & 0xffff;
    int comparison = opcode == 89 || opcode == 90 || opcode == 93 || opcode == 94;
    if (len < 5u + comparison || p[1] != (comparison ? m->floating : m->float4) || p[3] >= m->bound || p[4] >= m->bound) return 0;
    uint32_t reference = comparison ? p[5] : 0;
    if (comparison && (!reference || reference >= m->bound || m->ids[reference].type != m->floating)) return 0;
    int implicit = opcode == 87 || opcode == 89 || opcode == 91 || opcode == 93;
    int projected = opcode >= 91 && opcode <= 94;
    unsigned mask_at = 5 + comparison;
    unsigned operands = len > mask_at ? p[mask_at] : 0, gradient = operands & 4, offset_mask = operands & (8 | 16);
    unsigned allowed = (implicit ? 1u : 6u) | 8u | 16u;
    if ((operands & ~allowed) || offset_mask == (8 | 16) ||
        (!implicit && (operands & 6) != (gradient ? 4u : 2u))) return 0;
    unsigned next = len > mask_at ? mask_at + 1 : mask_at;
    unsigned required = (implicit ? !!(operands & 1) : 1u + !!gradient) + !!offset_mask;
    if (len - next != required) return 0;
    for (unsigned arg = mask_at + 1; arg < len; arg++) if (p[arg] >= m->bound) return 0;
    uint32_t shader_bias = implicit && (operands & 1) ? p[next++] : 0;
    uint32_t explicit_lod = implicit ? 0 : p[next++];
    uint32_t dy = gradient ? p[next++] : 0;
    uint32_t offset = offset_mask ? p[next++] : 0;
    if (shader_bias && m->ids[shader_bias].type != m->floating) return 0;
    uint32_t sampled_type = m->ids[p[3]].type;
    uint32_t image_type = m->ids[sampled_type].image;
    if (!image_type || m->ids[image_type].dimension < 1 || m->ids[image_type].dimension > 2 || m->ids[image_type].array > 1) return 0;
    unsigned dimensions = m->ids[image_type].dimension + 1;
    unsigned array = m->ids[image_type].array;
    if (dimensions == 3 && array) return 0;
    uint32_t spatial = dimensions == 3 ? m->float3 : m->float2;
    if (!implicit && (m->ids[explicit_lod].type != (gradient ? spatial : m->floating) ||
        (gradient && m->ids[dy].type != spatial))) return 0;
    if (offset && m->ids[offset].type != (dimensions == 3 ? m->int3 : m->int2)) return 0;
    uint32_t coordinate = p[4], coordinate_type = m->ids[coordinate].type;
    if (projected) {
        if (array || m->ids[coordinate_type].scalar != m->floating || m->ids[coordinate_type].lanes <= dimensions) return 0;
        uint32_t position = dimensions == 3 ? REDUCTION_VALUE(m, 79, m->float3, coordinate, coordinate, 0, 1, 2) :
                                             REDUCTION_VALUE(m, 79, m->float2, coordinate, coordinate, 0, 1);
        // SPIR-V places q immediately after the spatial components. Any
        // extra components follow q and do not affect the projection.
        uint32_t q = reduction_component(m, coordinate, dimensions);
        uint32_t divisor = reduction_splat(m, spatial, q, dimensions);
        coordinate = REDUCTION_VALUE(m, 136, spatial, position, divisor);
        if (comparison) reference = REDUCTION_VALUE(m, 136, m->floating, reference, q);
    } else if (coordinate_type != (array ? m->float3 : spatial)) {
        unsigned lanes = dimensions + array;
        if (m->ids[coordinate_type].scalar != m->floating || m->ids[coordinate_type].lanes < lanes) return 0;
        // The SPIR-V coordinate can contain unused trailing components. GLSL
        // shadow samples commonly retain the separately supplied reference.
        coordinate = lanes == 3 ? REDUCTION_VALUE(m, 79, m->float3, coordinate, coordinate, 0, 1, 2) :
                                  REDUCTION_VALUE(m, 79, m->float2, coordinate, coordinate, 0, 1);
    }
    uint32_t image = REDUCTION_VALUE(m, 100, image_type, p[3]);
    uint32_t levels = REDUCTION_VALUE(m, 106, m->sint, image);
    uint32_t last = REDUCTION_VALUE(m, 111, m->floating, levels);
    uint32_t zero = reduction_float(m, 0), one = reduction_float(m, 1);
    last = REDUCTION_VALUE(m, 131, m->floating, last, one);
    uint32_t lower = filter->min_lod, upper = filter->max_lod;
    uint32_t lod = explicit_lod;
    if (implicit) {
        uint32_t xy = array ? REDUCTION_VALUE(m, 79, m->float2, coordinate, coordinate, 0, 1) : coordinate;
        uint32_t query = REDUCTION_VALUE(m, 105, m->float2, p[3], xy);
        // QueryLod.y gives the driver's unclamped derivative LOD. The point
        // sampler has zero bias, so the original sampler and instruction bias
        // can be applied exactly once before selecting the contributing mips.
        lod = reduction_component(m, query, 1);
    } else if (gradient) lod = reduction_gradient_lod(m, image, explicit_lod, dy, dimensions, array);
    uint32_t bias = filter->bias;
    if (shader_bias) {
        bias = REDUCTION_VALUE(m, 129, m->floating, bias, shader_bias);
        uint32_t negative_limit = REDUCTION_VALUE(m, 127, m->floating, filter->max_bias); // OpFNegate
        bias = REDUCTION_EXT(m, m->floating, 43, bias, negative_limit, filter->max_bias);
    }
    lod = REDUCTION_VALUE(m, 129, m->floating, lod, bias);
    lod = REDUCTION_EXT(m, m->floating, 43, lod, lower, upper);
    uint32_t minify = REDUCTION_VALUE(m, 186, m->code.boolean, lod, zero);
    uint32_t min_filter = filter->min_linear, mag_filter = filter->mag_linear;
    uint32_t selected = REDUCTION_VALUE(m, 169, m->floating, minify, min_filter, mag_filter);
    uint32_t linear = REDUCTION_VALUE(m, 186, m->code.boolean, selected, zero);
    lod = REDUCTION_EXT(m, m->floating, 43, lod, zero, last);
    uint32_t first = REDUCTION_EXT(m, m->floating, 8, lod), second = REDUCTION_EXT(m, m->floating, 9, lod);
    uint32_t minimum = REDUCTION_VALUE(m, 186, m->code.boolean, filter->minimum, zero);
    minimum = reduction_mask(m, minimum, 4);
    uint32_t a = reduction_level(m, p[3], image, coordinate, first, linear, dimensions, array, minimum, offset_mask, offset, reference);
    uint32_t b = reduction_level(m, p[3], image, coordinate, second, linear, dimensions, array, minimum, offset_mask, offset, reference);
    uint32_t value = reduction_combine(m, a, b, minimum);
    if (comparison) value = reduction_component(m, value, 0);
    return reduction_finish(m, p, len, value, filter->active);
}

static int reduction_query(reduction_module *m, const uint32_t *p, unsigned len, const reduction_filter *filter) {
    if (len != 5 || p[1] != m->float2 || p[3] >= m->bound || p[4] >= m->bound) return 0;
    uint32_t image_type = m->ids[m->ids[p[3]].type].image;
    if (!image_type || m->ids[image_type].dimension < 1 || m->ids[image_type].dimension > 2 || m->ids[image_type].array > 1) return 0;
    if (m->ids[p[4]].type != (m->ids[image_type].dimension == 2 ? m->float3 : m->float2)) return 0;
    uint32_t query = REDUCTION_VALUE(m, 105, m->float2, p[3], p[4]);
    uint32_t raw = reduction_component(m, query, 1);
    raw = REDUCTION_VALUE(m, 129, m->floating, raw, filter->bias);
    uint32_t lod = REDUCTION_EXT(m, m->floating, 43, raw, filter->min_lod, filter->max_lod);
    uint32_t image = REDUCTION_VALUE(m, 100, image_type, p[3]);
    uint32_t levels = REDUCTION_VALUE(m, 106, m->sint, image);
    uint32_t last = REDUCTION_VALUE(m, 111, m->floating, levels);
    last = REDUCTION_VALUE(m, 131, m->floating, last, reduction_float(m, 1));
    lod = REDUCTION_EXT(m, m->floating, 43, lod, reduction_float(m, 0), last);
    // Metal's reduction samplers select mips linearly, so the accessed mip
    // retains its fraction. The second component is the unclamped LOD.
    uint32_t result = REDUCTION_VALUE(m, 80, m->float2, lod, raw);
    return reduction_finish(m, p, len, result, filter->active);
}

static uint32_t *spv_sample_reduction(const uint32_t *words, size_t count, const sample_reduction *filters,
                                     size_t nf, size_t *out_count, const char **reason) {
    *reason = "invalid sampler reduction module";
    if (count < 5 || words[0] != 0x07230203 || !words[3] || words[3] > 0x3fffff) return NULL;
    reduction_module m = {.bound=words[3], .code={.next=words[3]}};
    texel_words out = {0}; size_t functions = count;
    int has_queries = 0, needs_glsl = 0;
    m.ids = calloc(m.bound, sizeof *m.ids);
    if (!m.ids) goto fail;
    for (size_t i = 5; i < count;) {
        const uint32_t *p = words + i; unsigned len = p[0] >> 16, op = p[0] & 0xffff;
        if (!len || len > count - i) goto fail;
        if (op == 17 && len == 2 && p[1] == 50) has_queries = 1;
        if (op == 11 && len == 6 && !memcmp(p + 2, "GLSL.std.450", 12)) m.glsl = p[1];
        if (op == 71 && len == 4 && p[1] < m.bound && p[2] == 33)
            for (size_t f = 0; f < nf; f++) if (filters[f].binding == p[3] && !m.ids[p[1]].binding)
                m.ids[p[1]].binding = (uint32_t)f + 1;
        if (op >= 19 && op <= 33 && len >= 2 && p[1] < m.bound) {
            reduction_id *id = m.ids + p[1]; id->is_type = 1;
            if (op == 20) m.code.boolean = p[1];
            if (op == 21 && len == 4 && p[2] == 32) { id->integer = 1; if (p[3] == 1) m.sint = p[1]; }
            if (op == 22 && len == 3 && p[2] == 32) m.floating = p[1];
            if (op == 23 && len == 4) { id->scalar = p[2]; id->lanes = p[3]; }
            if (op == 25 && len >= 9) { id->dimension = p[3]; id->array = p[5]; }
            if (op == 27 && len == 3 && p[2] < m.bound) id->image = p[2];
        } else if (len >= 3 && p[1] < m.bound && m.ids[p[1]].is_type && p[2] < m.bound) m.ids[p[2]].type = p[1];
        if (op == 54 && functions == count) functions = i;
        if ((op == 61 || op == 83 || op == 100) && len >= 4 && p[2] < m.bound && p[3] < m.bound) {
            m.ids[p[2]].binding = m.ids[p[3]].binding;
            m.ids[p[2]].index = m.ids[p[3]].index;
        }
        if (op == 86 && len == 5 && p[2] < m.bound && p[4] < m.bound) {
            m.ids[p[2]].binding = m.ids[p[4]].binding;
            m.ids[p[2]].index = m.ids[p[4]].index;
        }
        if ((op == 65 || op == 66) && len == 5 && p[2] < m.bound && p[3] < m.bound && p[4] < m.bound &&
            m.ids[p[3]].binding && !m.ids[p[3]].index) {
            m.ids[p[2]].binding = m.ids[p[3]].binding;
            m.ids[p[2]].index = p[4];
        }
        i += len;
    }
    if (!m.floating || functions == count) goto fail;
    if (!m.code.boolean) { m.code.boolean = m.code.next++; REDUCTION_GLOBAL(&m, 20, m.code.boolean); }
    if (!m.sint) { m.sint = m.code.next++; REDUCTION_GLOBAL(&m, 21, m.sint, 32, 1); }
    if (!m.glsl) { m.glsl = m.code.next++; needs_glsl = 1; }
    m.float2 = reduction_vector(&m, m.floating, 2); m.float3 = reduction_vector(&m, m.floating, 3); m.float4 = reduction_vector(&m, m.floating, 4);
    m.int2 = reduction_vector(&m, m.sint, 2); m.int3 = reduction_vector(&m, m.sint, 3);
    m.bool2 = reduction_vector(&m, m.code.boolean, 2); m.bool3 = reduction_vector(&m, m.code.boolean, 3);
    m.code.bool4 = reduction_vector(&m, m.code.boolean, 4);
    for (size_t i = functions; i < count;) {
        const uint32_t *p = words + i; unsigned len = p[0] >> 16, op = p[0] & 0xffff;
        unsigned from = len, end = len;
        switch (op) {
            case 57: case 169: from = 4; break; // function call or select
            case 245: from = 3; break; // phi, including back-edge inputs
            case 62: case 63: case 64: from = 1; end = 3; break;
            case 65: case 66: case 67: case 70: from = 3; end = 4; break;
            case 254: from = 1; break;
        }
        if ((op == 65 || op == 66) && len == 5 && p[2] < m.bound && m.ids[p[2]].binding) from = end;
        if (end > len) goto fail;
        for (unsigned arg = from; arg < end; arg++) if (p[arg] < m.bound && m.ids[p[arg]].binding) {
            *reason = "reduction sampler escapes its direct binding"; goto fail;
        }
        uint32_t binding = len >= 4 && p[3] < m.bound ? m.ids[p[3]].binding : 0;
        if (binding && ((op >= 87 && op <= 94) || op == 96 || op == 97 || op == 105 || (op >= 305 && op <= 315))) {
            *reason = "reduction currently needs a direct 2D/3D float sample or LOD query";
            reduction_filter selected;
            if (!reduction_filter_for(&m, m.ids[p[3]], filters, nf, &selected)) goto fail;
            if (op == 105) {
                if (!reduction_query(&m, p, len, &selected)) goto fail;
            } else if ((op < 87 || op > 94) || !reduction_sample(&m, p, len, &selected)) goto fail;
        } else if (!texel_append(&m.code.body, p, len)) goto fail;
        i += len;
    }
    if (m.failed || m.code.next > 0x3fffff || !texel_append(&out, words, 5)) goto fail;
    if (!has_queries && !texel_append(&out, (uint32_t[]){2u<<16|17, 50}, 2)) goto fail;
    for (size_t i = 5; i < functions;) {
        unsigned len = words[i] >> 16;
        if (needs_glsl && (words[i] & 0xffff) == 14) {
            uint32_t import[] = {6u<<16|11, m.glsl, 0, 0, 0, 0};
            memcpy(import + 2, "GLSL.std.450", 12);
            if (!texel_append(&out, import, 6)) goto fail;
        }
        if (!texel_append(&out, words + i, len)) goto fail;
        i += len;
    }
    if (!texel_append(&out, m.code.globals.w, m.code.globals.n) || !texel_append(&out, m.code.body.w, m.code.body.n)) goto fail;
    out.w[3] = m.code.next; *out_count = out.n; *reason = NULL;
    free(m.ids); free(m.code.globals.w); free(m.code.body.w); return out.w;
fail:
    free(m.ids); free(m.code.globals.w); free(m.code.body.w); free(out.w); return NULL;
}
#undef REDUCTION_VALUE
#undef REDUCTION_GLOBAL
#undef REDUCTION_EXT
