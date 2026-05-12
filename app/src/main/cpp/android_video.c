/*
 * Android Camera2-backed implementation of the video.h ABI.
 *
 * Replaces simulator/platform/video_sim/video_sim.c in the Android build.
 * Lifecycle calls from Kern (app_video_open / stream_start / stream_stop /
 * close) upcall directly into Kotlin's CameraManager via JNI, which drives
 * a Camera2 session. The session pushes YUV_420_888 frames back into
 * Java_com_odudex_kern_CameraManager_deliverCameraFrame, which converts
 * to RGB565 and invokes the Kern-registered frame callback.
 */

#include "video/video.h"
#include "esp_err.h"
#include "esp_log.h"

#include <android/log.h>
#include <inttypes.h>
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

/* sim's esp_log.h maps ESP_LOG* to printf() (stdout, discarded on Android).
 * Route this file's logs to logcat so errors surface in `adb logcat`. */
#undef ESP_LOGI
#undef ESP_LOGW
#undef ESP_LOGE
#define ESP_LOGI(tag, fmt, ...) __android_log_print(ANDROID_LOG_INFO,  (tag), fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) __android_log_print(ANDROID_LOG_WARN,  (tag), fmt, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) __android_log_print(ANDROID_LOG_ERROR, (tag), fmt, ##__VA_ARGS__)

static const char *TAG = "android_video";

#define MAX_USER_BUFS 4
#define FAKE_VIDEO_FD 42

static app_video_frame_operation_cb_t s_frame_cb = NULL;
static uint8_t *s_frame_buf = NULL;
static uint32_t s_width = 0;
static uint32_t s_height = 0;
static size_t s_frame_size = 0;

static uint8_t *s_user_bufs[MAX_USER_BUFS];
static uint32_t s_num_user_bufs = 0;

static atomic_bool s_streaming = ATOMIC_VAR_INIT(false);

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --- JNI plumbing for upcalls into Kotlin CameraManager --- */

static JavaVM     *s_vm = NULL;
static jclass      s_camera_manager_class = NULL;
static jmethodID   s_mid_open  = NULL;
static jmethodID   s_mid_start = NULL;
static jmethodID   s_mid_stop  = NULL;
static jmethodID   s_mid_close = NULL;

/* Attach the calling thread to the JVM (as a daemon) if it isn't already
 * attached. Returns true if we attached and the caller must detach. The
 * FreeRTOS-emulated pthread that runs scanner.c / capture_entropy.c is not
 * a JVM thread, so attaching is mandatory before calling into Java. */
static bool ensure_attached(JNIEnv **out_env) {
    if (!s_vm) { *out_env = NULL; return false; }
    jint res = (*s_vm)->GetEnv(s_vm, (void **)out_env, JNI_VERSION_1_6);
    if (res == JNI_OK) return false;
    if (res == JNI_EDETACHED &&
        (*s_vm)->AttachCurrentThreadAsDaemon(s_vm, out_env, NULL) == JNI_OK) {
        return true;
    }
    *out_env = NULL;
    return false;
}

