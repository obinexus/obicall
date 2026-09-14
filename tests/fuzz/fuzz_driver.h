#ifndef OBICALL_FUZZ_DRIVER_H
#define OBICALL_FUZZ_DRIVER_H

/*
 * Every fuzz target here defines LLVMFuzzerTestOneInput in the standard
 * shape, so it builds unmodified under real libFuzzer
 * (-fsanitize=fuzzer, Clang only) for coverage-guided fuzzing where that
 * toolchain is available.
 *
 * This environment's available compilers (MSYS2 UCRT64 GCC, TDM-GCC) do
 * not ship libFuzzer - GCC has no -fsanitize=fuzzer, and no Clang
 * toolchain was set up for this build (see docs/VALIDATION.md, "not
 * tested"). OBICALL_FUZZ_LITE_DRIVER below is a standalone substitute:
 * not coverage-guided, but a real call into the same entry point with
 * random bytes across many iterations, so it still catches crashes and
 * UB (louder still under OBICALL_ENABLE_SANITIZERS on a platform that
 * has libasan/libubsan) without requiring Clang.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

#ifdef OBICALL_FUZZ_LITE_DRIVER
int main(int argc, char** argv) {
    unsigned int seed = (argc > 1) ? (unsigned int)strtoul(argv[1], NULL, 10) : (unsigned int)time(NULL);
    int iterations = (argc > 2) ? atoi(argv[2]) : 20000;
    srand(seed);

    uint8_t buf[4096];
    for (int i = 0; i < iterations; ++i) {
        uint32_t len = (uint32_t)(rand() % (int)sizeof(buf));
        for (uint32_t j = 0; j < len; ++j) buf[j] = (uint8_t)rand();
        LLVMFuzzerTestOneInput(buf, len);
    }
    fprintf(stderr, "fuzz-lite: %d iterations, seed=%u, no crash\n", iterations, seed);
    return 0;
}
#endif

#endif /* OBICALL_FUZZ_DRIVER_H */
