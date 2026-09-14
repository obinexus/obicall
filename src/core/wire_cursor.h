#ifndef OBICALL_WIRE_CURSOR_H
#define OBICALL_WIRE_CURSOR_H

#include <stdint.h>
#include <string.h>

typedef struct wcursor {
    uint8_t* p;
    uint32_t cap;
    uint32_t off;
} wcursor_t;

typedef struct rcursor {
    const uint8_t* p;
    uint32_t len;
    uint32_t off;
} rcursor_t;

static inline void wc_bytes(wcursor_t* c, const void* src, uint32_t n, int* ok) {
    if (!*ok) return;
    if (n > c->cap - c->off) { *ok = 0; return; }
    memcpy(c->p + c->off, src, n);
    c->off += n;
}
static inline void wc_u8(wcursor_t* c, uint8_t v, int* ok) { wc_bytes(c, &v, 1, ok); }
static inline void wc_u16(wcursor_t* c, uint16_t v, int* ok) {
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    wc_bytes(c, b, 2, ok);
}
static inline void wc_u32(wcursor_t* c, uint32_t v, int* ok) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    wc_bytes(c, b, 4, ok);
}
static inline void wc_u64(wcursor_t* c, uint64_t v, int* ok) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (8 * i));
    wc_bytes(c, b, 8, ok);
}
static inline void wc_i64(wcursor_t* c, int64_t v, int* ok) { wc_u64(c, (uint64_t)v, ok); }
static inline void wc_f64(wcursor_t* c, double v, int* ok) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    wc_u64(c, bits, ok);
}
static inline void wc_fixed(wcursor_t* c, const char* s, uint32_t fixed_len, int* ok) {
    wc_bytes(c, s, fixed_len, ok);
}

static inline void rc_bytes(rcursor_t* c, void* dst, uint32_t n, int* ok) {
    if (!*ok) return;
    if (n > c->len - c->off) { *ok = 0; return; }
    memcpy(dst, c->p + c->off, n);
    c->off += n;
}
static inline uint8_t rc_u8(rcursor_t* c, int* ok) {
    uint8_t v = 0;
    rc_bytes(c, &v, 1, ok);
    return v;
}
static inline uint16_t rc_u16(rcursor_t* c, int* ok) {
    uint8_t b[2] = {0};
    rc_bytes(c, b, 2, ok);
    return (uint16_t)(b[0] | (b[1] << 8));
}
static inline uint32_t rc_u32(rcursor_t* c, int* ok) {
    uint8_t b[4] = {0};
    rc_bytes(c, b, 4, ok);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline uint64_t rc_u64(rcursor_t* c, int* ok) {
    uint8_t b[8] = {0};
    rc_bytes(c, b, 8, ok);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= ((uint64_t)b[i]) << (8 * i);
    return v;
}
static inline int64_t rc_i64(rcursor_t* c, int* ok) { return (int64_t)rc_u64(c, ok); }
static inline double rc_f64(rcursor_t* c, int* ok) {
    uint64_t bits = rc_u64(c, ok);
    double v;
    memcpy(&v, &bits, 8);
    return v;
}
static inline void rc_fixed(rcursor_t* c, char* dst, uint32_t fixed_len, int* ok) {
    rc_bytes(c, dst, fixed_len, ok);
}

#endif /* OBICALL_WIRE_CURSOR_H */
