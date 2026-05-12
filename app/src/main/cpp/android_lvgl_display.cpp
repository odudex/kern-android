#include "android_lvgl_display.h"

#include "lvgl.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#define LOG_TAG "KernAndroidDisplay"

namespace {

constexpr int kTouchPressed = 0;

std::mutex g_display_mutex;
ANativeWindow *g_window = nullptr;
lv_display_t *g_display = nullptr;
kern_android_layout_t g_layout = KERN_ANDROID_LAYOUT_BOARD;
int32_t g_fallback_w = 720;
int32_t g_fallback_h = 720;
int32_t g_logical_w = 720;
int32_t g_logical_h = 720;
int32_t g_surface_w = 0;
int32_t g_surface_h = 0;
int32_t g_view_x = 0;
int32_t g_view_y = 0;
int32_t g_view_w = 0;
int32_t g_view_h = 0;
float g_scale = 1.0f;
bool g_touch_down = false;
int32_t g_touch_x = 0;
int32_t g_touch_y = 0;
// LVGL renders dirty regions directly into g_framebuffer (DIRECT mode) and
// present_locked() copies it into the current ANativeWindow buffer.
std::vector<uint32_t> g_framebuffer;

uint32_t now_ms() {
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

size_t pixel_count(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return 0;
    return static_cast<size_t>(width) * static_cast<size_t>(height);
}

void allocate_buffers_locked() {
    g_framebuffer.assign(pixel_count(g_logical_w, g_logical_h), 0x00000000);
}

void choose_logical_size_locked() {
    if (g_layout == KERN_ANDROID_LAYOUT_SURFACE && g_surface_w > 0 && g_surface_h > 0) {
        g_logical_w = g_surface_w;
        g_logical_h = g_surface_h;
        return;
    }

    g_logical_w = g_fallback_w > 0 ? g_fallback_w : 720;
    g_logical_h = g_fallback_h > 0 ? g_fallback_h : 720;
}

void recompute_view_locked() {
    if (g_surface_w <= 0 || g_surface_h <= 0 || g_logical_w <= 0 || g_logical_h <= 0) {
        g_view_x = g_view_y = g_view_w = g_view_h = 0;
        g_scale = 1.0f;
        return;
    }

    if (g_layout == KERN_ANDROID_LAYOUT_SURFACE &&
        g_logical_w == g_surface_w && g_logical_h == g_surface_h) {
        g_view_x = 0;
        g_view_y = 0;
        g_view_w = g_surface_w;
        g_view_h = g_surface_h;
        g_scale = 1.0f;
        return;
    }

    const float sx = static_cast<float>(g_surface_w) / static_cast<float>(g_logical_w);
    const float sy = static_cast<float>(g_surface_h) / static_cast<float>(g_logical_h);
    g_scale = std::min(sx, sy);
    g_view_w = std::max(1, static_cast<int32_t>(g_logical_w * g_scale));
    g_view_h = std::max(1, static_cast<int32_t>(g_logical_h * g_scale));
    g_view_x = (g_surface_w - g_view_w) / 2;
    g_view_y = (g_surface_h - g_view_h) / 2;
}

void update_window_size_locked() {
    if (!g_window) return;

    ANativeWindow_setBuffersGeometry(g_window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    g_surface_w = ANativeWindow_getWidth(g_window);
    g_surface_h = ANativeWindow_getHeight(g_window);
}

void apply_size_locked() {
    const int32_t old_w = g_logical_w;
    const int32_t old_h = g_logical_h;

    choose_logical_size_locked();
    if (g_logical_w <= 0 || g_logical_h <= 0) {
        g_logical_w = 720;
        g_logical_h = 720;
    }

    if (old_w != g_logical_w || old_h != g_logical_h || g_framebuffer.empty()) {
        allocate_buffers_locked();
        if (g_display) {
            lv_display_set_resolution(g_display, g_logical_w, g_logical_h);
            lv_display_set_buffers(g_display, g_framebuffer.data(), nullptr,
                                   static_cast<uint32_t>(g_framebuffer.size() * sizeof(uint32_t)),
                                   LV_DISPLAY_RENDER_MODE_DIRECT);
            lv_obj_invalidate(lv_screen_active());
        }
    }

    recompute_view_locked();
}

// LVGL XRGB8888 in memory (LE uint32): [B, G, R, X=ff].
// ANativeWindow RGBA8888 in memory:    [R, G, B, A=ff].
// Swap byte 0 <-> byte 2 and force the alpha byte. Compilers turn this into
// a rev/bswap + one 32-bit store, replacing four byte stores.
inline uint32_t xrgb_to_rgba(uint32_t xrgb) {
    return ((xrgb & 0x00ff0000u) >> 16)
         | (xrgb & 0x0000ff00u)
         | ((xrgb & 0x000000ffu) << 16)
         | 0xff000000u;
}

// Caller must hold g_display_mutex. Blits g_framebuffer into the current
// ANativeWindow and posts it. Reads all shared display state while the
// caller's lock is held; flush_cb invokes this directly.
void present_locked() {
    if (!g_window || g_framebuffer.empty()) return;

    ANativeWindow_Buffer buffer {};
    if (ANativeWindow_lock(g_window, &buffer, nullptr) != 0) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "ANativeWindow_lock failed");
        return;
    }

    auto *base = static_cast<uint8_t *>(buffer.bits);
    const int32_t stride_bytes = buffer.stride * 4;

    for (int32_t y = 0; y < buffer.height; ++y) {
        std::memset(base + y * stride_bytes, 0, static_cast<size_t>(stride_bytes));
    }

    if (g_view_w > 0 && g_view_h > 0) {
        const float inv_scale = 1.0f / g_scale;
        for (int32_t dy = 0; dy < g_view_h; ++dy) {
            const int32_t sy = std::min(g_logical_h - 1, static_cast<int32_t>(dy * inv_scale));
            const uint32_t *src_row = &g_framebuffer[static_cast<size_t>(sy) * g_logical_w];
            auto *row = reinterpret_cast<uint32_t *>(
                base + (g_view_y + dy) * stride_bytes + g_view_x * 4);
            for (int32_t dx = 0; dx < g_view_w; ++dx) {
                const int32_t sx = std::min(g_logical_w - 1, static_cast<int32_t>(dx * inv_scale));
                row[dx] = xrgb_to_rgba(src_row[sx]);
            }
        }
    }

    ANativeWindow_unlockAndPost(g_window);
}

void flush_cb(lv_display_t *display, const lv_area_t * /*area*/, uint8_t * /*px_map*/) {
    // DIRECT mode: LVGL has already rendered dirty regions into g_framebuffer
    // (which is the buffer it owns). We only need to present once per refresh
    // cycle, on the final partial flush.
    std::lock_guard<std::mutex> guard(g_display_mutex);
    if (!g_framebuffer.empty() && lv_display_flush_is_last(display)) {
        present_locked();
    }
    lv_display_flush_ready(display);
}

void touch_read_cb(lv_indev_t *, lv_indev_data_t *data) {
    std::lock_guard<std::mutex> guard(g_display_mutex);
    data->point.x = g_touch_x;
    data->point.y = g_touch_y;
    data->state = g_touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

} // namespace

bool kern_android_display_create(int32_t fallback_width, int32_t fallback_height,
                                 kern_android_layout_t layout,
                                 ANativeWindow *window,
                                 lv_display_t **display_out,
                                 lv_indev_t **indev_out) {
    std::lock_guard<std::mutex> guard(g_display_mutex);
    g_fallback_w = fallback_width;
    g_fallback_h = fallback_height;
    g_layout = layout;
    g_window = window;
    update_window_size_locked();
    choose_logical_size_locked();
    allocate_buffers_locked();

    lv_tick_set_cb(now_ms);
    lv_display_t *display = lv_display_create(g_logical_w, g_logical_h);
    if (!display) return false;

    g_display = display;
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_flush_cb(display, flush_cb);
    lv_display_set_buffers(display, g_framebuffer.data(), nullptr,
                           static_cast<uint32_t>(g_framebuffer.size() * sizeof(uint32_t)),
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    recompute_view_locked();

    if (display_out) *display_out = display;
    if (indev_out) *indev_out = indev;
    return true;
}

void kern_android_display_set_window(ANativeWindow *window) {
    std::lock_guard<std::mutex> guard(g_display_mutex);
    if (g_window && g_window != window) {
        ANativeWindow_release(g_window);
    }
    g_window = window;
    update_window_size_locked();
    apply_size_locked();
}

void kern_android_display_set_touch(int action, float surface_x, float surface_y) {
    std::lock_guard<std::mutex> guard(g_display_mutex);
    g_touch_down = (action == kTouchPressed);

    if (g_view_w <= 0 || g_view_h <= 0 || g_scale <= 0.0f) {
        g_touch_x = std::clamp(static_cast<int32_t>(surface_x), 0, g_logical_w - 1);
        g_touch_y = std::clamp(static_cast<int32_t>(surface_y), 0, g_logical_h - 1);
        return;
    }

    const float bx = (surface_x - static_cast<float>(g_view_x)) / g_scale;
    const float by = (surface_y - static_cast<float>(g_view_y)) / g_scale;
    g_touch_x = std::clamp(static_cast<int32_t>(bx), 0, g_logical_w - 1);
    g_touch_y = std::clamp(static_cast<int32_t>(by), 0, g_logical_h - 1);
}

void kern_android_display_present(void) {
    std::lock_guard<std::mutex> guard(g_display_mutex);
    present_locked();
}

void kern_android_display_destroy(void) {
    std::lock_guard<std::mutex> guard(g_display_mutex);
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
    g_display = nullptr;
    g_framebuffer.clear();
}
