// Assemble the same aligned NVN records for Metal and Vulkan replay fixtures.
#pragma once
typedef struct { _Alignas(8) uint8_t bytes[4096]; size_t length; } list_t;
static void put(list_t *list, unsigned op, const void *payload, size_t bytes) {
    op_hdr header = {op, (uint32_t)((sizeof header + bytes + 7) & ~(size_t)7)};
    assert(list->length + header.size <= sizeof list->bytes);
    memcpy(list->bytes + list->length, &header, sizeof header);
    if (bytes) memcpy(list->bytes + list->length + sizeof header, payload, bytes);
    list->length += header.size;
}
#define RECORD(op, type, ...) put(&list, op, &(type){__VA_ARGS__}, sizeof(type))
