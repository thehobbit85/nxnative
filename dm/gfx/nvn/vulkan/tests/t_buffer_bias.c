// Validate generated buffer layouts independently with spirv-val. The replay
// fixture checks the same transformation's memory and atomic behavior on a GPU.
#include "../spv_buffer_bias.h"
#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv) {
    assert(argc==3||argc==4);
    FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);assert(bytes>20&&bytes%4==0);assert(!fseek(f,0,SEEK_SET));
    uint32_t *w=malloc((size_t)bytes);assert(w);assert(fread(w,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
    // Make both aliases use exactly the same source block/pointer type. Only the
    // second gets a bias, so changing the shared original type would be wrong.
    if(argc==3) {
    uint32_t variables[2]={0}, pointer=0;
    for(size_t i=5;i<(size_t)bytes/4;i+=w[i]>>16)
        if((w[i]&0xffff)==71 && w[i]>>16==4 && w[i+2]==33 && w[i+3]<2) variables[w[i+3]]=w[i+1];
    for(size_t i=5;i<(size_t)bytes/4;i+=w[i]>>16)
        if((w[i]&0xffff)==59 && w[i+2]==variables[0]) pointer=w[i+1];
    assert(pointer);
    for(size_t i=5;i<(size_t)bytes/4;i+=w[i]>>16)
        if((w[i]&0xffff)==59 && w[i+2]==variables[1]) w[i+1]=pointer;
    }
    const uint32_t binding=argc==4?(uint32_t)strtoul(argv[3],NULL,10):1,bias=4;size_t n;const char *reason;
    assert(!spv_buffer_bias(w,4,&binding,&bias,1,&n,&reason));
    uint32_t *out=spv_buffer_bias(w,(size_t)bytes/4,&binding,&bias,1,&n,&reason);assert(out&&!reason);
    f=fopen(argv[2],"wb");assert(f);assert(fwrite(out,4,n,f)==n);fclose(f);free(out);free(w);
    puts("Buffer bias preserves shared block types and runtime array layouts");
    return 0;
}
