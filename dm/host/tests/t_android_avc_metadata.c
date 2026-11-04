// The returned frame's declared matrix/range override resolution heuristics,
// and queued frames must not overwrite the metadata of the last returned one.
#include "../hle_movie_avc.c"
#include <assert.h>
int main(void) {
    mavc *a = mavc_new();
    assert(!mavc_bt709(a, 480) && mavc_bt709(a, 720));
    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setInt32(format, "width", 2);
    AMediaFormat_setInt32(format, "height", 2);
    AMediaFormat_setInt32(format, "color-format", 21);
    uint8_t picture[] = {16, 16, 16, 16, 128, 128}, out[6];
    for (int i = 0; i < 2; i++) {
        AMediaFormat_setInt32(format, "color-standard", i ? 4 : 1);
        AMediaFormat_setInt32(format, "color-range", i ? 1 : 2);
        assert(copy_picture(a, format, picture, sizeof picture, i));
    }
    assert(mavc_take(a, 0, out, 2, 2, NULL));
    assert(mavc_bt709(a, 480) && !mavc_fullrange(a));
    assert(mavc_take(a, 0, out, 2, 2, NULL));
    assert(!mavc_bt709(a, 720) && mavc_fullrange(a));
    mavc_drop(a);
    assert(mavc_bt709(a, 720) && !mavc_fullrange(a));
    AMediaFormat_delete(format);
    mavc_free(a);
    puts("AVC frame matrix and range metadata passed");
}
