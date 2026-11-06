typedef struct { float low, low_hz, high, high_hz; } guest_rumble;
extern void hid_ids(const uint32_t *, size_t);
extern int vib_handles(uint32_t *, int, const uint32_t *, uint32_t);
extern unsigned vib_mounted(const uint32_t *);
extern uint32_t vib_send(const uint32_t *, const guest_rumble *, int);
extern void vib_actual(guest_rumble *, const uint32_t *);

static void send_haptics(uint64_t button) {
    // B and X address the handheld (soft and sharp), Y addresses player two, A stops the handheld.
    uint32_t id = button & 8 ? 1 : 32, style = id == 32 ? 2 : 1, handles[2];
    int n = vib_handles(handles, 2, &id, style);
    if (n != 2 || !vib_mounted(handles) || !vib_mounted(handles + 1)) {
        NOTE("GUEST_HAPTIC_UNAVAILABLE\n");
        return;
    }
    float hz = button & 4 ? 800 : 50;
    guest_rumble v[2] = {{.25f, hz, 0, 320}, {0, 160, .75f, hz}};
    if (id == 1) v[1].high_hz = 800; // independent wave periods on a two-motor controller
    if (button & (1u << 6)) {
        v[0].low = 0;
        require(vib_send(handles, v, 1) == 0);
        guest_rumble other;
        vib_actual(&other, &handles[1]);
        require(other.high == .75f);
        NOTE("GUEST_HAPTIC_RIGHT_ONLY\n");
        return;
    }
    if (button & 1) v[0].low = v[1].high = 0;
    require(vib_send(handles, v, 2) == 0);
    for (int m = 0; m < 2; m++) {
        guest_rumble actual;
        vib_actual(&actual, &handles[m]);
        require(actual.low == v[m].low && actual.high == v[m].high);
        require(actual.low_hz == v[m].low_hz && actual.high_hz == v[m].high_hz);
    }
    if (button & 1) NOTE("GUEST_HAPTIC_STOP\n");
    else if (id == 1) NOTE("GUEST_HAPTIC_SECOND\n");
    else if (button & 4) NOTE("GUEST_HAPTIC_SHARP\n");
    else NOTE("GUEST_HAPTIC_SOFT\n");
}
