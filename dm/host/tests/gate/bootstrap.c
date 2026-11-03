// Exercise the SDK boot functions compiled from the host; platform bring-up
// (scheduler, graphics and input) belongs to the complete executable.
#define main bootstrap_host_main
#include "../../main.c"
#undef main
#include "../../hle.h"
#include <assert.h>

static unsigned checkpoint;
HLE("test_boot_checkpoint", void, boot_checkpoint, uintptr_t x0, uintptr_t x1, unsigned step) {
    assert(step == checkpoint++);
    if (step == 1) assert(x0 == 0xabc && x1 == 0);
    if (step == 4) assert(x0 == 3);
    if (step == 5) {
        char **args = (char **)x0;
        assert(!strcmp(args[0], "game") && !strcmp(args[1], "two words") && !strcmp(args[2], "--test") && !args[3]);
    }
}

static void boot(void) {
    char *args[] = {"dmhost", ".", "main", "sdk", "--", "two words", "--test", NULL};
    const char *names[7 + DM_GAME_NMOD];
    assert(host_arguments(7, args, names) == 2 && !strcmp(names[0], "main") && !strcmp(names[1], "sdk"));
    sdk_init(0xabc, g_argc, g_argv);
    run_nnmain();
}

void test_bootstrap(void) {
#ifdef __ANDROID__
    // Check that start comes from the process record, not the time of this call.
    uint64_t before = now_ns();
    usleep(30000);
    assert(proc_start_mono_ns() <= before);
    // The loader must have sealed guest text and the counted import without write access.
    FILE *maps = fopen("/proc/self/maps", "r");
    assert(maps);
    char line[512], perm[5]; unsigned long lo, hi;
    uintptr_t pc = (uintptr_t)req("nnMain"), stub = 0;
    for (int i = 0; i < g_nmod; i++)
        if (!strcmp(g_mod[i].name, "bootprobe"))
            memcpy(&stub, g_mod[i].base + g_mod[i].data_off, sizeof stub);
    int code_sealed = 0, stub_sealed = 0;
    while (fgets(line, sizeof line, maps)) {
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perm) != 3 || strncmp(perm, "r-x", 3)) continue;
        if (pc >= lo && pc < hi) code_sealed = 1;
        if (stub >= lo && stub < hi) stub_sealed = 1;
    }
    fclose(maps); assert(code_sealed && stub_sealed);
#endif
    front_run(boot);
    assert(checkpoint == 10);
    puts("SDK initialization, main thread arguments, argv, DT_INIT and nnMain through the headless frontend: OK");
}
