#include "android_lvgl_display.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>

extern "C" {
#include "bsp/pmic.h"
#include "core/pin.h"
#include "core/storage.h"
#include "utils/session.h"
#include "core/settings.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "nvs_flash.h"
#include "pages/login/login.h"
#include "pages/pin/pin_page.h"
#include "sim_flash.h"
#include "sim_nvs.h"
#include "sim_sdcard.h"
#include "ui/assets/kern_logo_lvgl.h"
#include "ui/theme.h"
#include "utils/bip39_filter.h"
#include "video/video.h"
#include "wally_core.h"
}

#define LOG_TAG "KernAndroid"

namespace {

std::mutex g_lifecycle_mutex;
std::thread g_loop_thread;
std::atomic_bool g_running {false};
std::atomic_bool g_paused {false};
bool g_created = false;

JavaVM *g_jvm = nullptr;
jclass g_main_activity_class = nullptr;
jmethodID g_mid_request_finish = nullptr;

void cache_finish_app_binding(JNIEnv *env) {
    if (g_jvm) return;
    if (env->GetJavaVM(&g_jvm) != JNI_OK) {
        g_jvm = nullptr;
        return;
    }
    jclass local = env->FindClass("com/odudex/kern/MainActivity");
    if (!local) {
        env->ExceptionClear();
        return;
    }
    g_main_activity_class = (jclass)env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    if (!g_main_activity_class) return;
    g_mid_request_finish =
        env->GetStaticMethodID(g_main_activity_class, "requestFinish", "()V");
    if (!g_mid_request_finish) {
        env->ExceptionClear();
    }
}

void post_unlock_cb();
void session_expired_handler();

void session_expired_handler() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    pin_page_create(scr, PIN_PAGE_UNLOCK, post_unlock_cb, nullptr);
}

void post_unlock_cb() {
    pin_page_destroy();

    uint16_t timeout = pin_get_session_timeout();
    if (timeout > 0) {
        session_start(timeout);
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    login_page_create(scr);
}

void splash_done_cb(lv_timer_t *timer) {
    lv_timer_delete(timer);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    if (pin_is_configured()) {
        pin_page_create(scr, PIN_PAGE_UNLOCK, post_unlock_cb, nullptr);
    } else {
        login_page_create(scr);
    }
}

void render_loop() {
    while (g_running.load()) {
        if (g_paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        uint32_t wait_ms = 16;
        if (lvgl_port_lock(50)) {
            wait_ms = lv_timer_handler();
            lvgl_port_unlock();
        }
        if (wait_ms > 33) wait_ms = 33;
        if (wait_ms < 1) wait_ms = 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }
}

bool start_kern(ANativeWindow *window, int width, int height,
                const char *files_dir, const char *board) {
    lvgl_port_cfg_t cfg {};
    lvgl_port_init(&cfg);

    std::string base_dir = files_dir ? files_dir : "/data/data/com.odudex.kern/files";
    std::string nvs_dir = base_dir + "/nvs";
    std::string flash_dir = base_dir + "/spiffs";
    sim_nvs_set_data_dir(nvs_dir.c_str());
    sim_flash_set_data_dir(flash_dir.c_str());
    sim_sdcard_set_data_dir(base_dir.c_str());

    if (wally_init(0) != WALLY_OK) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "wally_init failed");
        return false;
    }

    lv_init();
    lv_display_t *display = nullptr;
    lv_indev_t *indev = nullptr;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "Starting board profile %s on Android surface %dx%d",
                        board ? board : "default", width, height);
    if (!kern_android_display_create(SIM_LCD_H_RES, SIM_LCD_V_RES,
                                     KERN_ANDROID_LAYOUT_BOARD, window, &display, &indev)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "LVGL Android display init failed");
        return false;
    }
    (void)display;
    (void)indev;

    theme_init();
    lv_obj_t *scr = lv_screen_active();
    theme_apply_screen(scr);
    lv_refr_now(nullptr);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "nvs_flash_init failed: 0x%x", ret);
        return false;
    }

    ret = storage_init();
    if (ret != ESP_OK) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "storage_init failed for %s: 0x%x", flash_dir.c_str(), ret);
        return false;
    }

    settings_init();
    bsp_pmic_init();

    esp_err_t video_ret = app_video_init_once(nullptr);
    if (video_ret != ESP_OK) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG,
                            "Video pipeline init failed: 0x%x", video_ret);
    }

    kern_logo_animated(scr);
    bip39_filter_init();
    pin_init();
    session_set_expired_callback(session_expired_handler);

    lv_timer_t *splash_timer = lv_timer_create(splash_done_cb, 3000, nullptr);
    lv_timer_set_repeat_count(splash_timer, 1);

    g_running.store(true);
    g_paused.store(false);
    g_loop_thread = std::thread(render_loop);
    return true;
}

