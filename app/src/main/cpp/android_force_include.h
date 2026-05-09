#pragma once

#include "mbedtls_compat.h"
#include "lvgl_compat.h"

#if defined(__ANDROID__)
#include <pthread.h>
int pthread_cancel(pthread_t thread);
#endif
