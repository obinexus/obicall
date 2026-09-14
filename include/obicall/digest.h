#ifndef OBICALL_DIGEST_H
#define OBICALL_DIGEST_H

#include <stdint.h>
#include <stddef.h>

#include "obicall/platform.h"
#include "obicall/types.h"
#include "obicall/status.h"
#include "obicall/obicall_export.h"

OBICALL_BEGIN_DECLS

/* SHA-256, used for artifact/config/input digests. A hash proves an
 * artifact matches what a trusted manifest declared; it does not, by
 * itself, authenticate the manifest's publisher (docs/ABI.md "Trust
 * model"). */
OBICALL_API void OBICALL_CALL obicall_sha256(const uint8_t* data, size_t len,
                                              uint8_t out[OBICALL_DIGEST_LEN]);

typedef struct obicall_sha256_ctx {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t buffer[64];
    uint32_t buffer_len;
} obicall_sha256_ctx_t;

OBICALL_API void OBICALL_CALL obicall_sha256_init(obicall_sha256_ctx_t* ctx);
OBICALL_API void OBICALL_CALL obicall_sha256_update(obicall_sha256_ctx_t* ctx, const uint8_t* data,
                                                      size_t len);
OBICALL_API void OBICALL_CALL obicall_sha256_final(obicall_sha256_ctx_t* ctx,
                                                     uint8_t out[OBICALL_DIGEST_LEN]);

OBICALL_API uint32_t OBICALL_CALL obicall_crc32(const uint8_t* data, uint32_t len);

OBICALL_END_DECLS

#endif /* OBICALL_DIGEST_H */
