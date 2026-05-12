/*
 * Android-specific FreeRTOS simulation. Adapted from
 * third_party/Kern/simulator/platform/esp_idf_stubs/freertos_sim.c — kept
 * in sync with that file EXCEPT for the vTaskSuspend implementation.
 *
 * Why this exists: Android bionic intentionally implements pthread_cancel
 * as a no-op (NDK has documented this for years). The upstream sim relies
 * on the desktop glibc semantics where vTaskDelete(handle) does
 * pthread_cancel() + pthread_join() to tear down a task that previously
 * called vTaskSuspend(NULL) → pause(). On Android the cancel is silently
 * ignored, the task sits in pause() forever, and vTaskDelete's join
 * blocks forever — which manifests as the scanner UI freezing on every
 * close (qr_decode_task → vTaskSuspend, qr_decoder_cleanup → vTaskDelete).
 *
 * Fix: vTaskSuspend(NULL) terminates the calling thread via pthread_exit
 * instead of parking it. Kern's use of vTaskSuspend(NULL) is always
 * preceded by xSemaphoreGive(done_sem) and followed (by another thread)
 * by vTaskDelete, never by vTaskResume, so terminating early is
 * semantically equivalent for this codebase. vTaskDelete's subsequent
 * pthread_cancel becomes a harmless no-op and pthread_join returns
 * immediately because the thread already exited.
 *
 * The rest of this file mirrors freertos_sim.c verbatim — keep them in
 * sync when bumping the submodule.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                            */
/* -------------------------------------------------------------------------- */

static struct timespec ticks_to_abstime(TickType_t ticks) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (ticks == portMAX_DELAY) {
        ts.tv_sec += 86400;
    } else {
        ts.tv_sec  += (time_t)(ticks / 1000);
        ts.tv_nsec += (long)((ticks % 1000) * 1000000L);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
    }
    return ts;
}

/* -------------------------------------------------------------------------- */
/* Tasks                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    pthread_t      thread;
    TaskFunction_t func;
    void          *param;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    bool            finished;
    bool            detached;
} task_impl_t;

static pthread_key_t s_task_key;
static pthread_once_t s_task_key_once = PTHREAD_ONCE_INIT;

static void create_task_key(void) {
    pthread_key_create(&s_task_key, NULL);
}

static void task_free(task_impl_t *t) {
    pthread_mutex_destroy(&t->mutex);
    pthread_cond_destroy(&t->cond);
    free(t);
}

static void task_finish(task_impl_t *t) {
    bool free_self = false;
    pthread_once(&s_task_key_once, create_task_key);
    pthread_mutex_lock(&t->mutex);
    t->finished = true;
    if (t->detached) {
        free_self = true;
    } else {
        pthread_cond_broadcast(&t->cond);
    }
    pthread_mutex_unlock(&t->mutex);

    if (free_self) {
        pthread_setspecific(s_task_key, NULL);
        task_free(t);
    }
}

static void *task_thread(void *arg) {
    task_impl_t *t = (task_impl_t *)arg;
    pthread_once(&s_task_key_once, create_task_key);
    pthread_setspecific(s_task_key, t);
    t->func(t->param);
    task_finish(t);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t func, const char *name,
                       uint32_t stack_size, void *param,
                       UBaseType_t priority, TaskHandle_t *handle) {
    (void)name;
    (void)stack_size;
    (void)priority;

    task_impl_t *t = calloc(1, sizeof(task_impl_t));
    if (!t) return pdFAIL;

    t->func = func;
    t->param = param;
    t->detached = (handle == NULL);
    pthread_mutex_init(&t->mutex, NULL);
    pthread_cond_init(&t->cond, NULL);

    if (pthread_create(&t->thread, NULL, task_thread, t) != 0) {
        task_free(t);
        return pdFAIL;
    }

    if (t->detached)
        pthread_detach(t->thread);

    if (handle) *handle = t;
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t func, const char *name,
                                   uint32_t stack_size, void *param,
                                   UBaseType_t priority, TaskHandle_t *handle,
                                   int core_id) {
    (void)core_id;
    return xTaskCreate(func, name, stack_size, param, priority, handle);
}

void vTaskDelete(TaskHandle_t handle) {
    if (handle == NULL) {
        pthread_once(&s_task_key_once, create_task_key);
        task_impl_t *self = (task_impl_t *)pthread_getspecific(s_task_key);
        if (self)
            task_finish(self);
        pthread_exit(NULL);
        /* unreachable */
    }

    task_impl_t *t = (task_impl_t *)handle;
    pthread_cancel(t->thread);

    struct timespec abs = ticks_to_abstime(pdMS_TO_TICKS(100));
    pthread_mutex_lock(&t->mutex);
    while (!t->finished) {
        int rc = pthread_cond_timedwait(&t->cond, &t->mutex, &abs);
        if (rc != 0)
            break;
    }

    if (!t->finished) {
        t->detached = true;
        pthread_detach(t->thread);
        pthread_mutex_unlock(&t->mutex);
        return;
    }
    pthread_mutex_unlock(&t->mutex);

    pthread_join(t->thread, NULL);
    task_free(t);
}

