// Emulator fixture. Commands on stdin change one real Android input device;
// closing stdin unplugs it, including when a button is still held.
#include <assert.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void emit(int fd, int type, int code, int value) {
    struct input_event event = {.type = type, .code = code, .value = value};
    assert(write(fd, &event, sizeof event) == sizeof event);
}
// Act as the force-feedback driver. Android uploads the two motor levels then
// starts/stops the effect by ID; record what actually reached this device.
static void *feedback(void *arg) {
    int fd = *(int *)arg;
    struct input_event e;
    while (read(fd, &e, sizeof e) == sizeof e) {
        if (e.type == EV_UINPUT && e.code == UI_FF_UPLOAD) {
            struct uinput_ff_upload upload = {.request_id = e.value};
            if (ioctl(fd, UI_BEGIN_FF_UPLOAD, &upload)) break;
            printf("upload %d %u %u\n", upload.effect.id, upload.effect.u.rumble.strong_magnitude,
                   upload.effect.u.rumble.weak_magnitude);
            fflush(stdout);
            upload.retval = 0;
            if (ioctl(fd, UI_END_FF_UPLOAD, &upload)) break;
        } else if (e.type == EV_UINPUT && e.code == UI_FF_ERASE) {
            struct uinput_ff_erase erase = {.request_id = e.value};
            if (ioctl(fd, UI_BEGIN_FF_ERASE, &erase)) break;
            erase.retval = 0;
            if (ioctl(fd, UI_END_FF_ERASE, &erase)) break;
        } else if (e.type == EV_FF) {
            printf("play %u %d\n", e.code, e.value);
            fflush(stdout);
        }
    }
    return NULL;
}
int main(int argc, char **argv) {
    int rumble = argc == 3 && !strcmp(argv[2], "--rumble");
    assert(argc == 2 || rumble);
    int id = atoi(argv[1]);
    assert(id >= 1 && id <= 9);
    int fd = open("/dev/uinput", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("/dev/uinput"); return 1; }
    assert(ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0);
    for (int key = BTN_SOUTH; key <= BTN_THUMBR; key++) assert(ioctl(fd, UI_SET_KEYBIT, key) == 0);
    assert(ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0);
    struct uinput_user_dev pad = {.id = {.bustype = BUS_USB, .vendor = 0x1234, .product = id, .version = 1}};
    if (rumble) {
        assert(ioctl(fd, UI_SET_EVBIT, EV_FF) == 0 && ioctl(fd, UI_SET_FFBIT, FF_RUMBLE) == 0);
        pad.ff_effects_max = 16;
    }
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
    pthread_t worker;
    if (rumble) assert(!pthread_create(&worker, NULL, feedback, &fd));
    int type, code, value;
    while (scanf("%d %d %d", &type, &code, &value) == 3) {
        emit(fd, type, code, value);
        emit(fd, EV_SYN, SYN_REPORT, 0);
    }
    // Let process exit close the FF device after both threads have left.
    // UI_DEV_DESTROY racing an outstanding FF upload can block on its reply.
    if (rumble) _exit(0);
    assert(ioctl(fd, UI_DEV_DESTROY) == 0);
    close(fd);
}
