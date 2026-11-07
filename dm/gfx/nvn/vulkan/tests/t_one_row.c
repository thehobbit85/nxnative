#include "../spv_one_row.h"
#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv) {
    assert(argc == 3);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long bytes = ftell(file); assert(bytes >= 20 && bytes % 4 == 0);
    rewind(file);
    uint32_t *input = malloc(bytes); assert(input);
    assert(fread(input, 1, bytes, file) == (size_t)bytes); fclose(file);
    size_t count = 0; const char *reason;
    assert(!spv_one_row(input, 4, &count, &reason) && reason);
    uint32_t *output = spv_one_row(input, bytes / 4, &count, &reason);
    if (reason) { fprintf(stderr, "%s\n", reason); return 1; }
    if (output) {
        size_t again;
        assert(!spv_one_row(output, count, &again, &reason) && !reason);
    } else { output = input; count = bytes / 4; }
    file = fopen(argv[2], "wb"); assert(file);
    assert(fwrite(output, sizeof *output, count, file) == count); assert(!fclose(file));
    if (output != input) free(output);
    free(input); return 0;
}
