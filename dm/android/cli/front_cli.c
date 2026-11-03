// No display is attached to the Android command-line host. Guest execution uses
// the calling thread, as it does in the windowless Mac frontend.
#include "front.h"

void front_run(void (*guest)(void)) { guest(); }
void *front_metal_layer(void) { return NULL; }
int front_screen_max_fps(void) { return 0; }
int front_screen_variable_refresh(void) { return 0; }
void front_screen_match(int fps) { (void)fps; }

void (*front_progress_hook)(const char *, long, long);
void front_progress(const char *title, long done, long total) {
    (void)title; (void)done; (void)total;
}

int front_text_input(const char *title, const char *sub, const char *guide, const char *initial,
                     int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap;
    return -1;
}