const char *jstring_or_default(JNIEnv *env, jstring value, const char *fallback,
                               std::string &storage) {
    if (!value) return fallback;
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return fallback;
    storage = chars;
    env->ReleaseStringUTFChars(value, chars);
    return storage.c_str();
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_odudex_kern_KernNative_create(JNIEnv *env, jobject, jobject surface,
                                       jint width, jint height, jstring files_dir,
                                       jstring board) {
    std::lock_guard<std::mutex> guard(g_lifecycle_mutex);
    cache_finish_app_binding(env);
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) return;

    if (g_created) {
        if (lvgl_port_lock(100)) {
            kern_android_display_set_window(window);
            lvgl_port_unlock();
        } else {
            __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Could not lock LVGL for surface update");
            ANativeWindow_release(window);
        }
        g_paused.store(false);
        return;
    }

    std::string files_storage;
    std::string board_storage;
    const char *files = jstring_or_default(env, files_dir, nullptr, files_storage);
    const char *board_name = jstring_or_default(env, board, "wave_5", board_storage);

    g_created = start_kern(window, width, height, files, board_name);
    if (!g_created) {
        ANativeWindow_release(window);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_odudex_kern_KernNative_resize(JNIEnv *env, jobject, jobject surface,
                                       jint width, jint height) {
    std::lock_guard<std::mutex> guard(g_lifecycle_mutex);
    if (!g_created) return;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) return;
    if (lvgl_port_lock(100)) {
        kern_android_display_set_window(window);
        lvgl_port_unlock();
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Android surface resized to %dx%d",
                            width, height);
    } else {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Could not lock LVGL for resize");
        ANativeWindow_release(window);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_odudex_kern_KernNative_touch(JNIEnv *, jobject, jint action, jfloat x, jfloat y) {
    if (!g_created) return;
    kern_android_display_set_touch(action, x, y);
}

extern "C" JNIEXPORT void JNICALL
Java_com_odudex_kern_KernNative_pause(JNIEnv *, jobject) {
    g_paused.store(true);
}

extern "C" JNIEXPORT void JNICALL
Java_com_odudex_kern_KernNative_resume(JNIEnv *, jobject) {
    g_paused.store(false);
}

extern "C" JNIEXPORT void JNICALL
Java_com_odudex_kern_KernNative_destroy(JNIEnv *, jobject) {
    std::lock_guard<std::mutex> guard(g_lifecycle_mutex);
    if (!g_created) return;

    g_running.store(false);
    if (g_loop_thread.joinable()) {
        g_loop_thread.join();
    }
    kern_android_display_destroy();
    g_created = false;
}

// Called from patched esp_restart on the render thread (inside lv_timer_handler,
// holding the LVGL lock). Schedules MainActivity.finishAndRemoveTask on the
// UI thread, then pthread_exits — noreturn because esp_restart is.
extern "C" __attribute__((noreturn)) void kern_android_finish_app(void) {
    if (g_jvm && g_main_activity_class && g_mid_request_finish) {
        JNIEnv *env = nullptr;
        bool attached = false;
        jint res = g_jvm->GetEnv((void **)&env, JNI_VERSION_1_6);
        if (res == JNI_EDETACHED) {
            if (g_jvm->AttachCurrentThreadAsDaemon(&env, nullptr) == JNI_OK) {
                attached = true;
            } else {
                env = nullptr;
            }
        } else if (res != JNI_OK) {
            env = nullptr;
        }
        if (env) {
            env->CallStaticVoidMethod(g_main_activity_class, g_mid_request_finish);
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
            if (attached) g_jvm->DetachCurrentThread();
        }
    } else {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG,
                            "finish_app: JNI binding missing; exiting");
        _exit(0);
    }

    // Unblock KernNative.destroy's join(); the leaked LVGL lock dies with
    // the process when MainActivity.onDestroy calls exitProcess().
    g_running.store(false);
    pthread_exit(nullptr);
}
