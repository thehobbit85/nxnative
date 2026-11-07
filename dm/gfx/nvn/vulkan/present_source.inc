// Shared by Android window presentation and offscreen screenshot rendering.
// Window bytes already contain the display transfer function. The Apple path
// samples RGBA8/BGRA8 sRGB windows through an ordinary UNORM view for the same reason.
static VkImageView window_source_locked(vtex *v) {
    if (v->window_view) return v->window_view;
    VkFormat format = v->f.f;
    if (format == VK_FORMAT_R8G8B8A8_SRGB) format = VK_FORMAT_R8G8B8A8_UNORM;
    if (format == VK_FORMAT_B8G8R8A8_SRGB) format = VK_FORMAT_B8G8R8A8_UNORM;
    VkImageViewUsageCreateInfo usage = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &usage, 0, v->img, VK_IMAGE_VIEW_TYPE_2D,
                                  format, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (v->f.noalpha) info.components.a = VK_COMPONENT_SWIZZLE_ONE;
    if (vkCreateImageView(g_dev, &info, NULL, &v->window_view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v->window_view;
}

static VkImageView window_source(vtex *texture) {
    static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mutex);VkImageView view=window_source_locked(texture);pthread_mutex_unlock(&mutex);
    return view;
}
