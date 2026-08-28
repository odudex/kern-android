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
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

extern "C" {
#include "bsp/pmic.h"
#include "core/entropy_pool.h"
#include "core/nvs_secure.h"
#include "core/pin.h"
#include "core/storage.h"
#include "core/settings.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "pages/session_lock.h"
#include "sim_flash.h"
#include "sim_nvs.h"
#include "sim_sdcard.h"
#include "ui/assets/kern_logo_lvgl.h"
#include "ui/entropy_input.h"
#include "ui/theme.h"
#include "ui/theme_widgets.h"
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

void splash_done_cb(lv_timer_t *timer) {
    lv_timer_delete(timer);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    session_lock_boot_gate(scr);
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
            // Present outside the LVGL lock: the blocking ANativeWindow
            // blit/post must not hold off the camera frame callback's
            // non-blocking display lock, or the preview starves.
            kern_android_display_flush_pending();
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
    // The mock SD card gets its own subdir (mirroring main_sim.c's layout)
    // so the file browser doesn't expose the nvs/ and spiffs/ backing stores
    // that live alongside it in the app files dir.
    std::string sdcard_dir = base_dir + "/sdcard";
    sim_nvs_set_data_dir(nvs_dir.c_str());
    sim_flash_set_data_dir(flash_dir.c_str());
    sim_sdcard_set_data_dir(sdcard_dir.c_str());

    // Older builds mounted the mock SD card at base_dir itself; user files
    // (mnemonics, descriptors) were saved under <base>/kern. Move that tree
    // under the new root once so existing data stays visible.
    std::string old_kern = base_dir + "/kern";
    std::string new_kern = sdcard_dir + "/kern";
    struct stat st {};
    if (stat(old_kern.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
        stat(new_kern.c_str(), &st) != 0) {
        mkdir(sdcard_dir.c_str(), 0755);
        if (rename(old_kern.c_str(), new_kern.c_str()) != 0) {
            __android_log_print(ANDROID_LOG_WARN, LOG_TAG,
                                "SD-card migration failed for %s", old_kern.c_str());
        }
    }

    // Seed before anything can ask for randomness (mirrors app_main)
    entropy_pool_init();

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

    // Feed touch events into the entropy pool. The render loop is not running
    // yet, so no LVGL lock is needed here.
    entropy_input_attach();

    theme_init();
    lv_obj_t *scr = lv_screen_active();
    theme_apply_screen(scr);
    lv_refr_now(nullptr);

    esp_err_t ret = nvs_secure_init();
    if (ret != ESP_OK) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "nvs_secure_init failed: 0x%x", ret);
        return false;
    }

    ret = storage_init();
    if (ret != ESP_OK) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "storage_init failed for %s: 0x%x", flash_dir.c_str(), ret);
        return false;
    }

    // Not fatal: every getter falls back to its default when the namespace is
    // unavailable, and those defaults are the safe ones.
    esp_err_t settings_ret = settings_init();
    if (settings_ret != ESP_OK) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "settings_init failed, using defaults: 0x%x", settings_ret);
    }

    bsp_pmic_init();

    esp_err_t video_ret = app_video_init_once(nullptr);
    if (video_ret != ESP_OK) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG,
                            "Video pipeline init failed: 0x%x", video_ret);
    }

    kern_logo_animated(scr);
    if (!bip39_filter_init()) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "bip39_filter_init failed");
    }

    // Fail closed: without it pin_is_configured() reports false and the boot
    // gate would walk straight past the PIN of a device that has one set.
    esp_err_t pin_ret = pin_init();
    if (pin_ret != ESP_OK) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "pin_init failed: 0x%x", pin_ret);
        return false;
    }

    session_lock_init();

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
