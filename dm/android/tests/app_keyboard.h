// Synthetic guest calls the public SDK keyboard import and checks its result buffer.
extern uint32_t keyboard_show(void *out, const void *arg);
static void check_keyboard(void) {
    uint16_t initial[] = {'A', 0xd83d, 0xde80, 'e', 0x301, 'Z', 'Q', 0};
    const unsigned char utf8[] = {'A', 0xf0, 0x9f, 0x9a, 0x80, 'e', 0xcc, 0x81, 'Z', 0};
    static const char titles[][12] = {"Unicode", "UTF16", "Minimum", "Cancel", "Background"};
    for (int test = 0; test < 5; test++) {
        _Alignas(8) uint8_t arg[0x500] = {0}, buffer[64];
        struct { void *data; uint64_t size; } out = {buffer, sizeof buffer};
        memset(buffer, 0x7b, sizeof buffer);
        for (int i = 0; titles[test][i]; i++) arg[0x24 + 2*i] = titles[test][i];
        arg[0x3ac] = 6; arg[0x3b0] = test == 2 ? 2 : 0;
        arg[0x3bd] = test != 1;
        if (test <= 1) {
            const void *work = initial;
            memcpy(arg + 0x4d0, &work, sizeof work);
            arg[0x3c4] = 7;
        }
        uint32_t result = keyboard_show(&out, arg);
        if (test >= 3) { require(result == 0x29f && buffer[0] == 0x7b); continue; }
        require(result == 0);
        if (test == 0) for (unsigned i = 0; i < sizeof utf8; i++) require(buffer[i] == utf8[i]);
        if (test == 1) {
            for (int i = 0; i < 6; i++) { uint16_t u; memcpy(&u, buffer + 2*i, 2); require(u == initial[i]); }
            require(!buffer[12] && !buffer[13]);
        }
        if (test == 2) require(buffer[0] == 'a' && buffer[1] == 'b' && !buffer[2]);
    }
    NOTE("APP_KEYBOARD_PASSED\n");
}
