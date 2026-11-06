// Exercise the shader variant separately so spirv-val can check static and
// dynamic descriptor indexing without requiring that optional GPU feature.
#include "../spv_texel_bias.h"
#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv) {
    assert(argc==3);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    assert(!fseek(f,0,SEEK_END));
    long bytes=ftell(f); assert(bytes>20 && bytes%4==0);
    rewind(f);
    uint32_t *module=malloc(bytes); assert(module);
    assert(fread(module,1,bytes,f)==(size_t)bytes); fclose(f);
    texel_bias edits[]={{0,0,1},{0,1,3},{1,0,2}};
    size_t words=0; const char *reason;
    assert(!spv_texel_bias(module,4,edits,3,&words,&reason));
    uint32_t *out=spv_texel_bias(module,(size_t)bytes/4,edits,3,&words,&reason);
    assert(out && !reason && words>(size_t)bytes/4);
    f=fopen(argv[2],"wb"); assert(f);
    assert(fwrite(out,sizeof *out,words,f)==words); assert(!fclose(f));
    free(out); free(module); return 0;
}
