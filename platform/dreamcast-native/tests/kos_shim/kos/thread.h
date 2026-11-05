/* Host shim of KOS for tests/test_scene_loader.c: threads on pthreads; thd_current names the caller's. */
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
typedef int prio_t;
#define PRIO_DEFAULT 10
typedef struct { pthread_t id; void *(*routine)(void *); void *arg; } kthread_t;
typedef struct kthread_attr { bool create_detached; size_t stack_size; void *stack_ptr; prio_t prio; const char *label; bool disable_tls; } kthread_attr_t;
extern _Thread_local kthread_t *shim_self;
#define thd_current shim_self
static void *shim_start(void *raw) { kthread_t *t = raw; shim_self = t; return t->routine(t->arg); }
static inline kthread_t *thd_create_ex(const kthread_attr_t *attr, void *(*routine)(void *), void *arg) {
    (void)attr;
    kthread_t *t = calloc(1, sizeof(*t));
    t->routine = routine; t->arg = arg;
    if (pthread_create(&t->id, NULL, shim_start, t)) { free(t); return NULL; }
    pthread_detach(t->id);
    return t;
}
