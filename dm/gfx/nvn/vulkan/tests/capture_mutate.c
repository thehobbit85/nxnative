// Negative controls for the live-capture/replay fixture. Keep the container
// valid while damaging one semantic input, so the replay must detect it.
#include "../capture_format.h"
#include <string.h>

int main(int argc,char **argv) {
    if (argc!=4) return 2;
    cap_reader reader;cap_writer writer;
    if (cap_load(&reader,argv[1])!=1 || !cap_open(&writer,argv[2])) return 2;
    cap_chunk chunk;uint8_t *data;int next,changed=0;
    while ((next=cap_next(&reader,&chunk,&data))>0) {
        if (!strcmp(argv[3],"pixels") && chunk.tag==CAP_TPIX) {
            cap_tpix *h=(cap_tpix *)data;
            size_t offset=sizeof *h+(size_t)h->nsub*sizeof(cap_tsub);
            if (offset>chunk.raw) return 2;
            memset(data+offset,0,chunk.raw-offset);changed++;
        } else if (!strcmp(argv[3],"interface") && chunk.tag==CAP_PROG && chunk.raw>=sizeof(cap_prog)) {
            ((cap_prog *)data)->iface[0]^=2;changed++;
        } else if (!strcmp(argv[3],"incomplete") && chunk.tag==CAP_VK_RESULT && chunk.raw==4) {
            memset(data,0,4);changed++;
        } else if (!strcmp(argv[3],"segment") && !changed && chunk.tag==CAP_SEGM && chunk.raw>=16) {
            // First record size is invalid, while the segment envelope is valid.
            memset(data+12,0,4);changed++;
        }
        cap_put(&writer,chunk.tag,chunk.submit,data,chunk.raw,1);free(data);
    }
    int ok=!next && changed && cap_close(&writer);cap_unload(&reader);return ok ? 0 : 2;
}
