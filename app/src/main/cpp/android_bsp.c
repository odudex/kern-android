#include "android_lvgl_display.h"

#include "bsp/config.h"
#include "bsp/display.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>

static const char *TAG = "BSP_ANDROID";
static pthread_mutex_t s_lvgl_mutex;
static pthread_once_t s_lvgl_mutex_once = PTHREAD_ONCE_INIT;

static void init_lvgl_mutex(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lvgl_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

static int mutex_timedlock_ms(pthread_mutex_t *mutex, uint32_t timeout_ms) {
    if (timeout_ms == 0) {
        return pthread_mutex_lock(mutex);
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (long)(timeout_ms / 1000);
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_mutex_timedlock(mutex, &ts);
}

esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg) {
    (void)cfg;
    pthread_once(&s_lvgl_mutex_once, init_lvgl_mutex);
    return ESP_OK;
}

esp_err_t lvgl_port_deinit(void) {
    return ESP_OK;
}

bool lvgl_port_lock(uint32_t timeout_ms) {
    pthread_once(&s_lvgl_mutex_once, init_lvgl_mutex);
    return mutex_timedlock_ms(&s_lvgl_mutex, timeout_ms) == 0;
}

void lvgl_port_unlock(void) {
    pthread_mutex_unlock(&s_lvgl_mutex);
}

void lvgl_port_flush_ready(lv_display_t *disp) {
    (void)disp;
}

void kern_lv_refr_now_real(lv_display_t *disp);

void lv_refr_now(lv_display_t *disp) {
    /* The flush callback in android_lvgl_display.cpp already calls
     * present_locked() on the last partial flush of a refr cycle, so we
     * don't repeat it here — doing so was double-presenting and hammering
     * ANativeWindow_lock on every Kern lv_refr_now() call. */
    kern_lv_refr_now_real(disp);
}

lv_display_t *bsp_display_start(void) {
    return NULL;
}

bool bsp_display_lock(uint32_t timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void) {
    lvgl_port_unlock();
}

esp_err_t bsp_display_brightness_init(void) {
    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int pct) {
    ESP_LOGI(TAG, "brightness_set(%d)", pct);
    return ESP_OK;
}

esp_err_t bsp_display_backlight_on(void) {
    return ESP_OK;
}

esp_err_t bsp_display_backlight_off(void) {
    return ESP_OK;
}

esp_err_t bsp_i2c_init(void) {
    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void) {
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void) {
    return (i2c_master_bus_handle_t)1;
}
