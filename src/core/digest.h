#ifndef FORGE_DIGEST_H
#define FORGE_DIGEST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SHA-256 over bytes, not a semantic identity or an authentication mechanism. */
typedef struct {
    uint32_t words[8];
    uint64_t bytes;
    unsigned char block[64];
    size_t used;
    bool failed;
} fg_sha256;
void fg_sha256_init(fg_sha256 *);
bool fg_sha256_update(fg_sha256 *, const void *, size_t);
bool fg_sha256_final(fg_sha256 *, unsigned char digest[32]);
bool fg_sha256_finish_hex(fg_sha256 *, char hex[65]);
bool fg_sha256_hex(const void *, size_t, char hex[65]);
/* Canonical unsigned big-endian length prefix followed by the exact bytes. */
bool fg_sha256_field(fg_sha256 *, const void *, size_t);
bool fg_sha256_u64(fg_sha256 *, uint64_t);
bool fg_sha256_valid_hex(const char *);
#endif
