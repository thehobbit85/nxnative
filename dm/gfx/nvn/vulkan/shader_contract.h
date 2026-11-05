#ifndef DM_VK_SHADER_CONTRACT_H
#define DM_VK_SHADER_CONTRACT_H
// Versioned offline products: reject incompatible sidecars before creating driver objects.
// spirv-val validates exported modules; this bounded reader checks their runtime contract.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "geometry_contract.h"

// Raster-discard vertex capture: the geometry ABI plus two helper bindings.
typedef struct { uint32_t output_binding, control_binding; } dm_vk_capture;
static const char *dm_vk_capture_line(const char *line, size_t n, dm_vk_capture *capture) {
    uint32_t values[4]; unsigned count = 0;
    if (n < 8 || memcmp(line, "capture ", 8)) return "invalid capture record";
    for (size_t at = 8; at < n;) {
        if (count == 4 || line[at] < '0' || line[at] > '9') return "invalid capture field";
        uint32_t value = 0;
        do {
            if (value > (INT32_MAX - (uint32_t)(line[at] - '0')) / 10) return "invalid capture field";
            value = value * 10 + (uint32_t)(line[at++] - '0');
        } while (at < n && line[at] >= '0' && line[at] <= '9');
        values[count++] = value;
        if (at < n && (line[at++] != ' ' || at == n)) return "invalid capture field";
    }
    if (count != 4 || values[0] != 144 || values[1] != 6 || values[2] == values[3]) return "invalid capture interface";
    capture->output_binding = values[2]; capture->control_binding = values[3];
    return NULL;
}

// Four NVN capture buffers, each an ordered list of generic attribute slices.
// Duplicate locations (including duplicates in different buffers) are valid.
typedef struct {
    uint16_t stride[4];
    uint8_t count[4], location[4][128], components[4][128];
} dm_vk_feedback;

static const char *dm_vk_feedback_line(const char *line, size_t n, dm_vk_feedback *layout) {
    uint32_t fields[258]; unsigned count = 0;
    if (n < 3 || memcmp(line, "tf ", 3)) return "invalid feedback record";
    for (size_t at = 3; at < n;) {
        if (count == 258 || line[at] < '0' || line[at] > '9') return "invalid feedback field";
        uint32_t value = 0;
        do {
            if (value > (UINT32_MAX - (uint32_t)(line[at] - '0')) / 10) return "invalid feedback field";
            value = value * 10 + (uint32_t)(line[at++] - '0');
        } while (at < n && line[at] >= '0' && line[at] <= '9');
        fields[count++] = value;
        if (at < n && (line[at++] != ' ' || at == n)) return "invalid feedback separator";
    }
    if (count < 4 || count % 2 || fields[0] >= 4) return "invalid feedback layout";
    unsigned buffer = fields[0], outputs = (count - 2) / 2, stride = 0;
    if (layout->count[buffer]) return "duplicate feedback buffer";
    for (unsigned i = 0; i < outputs; i++) {
        uint32_t loc = fields[2 + i * 2], components = fields[3 + i * 2];
        if (loc >= 32 || !components || components > 4) return "invalid feedback output";
        stride += components * 4;
    }
    if (fields[1] != stride) return "feedback stride differs from outputs";
    layout->stride[buffer] = (uint16_t)stride;
    layout->count[buffer] = (uint8_t)outputs;
    for (unsigned i = 0; i < outputs; i++) {
        layout->location[buffer][i] = (uint8_t)fields[2 + i * 2];
        layout->components[buffer][i] = (uint8_t)fields[3 + i * 2];
    }
    return NULL;
}

