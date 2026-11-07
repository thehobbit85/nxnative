// Inspect a trusted local Vulkan capture without creating a Vulkan device.
#include "../capture_format.h"
#include <string.h>
#include <inttypes.h>

int main(int argc,char **argv) {
    if (argc!=2) { fprintf(stderr,"usage: capture-inspect <capture>\n");return 2; }
    cap_reader reader;
    if (!cap_load(&reader,argv[1])) { fprintf(stderr,"[RED] cannot read capture\n");return 1; }
    cap_chunk chunk;uint8_t *data;int next,backend=0,layout=0,complete=0;
    uint64_t submits=0,segments=0,regions=0,images=0,bytes=0;
    while ((next=cap_next(&reader,&chunk,&data))>0) {
        switch (chunk.tag) {
            case CAP_HEAD:
                fwrite(data,1,chunk.raw,stdout);
                backend=memmem(data,chunk.raw,"backend\tvulkan\n",15)!=NULL && memmem(data,chunk.raw,"iface\t3\n",8)!=NULL;
                break;
            case CAP_LAYT: layout=cap_layout_matches((char *)data,chunk.raw);break;
            case CAP_SUBM: submits++;break;
            case CAP_SEGM: segments++;break;
            case CAP_REGN: regions++;break;
            case CAP_TPIX: images++;break;
            case CAP_VK_RESULT:
                if (chunk.raw==4) { uint32_t value;memcpy(&value,data,4);complete=value==1; }
                break;
        }
        bytes+=chunk.raw;free(data);
    }
    int ok=!next && reader.pos==reader.n && backend && layout && complete;
    printf("%s capture, %" PRIu64 " submits, %" PRIu64 " segments, %" PRIu64 " regions, %" PRIu64 " images, %" PRIu64 " raw bytes\n",
        ok ? "Complete" : "[RED] Incomplete or incompatible",submits,segments,regions,images,bytes);
    cap_unload(&reader);
    return ok ? 0 : 1;
}
