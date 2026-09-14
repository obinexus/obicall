#include "osal.h"

#if defined(_WIN32)
#include <windows.h>

int64_t osal_monotonic_ns(void) {
    static LARGE_INTEGER freq;
    static int have_freq = 0;
    if (!have_freq) {
        QueryPerformanceFrequency(&freq);
        have_freq = 1;
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * (1e9 / (double)freq.QuadPart));
}

int64_t osal_realtime_ns(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    const uint64_t epoch_diff_100ns = 116444736000000000ULL; /* 1601-01-01 -> 1970-01-01 */
    uint64_t hundred_ns = u.QuadPart - epoch_diff_100ns;
    return (int64_t)(hundred_ns * 100ULL);
}

#else
#include <time.h>

int64_t osal_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t osal_realtime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#endif
