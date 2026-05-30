#ifndef KERN_ANDROID_LVGL_DISPLAY_H
#define KERN_ANDROID_LVGL_DISPLAY_H

#include <android/native_window.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _lv_display_t lv_display_t;
typedef struct _lv_indev_t lv_indev_t;

typedef enum {
    KERN_ANDROID_LAYOUT_BOARD = 0,
    KERN_ANDROID_LAYOUT_SURFACE = 1,
} kern_android_layout_t;

bool kern_android_display_create(int32_t fallback_width, int32_t fallback_height,
                                 kern_android_layout_t layout,
                                 ANativeWindow *window,
                                 lv_display_t **display_out,
                                 lv_indev_t **indev_out);
void kern_android_display_set_window(ANativeWindow *window);
void kern_android_display_set_touch(int action, float surface_x, float surface_y);
void kern_android_display_destroy(void);
void kern_android_display_present(void);
// Presents the framebuffer if a refresh produced new content since the last
// present. Called from the render loop AFTER releasing the LVGL lock, so the
// blocking ANativeWindow blit/post does not hold off the camera frame
// callback's non-blocking display lock.
void kern_android_display_flush_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* KERN_ANDROID_LVGL_DISPLAY_H */
