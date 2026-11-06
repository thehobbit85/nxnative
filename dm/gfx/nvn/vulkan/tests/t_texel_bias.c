// Exercise the shader variant separately so spirv-val can check static and
// dynamic descriptor indexing without requiring that optional GPU feature.
#include "../spv_texel_bias.h"
#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv) {
    assert(argc==3 || argc==4);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    assert(!fseek(f,0,SEEK_END));
    long bytes=ftell(f); assert(bytes>20 && bytes%4==0);
    rewind(f);
    uint32_t *module=malloc(bytes); assert(module);
    assert(fread(module,1,bytes,f)==(size_t)bytes); fclose(f);
    texel_bias edits[]={{0,0,1,0},{0,1,3,0},{1,0,2,1+(2|1<<9)},{1,1,3,1+(1|2<<6)}};
    size_t count=4;
    if (argc==4 && !strcmp(argv[3],"wide")) {
        edits[0]=(texel_bias){.binding=0,.element=0,.texels=1,.scaled=2u<<TEXEL_WORD_SHIFT};
        edits[1]=(texel_bias){.binding=0,.element=1,.texels=3,.scaled=(4u<<TEXEL_WORD_SHIFT)|(1u<<20)};
        count=2;
    } else if (argc==4) {
        assert(!strcmp(argv[3],"scaled"));
        edits[0]=(texel_bias){.binding=0,.element=0,.scaled=8|(8<<5)|(1u<<20)};
        edits[1]=(texel_bias){.binding=0,.element=1,.scaled=10|(10<<5)|(10<<10)|(2u<<15),.swizzle=1+(4|3<<3|2<<6|1<<9)};
        count=2;
    }
    size_t words=0; const char *reason;
    assert(!spv_texel_bias(module,4,edits,count,&words,&reason));
    uint32_t *out=spv_texel_bias(module,(size_t)bytes/4,edits,count,&words,&reason);
    assert(out && !reason && words>(size_t)bytes/4);
    f=fopen(argv[2],"wb"); assert(f);
    assert(fwrite(out,sizeof *out,words,f)==words); assert(!fclose(f));
    free(out); free(module); return 0;
}
