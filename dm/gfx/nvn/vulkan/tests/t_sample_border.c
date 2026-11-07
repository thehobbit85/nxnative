#include "../spv_sample_border.h"
#include <assert.h>
#include <stdio.h>
int main(int argc,char **argv) {
    assert(argc==4);FILE *file=fopen(argv[1],"rb");assert(file);
    assert(!fseek(file,0,SEEK_END));long bytes=ftell(file);assert(bytes>=20 && bytes%4==0);rewind(file);
    uint32_t *input=malloc((size_t)bytes);assert(input);
    assert(fread(input,1,(size_t)bytes,file)==(size_t)bytes);fclose(file);
    sample_border edit={.binding=(uint32_t)strtoul(argv[3],NULL,10),.twin_binding=4,
        .nearest=0,.other=2,.factor={.25f,.5f,.75f,1}};
    size_t count;const char *reason;
    assert(!spv_sample_border(input,4,&edit,1,&count,&reason) && reason);
    uint32_t *output=spv_sample_border(input,(size_t)bytes/4,&edit,1,&count,&reason);
    if (!output) { fprintf(stderr,"%s: %s\n",argv[1],reason);free(input);return 1; }
    file=fopen(argv[2],"wb");assert(file);assert(fwrite(output,4,count,file)==count);assert(!fclose(file));
    free(output);free(input);return 0;
}
