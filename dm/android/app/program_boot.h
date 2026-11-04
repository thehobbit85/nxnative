#ifndef DM_ANDROID_PROGRAM_BOOT_H
#define DM_ANDROID_PROGRAM_BOOT_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Consume hle_oe's index+parameter record once, as the iOS front end does.
// Ordinary menu launches always start program 0. The allowed indices come from
// the compiled host profiles, which the APK target contract binds to its assets.
static int dm_android_program_boot(const char *files, const char *ext, int handoff, int previous,
                                   const int *have, size_t count, char *root, size_t root_n) {
    char rec[1024], param[1024], path[1200];
    if (snprintf(rec, sizeof rec, "%s/program.bin", files) >= (int)sizeof rec ||
        snprintf(param, sizeof param, "%s/launch_param.bin", files) >= (int)sizeof param) return -1;
    unsetenv("DM_LAUNCH_PARAM"); unsetenv("DM_PREV_PROGRAM");
    unlink(param);
    int index = 0, accepted = 0; size_t bytes = 0;
    FILE *f = handoff && count > 1 ? fopen(rec, "rb") : NULL;
    if (f) {
        uint32_t candidate;
        if (fread(&candidate, 4, 1, f) == 1) {
            for (size_t i = 0; i < count; i++) if (candidate == (uint32_t)have[i]) { index = have[i]; accepted = 1; break; }
        }
        if (accepted) {
            unsigned char buf[4096]; FILE *out = NULL; size_t n; int bad = 0;
            while ((n = fread(buf, 1, sizeof buf, f)) != 0) {
                if (!out && !(out = fopen(param, "wb"))) { bad = 1; break; }
                if (fwrite(buf, 1, n, out) != n) { bad = 1; break; }
                bytes += n;
            }
            if (ferror(f)) bad = 1;
            if (out && fclose(out)) bad = 1;
            if (bad) { fclose(f); unlink(param); unlink(rec); fprintf(stderr, "[RED] boot: cannot read program launch parameters\n"); return -1; }
        }
        fclose(f);
    }
    unlink(rec);
    if (handoff && !accepted) fprintf(stderr, "[RED] boot: missing or invalid program record; starting program 0\n");
    if (bytes) setenv("DM_LAUNCH_PARAM", param, 1);
    if (accepted) { snprintf(path, sizeof path, "%d", previous); setenv("DM_PREV_PROGRAM", path, 1); }
    snprintf(path, sizeof path, "%d", index); setenv("DM_PROGRAM", path, 1);
    if (count > 1) setenv("DM_EXEC_OUT", rec, 1); else unsetenv("DM_EXEC_OUT");
    int len = index ? snprintf(root, root_n, "%s/programs/%d", ext, index) : snprintf(root, root_n, "%s", ext);
    if (len < 0 || (size_t)len >= root_n) return -1;
    const char *sub[][2] = {{"DM_ROMFS", "romfs"}, {"DM_VK_DIR", "vk"}, {"DM_FONT_DIR", "font"}, {"DM_AOC_DIR", "aoc"}, {"DM_HTML", "html"}};
    for (size_t i = 0; i < sizeof sub / sizeof *sub; i++) {
        if (snprintf(path, sizeof path, "%s/romfs/romfs.img", root) >= (int)sizeof path) return -1;
        if (i || access(path, R_OK)) if (snprintf(path, sizeof path, "%s/%s", root, sub[i][1]) >= (int)sizeof path) return -1;
        setenv(sub[i][0], path, 0); // explicit diagnostic resource overrides still win
    }
    fprintf(stderr, "boot: program %d, previous %d, %zu launch parameter bytes, pid %ld\n", index, accepted ? previous : -1, bytes, (long)getpid());
    return index;
}
#endif
