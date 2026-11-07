// Keep the real recorder's program keys and descriptor numbering without adding
// replay-only entry points to the application or duplicating its side tables.
#include "../../hle_nvn.c"
#include "../../nvn_capture.h"

void replay_program(prog_t *program,const cap_prog *captured) {
    prog_keys_t *keys=pk_get(program,1);
    memcpy(keys->key,captured->key,sizeof keys->key);keys->ok=captured->ok;
}
int replay_dpool(dpool_t *pool,const cap_dpol *captured) {
    if (g_ntpb==NDPOOL || g_nspb==NDPOOL) return 0;
    g_tpb[g_ntpb++]=(struct dpool_base){pool,captured->tex_base};
    g_spb[g_nspb++]=(struct dpool_base){pool,captured->samp_base};
    return 1;
}
