typedef struct {
    int64_t delta_ns, sampling;
    float acc[3], gyro[3], angle[3], direction[9];
    uint32_t attributes, pad;
} guest_motion;
extern int six_handles(uint32_t *, int, const uint32_t *, uint32_t);
extern void six_current(guest_motion *, const uint32_t *);

static void report_motion(void) {
    uint32_t handle, handheld = 32;
    require(six_handles(&handle, 1, &handheld, 2) == 1);
    guest_motion state;
    six_current(&state, &handle);
    require(state.attributes & 1);
    char line[] = "GUEST_MOTION 00000000 00000000 00000000 00000000 00000000 00000000\n";
    for (unsigned i = 0; i < 6; i++) {
        float v = i < 3 ? state.acc[i] : state.gyro[i-3];
        uint32_t word = (uint32_t)(int32_t)(v * 1000);
        for (unsigned b = 0; b < 8; b++) line[20 + 9*i - b] = "0123456789abcdef"[(word >> (4*b)) & 15];
    }
    message(line, sizeof line - 1);
}
