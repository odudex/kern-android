#include <pthread.h>

int pthread_cancel(pthread_t thread) {
    (void)thread;
    return 0;
}