void vTaskDelay(TickType_t ticks) {
    usleep((useconds_t)ticks * 1000U);
}

void vTaskSuspend(TaskHandle_t handle) {
    (void)handle;
    /* See file header — pthread_exit instead of pause() for bionic. */
    pthread_once(&s_task_key_once, create_task_key);
    task_impl_t *self = (task_impl_t *)pthread_getspecific(s_task_key);
    if (self)
        task_finish(self);
    pthread_exit(NULL);
}

/* -------------------------------------------------------------------------- */
/* Semaphores                                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int             value;
    bool            is_binary;
} sem_impl_t;

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    sem_impl_t *s = calloc(1, sizeof(sem_impl_t));
    if (!s) return NULL;
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->value     = 0;
    s->is_binary = true;
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout) {
    sem_impl_t *s = (sem_impl_t *)sem;
    if (!s) return pdFAIL;

    pthread_mutex_lock(&s->mutex);

    if (timeout == 0) {
        if (s->value > 0) {
            s->value--;
            pthread_mutex_unlock(&s->mutex);
            return pdPASS;
        }
        pthread_mutex_unlock(&s->mutex);
        return pdFAIL;
    }

    struct timespec abs;
    if (timeout != portMAX_DELAY) abs = ticks_to_abstime(timeout);
    while (s->value <= 0) {
        int rc = (timeout == portMAX_DELAY)
            ? pthread_cond_wait(&s->cond, &s->mutex)
            : pthread_cond_timedwait(&s->cond, &s->mutex, &abs);
        if (rc != 0) {
            pthread_mutex_unlock(&s->mutex);
            return pdFAIL;
        }
    }
    s->value--;
    pthread_mutex_unlock(&s->mutex);
    return pdPASS;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    sem_impl_t *s = (sem_impl_t *)sem;
    if (!s) return pdFAIL;
    pthread_mutex_lock(&s->mutex);
    if (s->is_binary)
        s->value = 1;
    else
        s->value++;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    return pdPASS;
}

void vSemaphoreDelete(SemaphoreHandle_t sem) {
    sem_impl_t *s = (sem_impl_t *)sem;
    if (!s) return;
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);
    free(s);
}

/* -------------------------------------------------------------------------- */
/* Queues                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond_not_empty;
    pthread_cond_t  cond_not_full;
    uint8_t        *buffer;
    size_t          item_size;
    size_t          capacity;
    size_t          head;
    size_t          tail;
    size_t          count;
} queue_impl_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    queue_impl_t *q = calloc(1, sizeof(queue_impl_t));
    if (!q) return NULL;
    q->buffer = malloc((size_t)length * item_size);
    if (!q->buffer) { free(q); return NULL; }
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond_not_empty, NULL);
    pthread_cond_init(&q->cond_not_full, NULL);
    q->item_size = item_size;
    q->capacity  = length;
    return q;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout) {
    queue_impl_t *q = (queue_impl_t *)queue;
    if (!q || !item) return pdFAIL;

    pthread_mutex_lock(&q->mutex);

    if (timeout == 0) {
        if (q->count >= q->capacity) {
            pthread_mutex_unlock(&q->mutex);
            return pdFAIL;
        }
    } else {
        struct timespec abs;
        if (timeout != portMAX_DELAY) abs = ticks_to_abstime(timeout);
        while (q->count >= q->capacity) {
            int rc = (timeout == portMAX_DELAY)
                ? pthread_cond_wait(&q->cond_not_full, &q->mutex)
                : pthread_cond_timedwait(&q->cond_not_full, &q->mutex, &abs);
            if (rc != 0) {
                pthread_mutex_unlock(&q->mutex);
                return pdFAIL;
            }
        }
    }

    memcpy(q->buffer + q->tail * q->item_size, item, q->item_size);
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    pthread_cond_signal(&q->cond_not_empty);
    pthread_mutex_unlock(&q->mutex);
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *buffer, TickType_t timeout) {
    queue_impl_t *q = (queue_impl_t *)queue;
    if (!q || !buffer) return pdFAIL;

    pthread_mutex_lock(&q->mutex);

    if (timeout == 0) {
        if (q->count == 0) {
            pthread_mutex_unlock(&q->mutex);
            return pdFAIL;
        }
    } else {
        struct timespec abs;
        if (timeout != portMAX_DELAY) abs = ticks_to_abstime(timeout);
        while (q->count == 0) {
            int rc = (timeout == portMAX_DELAY)
                ? pthread_cond_wait(&q->cond_not_empty, &q->mutex)
                : pthread_cond_timedwait(&q->cond_not_empty, &q->mutex, &abs);
            if (rc != 0) {
                pthread_mutex_unlock(&q->mutex);
                return pdFAIL;
            }
        }
    }

    memcpy(buffer, q->buffer + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->cond_not_full);
    pthread_mutex_unlock(&q->mutex);
    return pdPASS;
}

void vQueueDelete(QueueHandle_t queue) {
    queue_impl_t *q = (queue_impl_t *)queue;
    if (!q) return;
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond_not_empty);
    pthread_cond_destroy(&q->cond_not_full);
    free(q->buffer);
    free(q);
}

/* -------------------------------------------------------------------------- */
/* Event Groups                                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    EventBits_t     bits;
} eg_impl_t;

EventGroupHandle_t xEventGroupCreate(void) {
    eg_impl_t *eg = calloc(1, sizeof(eg_impl_t));
    if (!eg) return NULL;
    pthread_mutex_init(&eg->mutex, NULL);
    pthread_cond_init(&eg->cond, NULL);
    return eg;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) {
    eg_impl_t *eg = (eg_impl_t *)group;
    if (!eg) return 0;
    pthread_mutex_lock(&eg->mutex);
    eg->bits |= bits;
    EventBits_t result = eg->bits;
    pthread_cond_broadcast(&eg->cond);
    pthread_mutex_unlock(&eg->mutex);
    return result;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
    eg_impl_t *eg = (eg_impl_t *)group;
    if (!eg) return 0;
    pthread_mutex_lock(&eg->mutex);
    EventBits_t prev = eg->bits;
    eg->bits &= ~bits;
    pthread_mutex_unlock(&eg->mutex);
    return prev;
}

EventBits_t xEventGroupGetBits(EventGroupHandle_t group) {
    eg_impl_t *eg = (eg_impl_t *)group;
    if (!eg) return 0;
    pthread_mutex_lock(&eg->mutex);
    EventBits_t result = eg->bits;
    pthread_mutex_unlock(&eg->mutex);
    return result;
}

void vEventGroupDelete(EventGroupHandle_t group) {
    eg_impl_t *eg = (eg_impl_t *)group;
    if (!eg) return;
    pthread_mutex_destroy(&eg->mutex);
    pthread_cond_destroy(&eg->cond);
    free(eg);
}