static void call_camera_void(jmethodID mid, const char *name) {
    if (!mid || !s_camera_manager_class) {
        ESP_LOGW(TAG, "%s: JNI not initialized", name);
        return;
    }
    JNIEnv *env = NULL;
    bool attached = ensure_attached(&env);
    if (!env) {
        ESP_LOGE(TAG, "%s: cannot attach thread", name);
        return;
    }
    (*env)->CallStaticVoidMethod(env, s_camera_manager_class, mid);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    if (attached) (*s_vm)->DetachCurrentThread(s_vm);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    s_vm = vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass local = (*env)->FindClass(env, "com/odudex/kern/CameraManager");
    if (!local) return JNI_ERR;
    s_camera_manager_class = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (!s_camera_manager_class) return JNI_ERR;
    s_mid_open  = (*env)->GetStaticMethodID(env, s_camera_manager_class, "openCamera",     "()V");
    s_mid_start = (*env)->GetStaticMethodID(env, s_camera_manager_class, "startStreaming", "()V");
    s_mid_stop  = (*env)->GetStaticMethodID(env, s_camera_manager_class, "stopStreaming",  "()V");
    s_mid_close = (*env)->GetStaticMethodID(env, s_camera_manager_class, "closeCamera",    "()V");
    if (!s_mid_open || !s_mid_start || !s_mid_stop || !s_mid_close) {
        (*env)->ExceptionClear(env);
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}

/* --- JNI entries called by Kotlin CameraManager --- */

JNIEXPORT void JNICALL
Java_com_odudex_kern_CameraManager_setNegotiatedResolution(JNIEnv *env, jclass clazz,
                                                            jint w, jint h) {
    (void)env; (void)clazz;
    pthread_mutex_lock(&s_state_mutex);
    s_width = (w > 0) ? (uint32_t)w : 0;
    s_height = (h > 0) ? (uint32_t)h : 0;
    pthread_mutex_unlock(&s_state_mutex);
}

static inline uint8_t clip255(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static void reset_user_bufs_locked(void) {
    s_num_user_bufs = 0;
    memset(s_user_bufs, 0, sizeof(s_user_bufs));
}

static void reset_frame_state_locked(bool clear_callback) {
    if (clear_callback) s_frame_cb = NULL;
    free(s_frame_buf);
    s_frame_buf = NULL;
    s_frame_size = 0;
    s_width = 0;
    s_height = 0;
    reset_user_bufs_locked();
}

JNIEXPORT void JNICALL
Java_com_odudex_kern_CameraManager_deliverCameraFrame(JNIEnv *env, jclass clazz,
                                                       jobject y_buf, jint y_row_stride,
                                                       jobject u_buf, jint u_row_stride, jint u_pixel_stride,
                                                       jobject v_buf, jint v_row_stride, jint v_pixel_stride,
                                                       jint width, jint height) {
    (void)clazz;
    const uint8_t *y_plane = (const uint8_t *)(*env)->GetDirectBufferAddress(env, y_buf);
    const uint8_t *u_plane = (const uint8_t *)(*env)->GetDirectBufferAddress(env, u_buf);
    const uint8_t *v_plane = (const uint8_t *)(*env)->GetDirectBufferAddress(env, v_buf);
    if (!y_plane || !u_plane || !v_plane) {
        ESP_LOGW(TAG, "deliverCameraFrame: non-direct ByteBuffer");
        return;
    }
    if (!atomic_load_explicit(&s_streaming, memory_order_acquire)) return;
    if (width <= 0 || height <= 0) return;

    pthread_mutex_lock(&s_state_mutex);
    app_video_frame_operation_cb_t cb = s_frame_cb;
    uint8_t *dst_base = s_frame_buf;
    size_t dst_size = s_frame_size;
    uint32_t buf_w = s_width;
    uint32_t buf_h = s_height;
    uint8_t *user_buf = (s_num_user_bufs > 0) ? s_user_bufs[0] : NULL;

    /* The sensor delivers landscape frames (W×H, e.g. 1280×960). Kern's UI
     * is portrait, so we rotate 90° CW during conversion into an H×W buffer
     * (e.g. 960×1280). Assumes SENSOR_ORIENTATION == 90 (the typical back-
     * camera mount on phones); read that characteristic from Camera2 and
     * branch on it here if we ever need to support other mounts. */
    if (!dst_base || (uint32_t)width != buf_h || (uint32_t)height != buf_w ||
        dst_size < (size_t)width * (size_t)height * 2) {
        pthread_mutex_unlock(&s_state_mutex);
        return;
    }

    /* BT.601 limited-range YUV → RGB565, integer fixed-point. U/V are
     * subsampled 2:1 so each chroma sample is reused for a 2×2 block of
     * luma. pixel_stride is read from the plane (NV12-style interleaving
     * gives uPixelStride=2, planar gives 1 — never assume tight packing).
     * 90° CW rotation: source (x, y) → dst (H-1-y, x), so dst index is
     * x*H + (H-1-y). Iterating source-natural keeps Y/U/V reads sequential;
     * dst writes step `H` apart but successive source rows fill adjacent
     * dst columns, so the working set stays cache-friendly. */
    uint16_t *dst = (uint16_t *)dst_base;
    for (int32_t y = 0; y < height; y++) {
        const uint8_t *y_row = y_plane + (size_t)y * y_row_stride;
        const uint8_t *u_row = u_plane + (size_t)(y >> 1) * u_row_stride;
        const uint8_t *v_row = v_plane + (size_t)(y >> 1) * v_row_stride;
        size_t dst_col = (size_t)(height - 1 - y);
        for (int32_t x = 0; x < width; x++) {
            int Y = (int)y_row[x] - 16;
            int U = (int)u_row[(x >> 1) * u_pixel_stride] - 128;
            int V = (int)v_row[(x >> 1) * v_pixel_stride] - 128;
            int r = (298 * Y + 409 * V + 128) >> 8;
            int g = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            int b = (298 * Y + 516 * U + 128) >> 8;
            uint8_t R = clip255(r);
            uint8_t G = clip255(g);
            uint8_t B = clip255(b);
            dst[(size_t)x * (size_t)height + dst_col] =
                (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
        }
    }

    uint8_t *deliver = dst_base;
    if (user_buf) {
        memcpy(user_buf, dst_base, dst_size);
        deliver = user_buf;
    }

    if (cb) cb(deliver, 0, buf_w, buf_h, dst_size);
    pthread_mutex_unlock(&s_state_mutex);
}

/* --- video.h ABI --- */

esp_err_t app_video_main(i2c_master_bus_handle_t i2c_bus_handle) {
    (void)i2c_bus_handle;
    return ESP_OK;
}

int app_video_open(char *dev, video_fmt_t init_fmt) {
    (void)dev;
    (void)init_fmt;

    pthread_mutex_lock(&s_state_mutex);
    reset_frame_state_locked(false);
    pthread_mutex_unlock(&s_state_mutex);

    /* Blocking upcall — Kotlin runs Camera2 open + session configure and
     * calls setNegotiatedResolution() before returning. Width=0 signals
     * failure (permission denied, no camera, ...). */
    call_camera_void(s_mid_open, "camera_open");

    pthread_mutex_lock(&s_state_mutex);
    uint32_t w = s_width;
    uint32_t h = s_height;
    pthread_mutex_unlock(&s_state_mutex);

    if (w == 0 || h == 0) {
        ESP_LOGE(TAG, "Camera open failed (no negotiated resolution)");
        return -1;
    }

    size_t size = (size_t)w * h * 2;
    pthread_mutex_lock(&s_state_mutex);
    s_frame_buf = malloc(size);
    if (!s_frame_buf) {
        reset_frame_state_locked(false);
        pthread_mutex_unlock(&s_state_mutex);
        ESP_LOGE(TAG, "Failed to allocate %zu-byte frame buffer", size);
        call_camera_void(s_mid_close, "camera_close");
        return -1;
    }
    memset(s_frame_buf, 0, size);
    s_frame_size = size;
    pthread_mutex_unlock(&s_state_mutex);

    ESP_LOGI(TAG, "Camera opened: %" PRIu32 "x%" PRIu32, w, h);
    return FAKE_VIDEO_FD; /* Must be >= 0 (see scanner.c:926). */
}

esp_err_t app_video_set_bufs(int video_fd, uint32_t fb_num, const void **fb) {
    (void)video_fd;
    pthread_mutex_lock(&s_state_mutex);
    reset_user_bufs_locked();
    if (fb && fb_num > 0) {
        uint32_t count = fb_num < MAX_USER_BUFS ? fb_num : MAX_USER_BUFS;
        for (uint32_t i = 0; i < count; i++) {
            s_user_bufs[i] = (uint8_t *)fb[i];
        }
        s_num_user_bufs = count;
    }
    pthread_mutex_unlock(&s_state_mutex);
    return ESP_OK;
}

esp_err_t app_video_get_bufs(int fb_num, void **fb) {
    if (!fb) return ESP_FAIL;
    pthread_mutex_lock(&s_state_mutex);
    for (int i = 0; i < fb_num; i++) {
        uint32_t idx = (uint32_t)i;
        fb[i] = (s_num_user_bufs > 0 && idx < s_num_user_bufs && s_user_bufs[idx])
                    ? s_user_bufs[idx]
                    : s_frame_buf;
    }
    pthread_mutex_unlock(&s_state_mutex);
    return ESP_OK;
}

uint32_t app_video_get_buf_size(void) {
    pthread_mutex_lock(&s_state_mutex);
    uint32_t size = (uint32_t)s_frame_size;
    pthread_mutex_unlock(&s_state_mutex);
    return size;
}

esp_err_t app_video_get_resolution(uint32_t *width, uint32_t *height) {
    pthread_mutex_lock(&s_state_mutex);
    if (width) *width = s_width;
    if (height) *height = s_height;
    pthread_mutex_unlock(&s_state_mutex);
    return ESP_OK;
}

esp_err_t app_video_register_frame_operation_cb(app_video_frame_operation_cb_t cb) {
    pthread_mutex_lock(&s_state_mutex);
    s_frame_cb = cb;
    pthread_mutex_unlock(&s_state_mutex);
    return ESP_OK;
}

esp_err_t app_video_stream_task_start(int video_fd, int core_id) {
    (void)video_fd;
    (void)core_id;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&s_streaming, &expected, true,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return ESP_OK;
    }
    call_camera_void(s_mid_start, "camera_start");
    return ESP_OK;
}

esp_err_t app_video_stream_task_stop(int video_fd) {
    (void)video_fd;
    bool expected = true;
    if (!atomic_compare_exchange_strong_explicit(&s_streaming, &expected, false,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return ESP_OK;
    }
    call_camera_void(s_mid_stop, "camera_stop");
    return ESP_OK;
}

esp_err_t app_video_close(int video_fd) {
    app_video_stream_task_stop(video_fd);
    call_camera_void(s_mid_close, "camera_close");

    pthread_mutex_lock(&s_state_mutex);
    reset_frame_state_locked(true);
    pthread_mutex_unlock(&s_state_mutex);
    return ESP_OK;
}

esp_err_t app_video_set_ae_target(int video_fd, uint32_t level) {
    (void)video_fd;
    (void)level;
    return ESP_OK;
}

esp_err_t app_video_set_focus(int video_fd, uint32_t position) {
    (void)video_fd;
    (void)position;
    return ESP_OK;
}

bool app_video_has_focus_motor(int video_fd) {
    (void)video_fd;
    return false;
}

esp_err_t app_video_disable_af(void) {
    return ESP_OK;
}

esp_err_t app_video_deinit(void) {
    return ESP_OK;
}
