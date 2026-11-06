// The source crop selects texels. The VI layer controls their on-screen shape.
#include "../present_rect.h"
#include <assert.h>
#include <stdio.h>

static void check(uint64_t screen, uint64_t layer, int sw, int sh, int dw, int dh,
                  int stretch, int x, int y, int w, int h) {
    vk_present_rect r = vk_present_fit(screen, layer, sw, sh, dw, dh, stretch);
    assert(r.x == x && r.y == y && r.w == w && r.h == h);
}

int main(void) {
    uint64_t tall = vk_present_shape(1920, 1344), square = vk_present_shape(1, 1);
    check(0, 0, 1200, 960, 1280, 800, 0, 0, 40, 1280, 720);
    check(tall, square, 1920, 1080, 1280, 800, 0, 240, 0, 800, 800);
    check(tall, square, 1920, 1344, 1280, 800, 0, 68, 0, 1143, 800);
    check(0, square, 1200, 960, 1280, 800, 1, 0, 0, 1280, 800);
    check(0, vk_present_shape(1952, 900), 1952, 900, 2868, 1320, 0, 0, 0, 2868, 1320);
    check(0, vk_present_shape(1920, 0), 1920, 1080, 720, 1280, 0, 0, 437, 720, 405);
    puts("Vulkan presentation shape and viewport passed");
}
