// Emulator fixture. Commands on stdin change one real Android input device;
// closing stdin unplugs it, including when a button is still held.
#include <assert.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void emit(int fd, int type, int code, int value) {
    struct input_event event = {.type = type, .code = code, .value = value};
    assert(write(fd, &event, sizeof event) == sizeof event);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    int id = atoi(argv[1]);
    assert(id >= 1 && id <= 9);
    int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) { perror("/dev/uinput"); return 1; }
    assert(ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0);
    for (int key = BTN_SOUTH; key <= BTN_THUMBR; key++) assert(ioctl(fd, UI_SET_KEYBIT, key) == 0);
    assert(ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0);
    struct uinput_user_dev pad = {.id = {.bustype = BUS_USB, .vendor = 0x1234, .product = id, .version = 1}};
    snprintf(pad.name, sizeof pad.name, "nxnative test controller %d", id);
    int axes[] = {ABS_X, ABS_Y, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y, ABS_GAS, ABS_BRAKE};
    for (size_t i = 0; i < sizeof axes / sizeof *axes; i++) {
        int axis = axes[i];
        assert(ioctl(fd, UI_SET_ABSBIT, axis) == 0);
        pad.absmin[axis] = axis == ABS_GAS || axis == ABS_BRAKE ? 0 : -1;
        pad.absmax[axis] = 1;
    }
    assert(write(fd, &pad, sizeof pad) == sizeof pad && ioctl(fd, UI_DEV_CREATE) == 0);
    puts("connected"); fflush(stdout);
    int type, code, value;
    while (scanf("%d %d %d", &type, &code, &value) == 3) {
        emit(fd, type, code, value);
        emit(fd, EV_SYN, SYN_REPORT, 0);
    }
    assert(ioctl(fd, UI_DEV_DESTROY) == 0);
    close(fd);
}
