#include "osal.h"

#include <stdlib.h>

#if defined(_WIN32)

#include <windows.h>

struct osal_thread {
    HANDLE h;
};
struct osal_mutex {
    CRITICAL_SECTION cs;
};

typedef struct trampoline_args {
    osal_thread_fn fn;
    void* arg;
} trampoline_args_t;

static DWORD WINAPI trampoline(LPVOID p) {
    trampoline_args_t* t = (trampoline_args_t*)p;
    t->fn(t->arg);
    free(t);
    return 0;
}

int osal_thread_start(osal_thread_fn fn, void* arg, osal_thread_t** out_thread) {
    trampoline_args_t* t = (trampoline_args_t*)malloc(sizeof(*t));
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    HANDLE h = CreateThread(NULL, 0, trampoline, t, 0, NULL);
    if (!h) { free(t); return -1; }
    osal_thread_t* ot = (osal_thread_t*)malloc(sizeof(*ot));
    ot->h = h;
    *out_thread = ot;
    return 0;
}

void osal_thread_join(osal_thread_t* thread) {
    if (!thread) return;
    WaitForSingleObject(thread->h, INFINITE);
    CloseHandle(thread->h);
    free(thread);
}

int osal_thread_start_detached(osal_thread_fn fn, void* arg) {
    trampoline_args_t* t = (trampoline_args_t*)malloc(sizeof(*t));
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    HANDLE h = CreateThread(NULL, 0, trampoline, t, 0, NULL);
    if (!h) { free(t); return -1; }
    CloseHandle(h); /* thread keeps running; only the handle is released */
    return 0;
}

void osal_sleep_ms(int ms) { Sleep((DWORD)ms); }

int osal_mutex_init(osal_mutex_t** out) {
    osal_mutex_t* m = (osal_mutex_t*)malloc(sizeof(*m));
    if (!m) return -1;
    InitializeCriticalSection(&m->cs);
    *out = m;
    return 0;
}
void osal_mutex_lock(osal_mutex_t* m) { EnterCriticalSection(&m->cs); }
void osal_mutex_unlock(osal_mutex_t* m) { LeaveCriticalSection(&m->cs); }
void osal_mutex_destroy(osal_mutex_t* m) {
    if (!m) return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

#else

#include <pthread.h>
#include <time.h>

struct osal_thread {
    pthread_t t;
};
struct osal_mutex {
    pthread_mutex_t m;
};

typedef struct trampoline_args {
    osal_thread_fn fn;
    void* arg;
} trampoline_args_t;

static void* trampoline(void* p) {
    trampoline_args_t* t = (trampoline_args_t*)p;
    t->fn(t->arg);
    free(t);
    return NULL;
}

int osal_thread_start(osal_thread_fn fn, void* arg, osal_thread_t** out_thread) {
    trampoline_args_t* t = (trampoline_args_t*)malloc(sizeof(*t));
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    osal_thread_t* ot = (osal_thread_t*)malloc(sizeof(*ot));
    if (!ot) { free(t); return -1; }
    if (pthread_create(&ot->t, NULL, trampoline, t) != 0) {
        free(t);
        free(ot);
        return -1;
    }
    *out_thread = ot;
    return 0;
}

void osal_thread_join(osal_thread_t* thread) {
    if (!thread) return;
    pthread_join(thread->t, NULL);
    free(thread);
}

int osal_thread_start_detached(osal_thread_fn fn, void* arg) {
    trampoline_args_t* t = (trampoline_args_t*)malloc(sizeof(*t));
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(t); return -1; }
    return 0;
}

void osal_sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int osal_mutex_init(osal_mutex_t** out) {
    osal_mutex_t* m = (osal_mutex_t*)malloc(sizeof(*m));
    if (!m) return -1;
    pthread_mutex_init(&m->m, NULL);
    *out = m;
    return 0;
}
void osal_mutex_lock(osal_mutex_t* m) { pthread_mutex_lock(&m->m); }
void osal_mutex_unlock(osal_mutex_t* m) { pthread_mutex_unlock(&m->m); }
void osal_mutex_destroy(osal_mutex_t* m) {
    if (!m) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

#endif
