// Small synthetic Vorbis identification/setup headers and audio packets for host tests.
typedef struct { unsigned char data[1024]; int bits; } vpkt;
static void vbits(vpkt *p, unsigned value, int n) { for (int i = 0; i < n; i++, p->bits++) p->data[p->bits / 8] |= ((value >> i) & 1) << (p->bits % 8); }
static void vhead(vpkt *p, int type) { vbits(p, type, 8); for (int i = 0; i < 6; i++) vbits(p, "vorbis"[i], 8); }
static void vcodebook(vpkt *p, int lookup) {
    vbits(p, 0x564342, 24); vbits(p, 1, 16); vbits(p, 1, 24);     // one dimension, one entry
    vbits(p, 0, 1); vbits(p, 0, 1); vbits(p, 0, 5);                // unordered, dense, one-bit code
    vbits(p, lookup, 4);
    if (lookup) { vbits(p, (788u << 21) | 1, 32); vbits(p, 0, 32); vbits(p, 0, 4); vbits(p, 0, 1); vbits(p, 0, 1); }   // minimum 1.0, delta 0.0, one-bit multiplicand, no sequence
}
// One channel, blocksizes as in the identification header (0x66: both 64; 0x86: 64 and 256), one mode per window size in use; packets as vaudio makes them
static void vheaders(vpkt *id, vpkt *setup, int blocksizes, int modes) {
    vhead(id, 1); vbits(id, 0, 32); vbits(id, 1, 8); vbits(id, 48000, 32); for (int i = 0; i < 3; i++) vbits(id, 0, 32); vbits(id, blocksizes, 8); vbits(id, 1, 8);
    vhead(setup, 5); vbits(setup, 1, 8); vcodebook(setup, 0); vcodebook(setup, 1);
    vbits(setup, 0, 6); vbits(setup, 0, 16);                                          // one time configuration
    vbits(setup, 0, 6); vbits(setup, 1, 16); vbits(setup, 0, 5); vbits(setup, 0, 2); vbits(setup, 5, 4); vbits(setup, 0, 6);   // one floor 1, no partitions
    vbits(setup, 0, 16); vbits(setup, 0, 24); vbits(setup, 32, 24); vbits(setup, 31, 24); vbits(setup, 0, 6); vbits(setup, 0, 8);   // residue 0: partition size 32, classbook 0
    vbits(setup, 1, 3); vbits(setup, 0, 1); vbits(setup, 1, 8);                       // pass 0 uses vector codebook 1
    vbits(setup, 0, 6); vbits(setup, 0, 16); vbits(setup, 0, 1); vbits(setup, 0, 1); vbits(setup, 0, 2);   // one mapping, no coupling, reserved bits
    vbits(setup, 0, 8); vbits(setup, 0, 8); vbits(setup, 0, 8);
    vbits(setup, modes - 1, 6); for (int m = 0; m < modes; m++) { vbits(setup, m, 1); vbits(setup, 0, 16); vbits(setup, 0, 16); vbits(setup, 0, 8); }   // mode m: short (0) or long (1) window
    vbits(setup, 1, 1);
}
static void vaudio(vpkt *p, int modes, int blockflag, int prev_long, int next_long) {   // the window flags are only in a long window's packet
    vbits(p, 0, 1);
    if (modes > 1) { vbits(p, blockflag, 1); if (blockflag) { vbits(p, prev_long, 1); vbits(p, next_long, 1); } }
    vbits(p, 1, 1); vbits(p, 255, 8); vbits(p, 255, 8); vbits(p, 0, 1); vbits(p, 0, 32);
}
