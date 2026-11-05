#ifndef DM_FEEDBACK_CORPUS_H
#define DM_FEEDBACK_CORPUS_H
// Independent oracle shared by the Vulkan gather and production Metal vs_tf.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { FC_WORDS = 144, FC_RECORDS = 128, FC_SOURCE_PREFIX = 11, FC_ORDER_PREFIX = 7,
       FC_COUNT = 97, FC_PREFIX = 13, FC_OUTPUT_WORDS = FC_PREFIX + FC_COUNT * 512 + 17,
       FC_SOURCE_WORDS = FC_SOURCE_PREFIX + FC_RECORDS * FC_WORDS + 19,
       FC_ORDER_WORDS = FC_ORDER_PREFIX + FC_COUNT + 9, FC_CASES = 20 };
static uint32_t fc_bits(unsigned record, unsigned word) {
    uint32_t values[] = {0x80000000u, 1, 0x7fc01234u, 0xff800000u, 0x3f800000u};
    return word % 7 < 5 ? values[word % 7] : 0x7fc00000u | (record << 8) | word;
}
static void fc_inputs(uint32_t *source, uint32_t *order) {
    memset(source, 0xcc, FC_SOURCE_WORDS * 4); memset(order, 0xcc, FC_ORDER_WORDS * 4);
    for (unsigned v = 0; v < FC_RECORDS; v++)
        for (unsigned w = 0; w < FC_WORDS; w++) source[FC_SOURCE_PREFIX + v * FC_WORDS + w] = fc_bits(v, w);
    for (unsigned i = 0; i < FC_COUNT; i++) order[FC_ORDER_PREFIX + i] = (i * 17 + 3) % 43;
}
static unsigned fc_count(unsigned test) { unsigned counts[] = {0, 1, 3, 65, 97}; return counts[test % 5]; }
static unsigned fc_first(unsigned test) { return test / 5 % 2 * 2; }
static unsigned fc_ordered(unsigned test) { return test / 10; }
static unsigned fc_stride(unsigned layout, unsigned buffer) {
    unsigned strides[2][4] = {{9, 1, 0, 5}, {512, 0, 0, 4}};
    return strides[layout][buffer];
}
static unsigned fc_word(unsigned layout, unsigned buffer, unsigned word) {
    if (layout) return buffer == 0 ? 140 + word % 4 : 16 + word;
    const unsigned maps[4][9] = {{44, 45, 16, 17, 18, 44, 45, 46, 47}, {140}, {0}, {16, 140, 141, 142, 143}};
    return maps[buffer][word];
}
static void fc_layout(unsigned layout, char text[2048]) {
    if (!layout) { strcpy(text, "tf 0 36 7 2 0 3 7 4\ntf 1 4 31 1\ntf 3 20 0 1 31 4\n"); return; }
    strcpy(text, "tf 0 2048");
    for (unsigned i = 0; i < 128; i++) strcat(text, " 31 4");
    strcat(text, "\ntf 3 16 0 4\n");
}
static int fc_check(const uint32_t *actual, unsigned layout, unsigned buffer, unsigned test) {
    unsigned stride = fc_stride(layout, buffer);
    for (unsigned at = 0; at < FC_OUTPUT_WORDS; at++) {
        uint32_t expected = 0xcccccccc;
        if (at >= FC_PREFIX && at - FC_PREFIX < fc_count(test) * stride) {
            unsigned linear = (at - FC_PREFIX) / stride;
            unsigned vertex = (fc_ordered(test) ? (linear * 17 + 3) % 43 : linear) + fc_first(test);
            expected = fc_bits(vertex, fc_word(layout, buffer, (at - FC_PREFIX) % stride));
        }
        if (actual[at] != expected) {
            fprintf(stderr, "layout%u buffer%u case%u word%u: %08x != %08x\n", layout, buffer, test, at, actual[at], expected);
            return 0;
        }
    }
    return 1;
}
#endif
