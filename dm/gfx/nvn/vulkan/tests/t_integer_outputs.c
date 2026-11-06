// A driver can accept a shader with the wrong attachment class and happen to
// produce the right bits. Check the actual interface and validate it separately.
#include "../spv_integer_outputs.h"
#include "../spv_alpha.h"
#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv) {
    assert(argc==6);
    uint8_t uint_mask=(uint8_t)strtoul(argv[3],NULL,0), sint_mask=(uint8_t)strtoul(argv[4],NULL,0);
    int alpha=atoi(argv[5]);
    FILE *f=fopen(argv[1],"rb");
    assert(f && !fseek(f,0,SEEK_END));
    long bytes=ftell(f);
    assert(bytes>=20 && !(bytes%4) && !fseek(f,0,SEEK_SET));
    uint32_t *src=malloc((size_t)bytes);
    assert(src && fread(src,1,(size_t)bytes,f)==(size_t)bytes);
    fclose(f);
    size_t words;
    const char *why;
    uint32_t *typed=spv_integer_outputs(src,(size_t)bytes/4,uint_mask,sint_mask,&words,&why);
    free(src);
    if (!typed) { fprintf(stderr,"%s\n",why); return 1; }
    if (alpha) {
        uint32_t *tested=spv_alpha(typed,words,&words,&why);
        free(typed);
        if (!tested) { fprintf(stderr,"%s\n",why); return 1; }
        typed=tested;
    }
    fragment_outputs outputs=spv_fragment_outputs(typed,words);
    assert(outputs.targets==3 && outputs.sint_targets==sint_mask && outputs.uint_targets==uint_mask);
    assert(!!outputs.alpha_var==!((uint_mask|sint_mask)&1));
    f=fopen(argv[2],"wb");
    assert(f && fwrite(typed,sizeof *typed,words,f)==words && !fclose(f));
    free(typed);
    return 0;
}
