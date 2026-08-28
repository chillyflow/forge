#include "digest.h"
#include <string.h>

static uint32_t rotate(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}
static void transform(fg_sha256 *state, const unsigned char block[64]) {
    static const uint32_t constants[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};
    uint32_t words[64];
    for (size_t i = 0; i < 16; i++) {
        const unsigned char *p = block + i * 4;
        words[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
                   (uint32_t)p[3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t a = words[i - 15], b = words[i - 2];
        words[i] = words[i - 16] + (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) + words[i - 7] +
                   (rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10));
    }
    uint32_t a = state->words[0], b = state->words[1], c = state->words[2], d = state->words[3],
             e = state->words[4], f = state->words[5], g = state->words[6], h = state->words[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t first = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) +
                         constants[i] + words[i];
        uint32_t second =
            (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state->words[0] += a;
    state->words[1] += b;
    state->words[2] += c;
    state->words[3] += d;
    state->words[4] += e;
    state->words[5] += f;
    state->words[6] += g;
    state->words[7] += h;
}
void fg_sha256_init(fg_sha256 *state) {
    if (!state)
        return;
    static const uint32_t initial[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memset(state, 0, sizeof(*state));
    memcpy(state->words, initial, sizeof(initial));
}
bool fg_sha256_update(fg_sha256 *state, const void *data, size_t size) {
    if (!state || state->failed)
        return false;
    if ((!data && size) || size > UINT64_MAX / 8 - state->bytes) {
        state->failed = true;
        return false;
    }
    const unsigned char *p = data;
    state->bytes += size;
    while (size) {
        size_t take = 64 - state->used;
        if (take > size)
            take = size;
        memcpy(state->block + state->used, p, take);
        state->used += take;
        p += take;
        size -= take;
        if (state->used == 64) {
            transform(state, state->block);
            state->used = 0;
        }
    }
    return true;
}
bool fg_sha256_final(fg_sha256 *state, unsigned char digest[32]) {
    if (!state || !digest || state->failed)
        return false;
    uint64_t bits = state->bytes * 8;
    state->block[state->used++] = 0x80;
    if (state->used > 56) {
        memset(state->block + state->used, 0, 64 - state->used);
        transform(state, state->block);
        state->used = 0;
    }
    memset(state->block + state->used, 0, 56 - state->used);
    for (size_t i = 0; i < 8; i++) {
        state->block[63 - i] = (unsigned char)(bits & 255u);
        bits >>= 8;
    }
    transform(state, state->block);
    for (size_t i = 0; i < 8; i++)
        for (size_t j = 0; j < 4; j++)
            digest[i * 4 + j] = (unsigned char)(state->words[i] >> ((3 - j) * 8));
    state->failed = true; /* A finished context cannot be reused without init. */
    return true;
}
bool fg_sha256_finish_hex(fg_sha256 *state, char hex[65]) {
    static const char alphabet[] = "0123456789abcdef";
    unsigned char digest[32];
    if (!hex || !fg_sha256_final(state, digest))
        return false;
    for (size_t i = 0; i < sizeof(digest); i++) {
        hex[i * 2] = alphabet[digest[i] >> 4];
        hex[i * 2 + 1] = alphabet[digest[i] & 15u];
    }
    hex[64] = 0;
    return true;
}
bool fg_sha256_hex(const void *data, size_t size, char hex[65]) {
    fg_sha256 state;
    fg_sha256_init(&state);
    return fg_sha256_update(&state, data, size) && fg_sha256_finish_hex(&state, hex);
}
bool fg_sha256_u64(fg_sha256 *state, uint64_t value) {
    unsigned char bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[7 - i] = (unsigned char)(value & 255u);
        value >>= 8;
    }
    return fg_sha256_update(state, bytes, sizeof(bytes));
}
bool fg_sha256_field(fg_sha256 *state, const void *data, size_t size) {
    return fg_sha256_u64(state, size) && fg_sha256_update(state, data, size);
}
bool fg_sha256_valid_hex(const char *hex) {
    if (!hex)
        return false;
    for (size_t i = 0; i < 64; i++)
        if (!((hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'a' && hex[i] <= 'f')))
            return false;
    return !hex[64];
}
