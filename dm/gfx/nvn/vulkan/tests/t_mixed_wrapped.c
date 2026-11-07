#include "../spv_mixed_wrapped.h"
#include <assert.h>
#include <stdio.h>

int main(int argc,char **argv) {
    assert(argc==3);
    FILE *file=fopen(argv[1],"rb");assert(file);
    assert(!fseek(file,0,SEEK_END));long bytes=ftell(file);rewind(file);
    assert(bytes>=20 && bytes%4==0);
    uint32_t *input=malloc(bytes);assert(input);
    assert(fread(input,1,bytes,file)==(size_t)bytes);fclose(file);
    mixed_wrapped_edit edit={0,3,4,3};size_t count;const char *reason;
    assert(!spv_mixed_wrapped(input,4,&edit,1,&count,&reason) && reason);
    mixed_wrapped_edit missing={0xffff,3,4,3};
    assert(!spv_mixed_wrapped(input,bytes/4,&missing,1,&count,&reason) && reason);
    uint32_t *mixed=spv_mixed_wrapped(input,bytes/4,&edit,1,&count,&reason);
    free(input);
    if (!mixed) { fprintf(stderr,"%s\n",reason);return 1; }
    uint32_t *output=spv_wrapped_buffer(mixed,count,&edit.buffer_binding,1,&count,&reason);
    free(mixed);
    if (!output) { fprintf(stderr,"%s\n",reason);return 1; }
    file=fopen(argv[2],"wb");assert(file);
    assert(fwrite(output,4,count,file)==count);fclose(file);free(output);return 0;
}
