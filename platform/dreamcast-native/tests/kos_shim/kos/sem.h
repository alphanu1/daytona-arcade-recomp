/* Host shim of KOS for tests/test_scene_loader.c: counting semaphores on pthreads. */
#pragma once
#include <pthread.h>
typedef struct { pthread_mutex_t lock; pthread_cond_t cond; int count; } semaphore_t;
static inline int sem_init(semaphore_t *s, int count) {
    pthread_mutex_init(&s->lock, NULL); pthread_cond_init(&s->cond, NULL); s->count = count; return 0;
}
static inline int sem_wait(semaphore_t *s) {
    pthread_mutex_lock(&s->lock);
    while (!s->count) pthread_cond_wait(&s->cond, &s->lock);
    --s->count; pthread_mutex_unlock(&s->lock); return 0;
}
static inline int sem_signal(semaphore_t *s) {
    pthread_mutex_lock(&s->lock); ++s->count; pthread_cond_signal(&s->cond); pthread_mutex_unlock(&s->lock); return 0;
}
