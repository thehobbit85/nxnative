// The Android boot selection uses the same index+bytes record as iOS.
#include "../../android/app/program_boot.h"
#include <assert.h>
#include <sys/stat.h>
static void record(const char *files, int index, const void *data, size_t n) {
    char p[1024]; snprintf(p, sizeof p, "%s/program.bin", files);
    FILE *f = fopen(p, "wb"); assert(f);
    uint32_t ix = index; assert(fwrite(&ix, 4, 1, f) == 1);
    if (n) assert(fwrite(data, n, 1, f) == 1); assert(!fclose(f));
}
static void paths(const char *root) {
    char p[1024]; const char *keys[] = {"DM_ROMFS", "DM_VK_DIR", "DM_FONT_DIR", "DM_AOC_DIR", "DM_HTML"};
    const char *dirs[] = {"romfs", "vk", "font", "aoc", "html"};
    for (int i = 0; i < 5; i++) { snprintf(p, sizeof p, "%s/%s", root, dirs[i]); assert(getenv(keys[i]) && !strcmp(getenv(keys[i]), p)); unsetenv(keys[i]); }
}
int main(void) {
    char tmp[64] = "/tmp/nx-program-XXXXXX";
#ifdef __ANDROID__
    strcpy(tmp, "/data/local/tmp/nxp-XXXXXX");
#endif
    assert(mkdtemp(tmp));
    char files[1024], ext[1024], root[1024], p[1024];
    snprintf(files, sizeof files, "%s/files", tmp); snprintf(ext, sizeof ext, "%s/game", tmp);
    assert(!mkdir(files, 0700) && !mkdir(ext, 0700));
    const int have[] = {0,1,2}; const uint8_t data[] = {0,0xff,4,0,6,9};
    setenv("DM_SAVE", "shared-save-sentinel", 1);
    record(files, 2, data, sizeof data); // an ordinary launch discards a stale handoff
    assert(dm_android_program_boot(files, ext, 0, 7, have, 3, root, sizeof root) == 0);
    assert(!strcmp(root, ext) && !strcmp(getenv("DM_PROGRAM"), "0") && !getenv("DM_PREV_PROGRAM") && !getenv("DM_LAUNCH_PARAM")); paths(root);
    for (int i = 0; i < 16; i++) {
        int ix = i % 3; record(files, ix, data, sizeof data);
        assert(dm_android_program_boot(files, ext, 1, 2, have, 3, root, sizeof root) == ix);
        snprintf(p, sizeof p, "%s/programs/%d", ext, ix); assert(!strcmp(root, ix ? p : ext)); paths(root);
        assert(atoi(getenv("DM_PROGRAM")) == ix && !strcmp(getenv("DM_PREV_PROGRAM"), "2"));
        uint8_t got[16] = {0}; FILE *f = fopen(getenv("DM_LAUNCH_PARAM"), "rb"); assert(f);
        assert(fread(got, 1, sizeof got, f) == sizeof data && !memcmp(got, data, sizeof data)); fclose(f);
        snprintf(p, sizeof p, "%s/program.bin", files); assert(access(p, F_OK) == -1);
        assert(!strcmp(getenv("DM_SAVE"), "shared-save-sentinel"));
    }
    record(files, 1, NULL, 0);
    assert(dm_android_program_boot(files, ext, 1, 0, have, 3, root, sizeof root) == 1 && !getenv("DM_LAUNCH_PARAM")); paths(root);
    record(files, 99, data, sizeof data);
    assert(dm_android_program_boot(files, ext, 1, 0, have, 3, root, sizeof root) == 0 && !getenv("DM_PREV_PROGRAM") && !getenv("DM_LAUNCH_PARAM")); paths(root);
    snprintf(p, sizeof p, "%s/program.bin", files); FILE *f = fopen(p, "wb"); assert(f); fputc(1, f); fclose(f);
    assert(dm_android_program_boot(files, ext, 1, 1, have, 3, root, sizeof root) == 0 && !getenv("DM_LAUNCH_PARAM")); paths(root);
    assert(dm_android_program_boot(files, ext, 1, 1, have, 3, root, sizeof root) == 0); paths(root);
    setenv("DM_ROMFS", "diagnostic-override", 1);
    assert(dm_android_program_boot(files, ext, 0, -1, have, 1, root, sizeof root) == 0 && !getenv("DM_EXEC_OUT"));
    assert(!strcmp(getenv("DM_ROMFS"), "diagnostic-override")); unsetenv("DM_ROMFS");
    snprintf(p, sizeof p, "%s/launch_param.bin", files); unlink(p);
    assert(!rmdir(files) && !rmdir(ext) && !rmdir(tmp));
    puts("PASS: Android program roots, binary parameters, previous index, one-shot records and fallback");
    return 0;
}
