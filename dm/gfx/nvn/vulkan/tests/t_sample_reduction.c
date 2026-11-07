#include "../spv_sample_reduction.h"
#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv) {
    assert(argc == 4 || argc == 5);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long bytes = ftell(file); assert(bytes >= 20 && bytes % 4 == 0);
    rewind(file);
    uint32_t *input = malloc(bytes); assert(input);
    assert(fread(input, 1, bytes, file) == (size_t)bytes); fclose(file);
    if (argc == 5) {
        assert(!strcmp(argv[4], "no-import"));
        for (size_t i=5; i<(size_t)bytes/4;) {
            unsigned len=input[i]>>16, op=input[i]&0xffff;
            assert(op != 12); // The fixture has no existing extended instructions.
            if (op == 11) {
                memmove(input+i, input+i+len, bytes-(i+len)*4);
                bytes -= len*4;
            } else i += len;
        }
    }
    sample_reduction filter = {.binding=(uint32_t)strtoul(argv[3],NULL,10), .minimum=1, .min_linear=1, .mag_linear=1, .max_lod=10, .max_bias=16};
    size_t count = 0; const char *reason;
    assert(!spv_sample_reduction(input, 4, &filter, 1, &count, &reason) && reason);
    uint32_t *output = spv_sample_reduction(input, bytes / 4, &filter, 1, &count, &reason);
    if (!output) { fprintf(stderr, "%s\n", reason); free(input); return 1; }
    file = fopen(argv[2], "wb"); assert(file);
    assert(fwrite(output, sizeof *output, count, file) == count); assert(!fclose(file));
    free(output); free(input); return 0;
}
