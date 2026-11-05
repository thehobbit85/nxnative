#ifndef DM_VK_MESHSTAGE_CONTRACT_H
#define DM_VK_MESHSTAGE_CONTRACT_H
// Logical task/mesh stages execute as CS and export the shared stage-memory ABI.
#include <stdint.h>
#include <stddef.h>
typedef struct {
    uint32_t threads, payload, task, vertices, primitives, words, output, primitive;
    uint32_t input_binding, output_binding, control_binding;
    uint32_t vertex_count, primitive_count, vertex_map[132], primitive_map[128];
} dm_vk_meshstage;
static int dm_vk_mesh_number(const char *line, size_t n, size_t *at, uint32_t *value) {
    if (*at == n || line[*at] < '0' || line[*at] > '9') return 0;
    *value = 0;
    do {
        if (*value > (INT32_MAX - (uint32_t)(line[*at] - '0')) / 10) return 0;
        *value = *value * 10 + (uint32_t)(line[(*at)++] - '0');
    } while (*at < n && line[*at] >= '0' && line[*at] <= '9');
    return *at == n || (line[(*at)++] == ' ' && *at < n);
}
static const char *dm_vk_meshstage_line(const char *line, size_t n, dm_vk_meshstage *mesh) {
    uint32_t v[11]; size_t at = 5;
    for (unsigned k = 0; k < 11; k++)
        if (!dm_vk_mesh_number(line, n, &at, &v[k])) return "invalid mesh field";
    if (at != n) return "extra mesh field";
    mesh->threads = v[0]; mesh->payload = v[1]; mesh->task = v[2]; mesh->vertices = v[3]; mesh->primitives = v[4];
    mesh->words = v[5]; mesh->output = v[6]; mesh->primitive = v[7];
    mesh->input_binding = v[8]; mesh->output_binding = v[9]; mesh->control_binding = v[10];
    return NULL;
}
static const char *dm_vk_meshstage_map(const char *line, size_t n, int vertex, dm_vk_meshstage *mesh) {
    size_t at = 6; uint32_t count = 0, *map = vertex ? mesh->vertex_map : mesh->primitive_map;
    if (vertex ? mesh->vertex_count : mesh->primitive_count) return "duplicate mesh output map";
    while (at < n) {
        uint32_t loc, comp;
        if (vertex && n - at >= 3 && line[at] == '-' && line[at + 1] == '1' && line[at + 2] == ' ') { loc = 0; at += 3; }
        else {
            if (!dm_vk_mesh_number(line, n, &at, &loc) || loc >= 32) return "invalid mesh output location";
            loc++;
        }
        if (!dm_vk_mesh_number(line, n, &at, &comp) || comp >= 4 || count == (vertex ? 132u : 128u)) return "invalid mesh output component";
        uint32_t word = loc * 4 + comp;
        if (count && map[count - 1] >= word) return "invalid mesh output order";
        map[count++] = word;
    }
    if (!count) return "empty mesh output map";
    if (vertex) mesh->vertex_count = count; else mesh->primitive_count = count;
    return NULL;
}
static const char *dm_vk_meshstage_validate(const dm_vk_meshstage *m, int task_stage, const uint32_t local[3]) {
    if (!m->threads || m->threads > 1024 || m->payload > 65535 || m->task > 1 || m->words > 8192 ||
        m->input_binding == m->output_binding || m->input_binding == m->control_binding || m->output_binding == m->control_binding ||
        local[0] != m->threads || local[1] != 1 || local[2] != 1) return "invalid mesh limits or bindings";
    uint32_t output = 4 + ((m->payload + 15) & ~15u) / 4, primitive, words;
    if (task_stage) {
        if (m->payload < 8 || m->task || m->vertices || m->primitives || m->vertex_count || m->primitive_count) return "invalid task layout";
        // The task's unused primitive offset follows the byte payload, which
        // need not be word-aligned; the allocation is rounded up separately.
        primitive = (output * 4 + m->payload) / 4; words = output + (m->payload + 3) / 4;
    } else {
        if (!m->vertices || m->vertices > 256 || !m->primitives || m->primitives > 512 ||
            !m->vertex_count || m->vertex_map[0] >= 4 || (m->task && m->payload < 4)) return "invalid mesh output limits";
        uint32_t count = m->vertices > m->primitives ? m->vertices : m->primitives;
        primitive = output + ((count + 31) / 32) * (m->vertex_count + m->primitive_count) * 32;
        words = primitive + (4 + 3 * m->primitives + 3) / 4;
    }
    return m->output == output && m->primitive == primitive && m->words == words ? NULL : "mesh stage memory differs from layout";
}
#endif
