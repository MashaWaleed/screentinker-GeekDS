#include "stp.h"
#include <string.h>

stp_display_t *display_drm_open(const char *card_path);
stp_display_t *display_fbdev_open(const char *dev_path);

stp_display_t *display_open(const char *want) {
    if (want && strcmp(want, "fbdev") == 0) return display_fbdev_open(NULL);
    if (want && strcmp(want, "drm") == 0) return display_drm_open(NULL);
    if (want && strncmp(want, "/dev/dri/", 9) == 0) return display_drm_open(want);
    if (want && strncmp(want, "/dev/fb", 7) == 0) return display_fbdev_open(want);

    stp_display_t *d = display_drm_open(NULL);
    if (d) return d;
    LOG("no usable DRM/KMS output, falling back to fbdev");
    return display_fbdev_open(NULL);
}
