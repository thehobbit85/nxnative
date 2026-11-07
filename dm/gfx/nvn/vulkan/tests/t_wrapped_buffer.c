#include "../spv_wrapped_buffer.h"
#include <assert.h>
#include <stdio.h>

int main(int argc,char **argv) {
    assert(argc==3);
    FILE *file=fopen(argv[1],"rb");assert(file);
    assert(!fseek(file,0,SEEK_END));long bytes=ftell(file);rewind(file);
    assert(bytes>=20 && bytes%4==0);
    uint32_t *input=malloc(bytes);assert(input);
    assert(fread(input,1,bytes,file)==(size_t)bytes);fclose(file);
    size_t count;const char *reason;uint32_t binding=0;
    assert(!spv_wrapped_buffer(input,4,&binding,1,&count,&reason) && reason);
    uint32_t missing=0xffff;
    assert(!spv_wrapped_buffer(input,bytes/4,&missing,1,&count,&reason) && reason);
    uint32_t *output=spv_wrapped_buffer(input,bytes/4,&binding,1,&count,&reason);
    if (!output) { fprintf(stderr,"%s\n",reason);free(input);return 1; }
    file=fopen(argv[2],"wb");assert(file);
    assert(fwrite(output,4,count,file)==count);fclose(file);
    free(input);free(output);return 0;
}