static int dm_vk_line_is(const char *line, size_t length, const char *value) {
    return strlen(value) == length && !memcmp(line, value, length);
}
static const char *dm_vk_shader_contract(const char *text, size_t length, const uint32_t *words, size_t count) {
    if (count < 5 || words[0] != 0x07230203 || words[4]) return "invalid SPIR-V header";
    if (words[1] < 0x10000 || words[1] > 0x10300 || (words[1] & 0xff)) return "incompatible SPIR-V version";
    if (memchr(text, 0, length)) return "embedded NUL in shader sidecar";
    uint32_t caps[128]; unsigned nc = 0, ne = 0, entries = 0, functions = 0;
    const char *extensions[128]; size_t extension_lengths[128];
    unsigned char cap_seen[128] = {0}, ext_seen[128] = {0};
    int model = -1;
    for (size_t at = 5; at < count;) {
        uint32_t op = words[at] & 0xffff, n = words[at] >> 16;
        if (!n || n > count - at) return "truncated SPIR-V instruction";
        if (op == 17) {
            if (n != 2 || nc == 128) return "invalid capability list";
            for (unsigned i = 0; i < nc; i++) if (caps[i] == words[at + 1]) return "duplicate module capability";
            caps[nc++] = words[at + 1];
        } else if (op == 10) {
            if (n < 2 || ne == 128) return "invalid extension list";
            const char *name = (const char *)(words + at + 1);
            const char *end = memchr(name, 0, (n - 1) * 4);
            if (!end) return "unterminated SPIR-V extension";
            extensions[ne] = name; extension_lengths[ne++] = (size_t)(end - name);
        } else if (op == 15) {
            if (n < 4 || ++entries != 1) return "expected one shader entry point";
            model = (int)words[at + 1];
        } else if (op == 54) {
            if (functions++) return "nested SPIR-V function";
        } else if (op == 56) {
            if (!functions) return "unmatched SPIR-V function end";
            functions--;
        }
        at += n;
    }
    if (entries != 1 || functions) return "incomplete SPIR-V module";
    int schema = 0, target = 0, stage = 0, captures = 0, geometries = 0, feedbacks = 0;
    dm_vk_feedback feedback = {0};
    dm_vk_geometry geometry = {0};
    for (size_t at = 0; at < length;) {
        const char *line = text + at, *end = memchr(line, '\n', length - at);
        size_t n = end ? (size_t)(end - line) : length - at;
        at += n + (end != NULL);
        if (!n) continue;
        if (n >= 7 && !memcmp(line, "schema ", 7)) {
            if (schema++ || !dm_vk_line_is(line, n, "schema 2")) return "incompatible shader schema";
        } else if (n >= 7 && !memcmp(line, "target ", 7)) {
            if (target++ || !dm_vk_line_is(line, n, "target vulkan1.1")) return "incompatible shader target";
        } else if (n >= 6 && !memcmp(line, "stage ", 6)) {
            const char *names[] = {"vs", "hs", "ds", "gs", "ps", "cs"};
            if (stage++ || model < 0 || model > 5 || !dm_vk_line_is(line + 6, n - 6, names[model])) return "shader stage does not match module";
        } else if (n >= 4 && !memcmp(line, "cap ", 4)) {
            if (n <= 4 || line[4] < '0' || line[4] > '9') return "invalid sidecar capability";
            uint32_t value = 0;
            for (size_t k = 4; k < n; k++) {
                if (line[k] < '0' || line[k] > '9' || value > (UINT32_MAX - (uint32_t)(line[k] - '0')) / 10) return "invalid sidecar capability";
                value = value * 10 + (uint32_t)(line[k] - '0');
            }
            unsigned i = 0; while (i < nc && caps[i] != value) i++;
            if (i == nc || cap_seen[i]++) return "incompatible or duplicate shader capability";
        } else if (n >= 4 && !memcmp(line, "ext ", 4)) {
            unsigned i = 0;
            while (i < ne && (extension_lengths[i] != n - 4 || memcmp(extensions[i], line + 4, n - 4))) i++;
            if (i == ne || ext_seen[i]++) return "incompatible or duplicate shader extension";
        } else if (n >= 3 && !memcmp(line, "tf ", 3)) {
            if (model != 0 && model != 3 && model != 5) return "invalid feedback stage";
            feedbacks++;
            const char *error = dm_vk_feedback_line(line, n, &feedback);
            if (error) return error;
        } else if (n >= 8 && !memcmp(line, "capture ", 8)) {
            if (model != 0 || captures++) return "invalid capture stage or duplicate interface";
            dm_vk_capture capture;
            const char *error = dm_vk_capture_line(line, n, &capture);
            if (error) return error;
        } else if (n >= 9 && !memcmp(line, "geometry ", 9)) {
            if (geometries++) return "duplicate geometry interface";
            const char *error = dm_vk_geometry_line(line, n, model, &geometry);
            if (error) return error;
        } else if (!((n >= 3 && !memcmp(line, "tg ", 3)) || (n >= 3 && !memcmp(line, "c1 ", 3)) ||
                     (n >= 4 && !memcmp(line, "vin ", 4)) || (n >= 2 && !memcmp(line, "b ", 2)))) {
            return "unknown shader sidecar record";
        }
    }
    if (schema != 1 || target != 1 || stage != 1) return "missing shader contract";
    if (geometries && geometry.capture != captures) return "geometry capture role needs its capture interface exclusively";
    if (feedbacks && model == 5 && !geometries) return "compute feedback needs geometry interface";
    for (unsigned i = 0; i < nc; i++) if (!cap_seen[i]) return "missing shader capability";
    for (unsigned i = 0; i < ne; i++) if (!ext_seen[i]) return "missing shader extension";
    return NULL;
}
#endif
