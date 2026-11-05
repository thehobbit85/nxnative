// Strong frontend boundary for the shared display-service fixture. Keeping it
// separate exercises the same weak-symbol override as each real frontend.
int display_width, display_height;
int front_display_pixels(int *w, int *h) {
    if (display_width <= 0 || display_height <= 0) return 0;
    *w = display_width; *h = display_height; return 1;
}
