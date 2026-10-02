/* CoreGraphics - CGMD5.c
   Copyright (C) 2026, LibreDarwin
   SPDX-License-Identifier: BSD-3-Clause

   MD5, for the ICC profile ID field.

   An ICC profile's 16-byte ID is not an opaque commitment: it is the MD5 of
   the profile itself with two fields zeroed, the flags at 44-47 and the ID
   itself at 84-99.  Confirmed on five of Apple's own profiles, so the digest
   is reproduced here rather than calling into a crypto library.

   This is transcribed from the algorithm in RFC 1321.  It is implemented
   locally rather than reached through CommonCrypto because the digest is
   load-bearing for byte-exact colour profiles and a self-contained
   implementation keeps the port's dependency surface unchanged; the parity
   harness checks the result against Apple's own ID bytes on every profile it
   builds, so a mistake in here cannot pass unnoticed. */

#include "CGInternal.h"

#include <stdint.h>
#include <string.h>

/* K[i] = floor(abs(sin(i + 1)) * 2^32), the per-round additive constants. */
static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

/* The per-round left-rotation amounts. */
static const unsigned char S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static uint32_t rotl32(uint32_t v, unsigned n)
{
    return (v << n) | (v >> (32 - n));
}

static uint32_t load_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* The compression function, over one 64-byte block. */
static void md5_block(uint32_t state[4], const unsigned char *block)
{
    uint32_t x[16];
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];

    for (unsigned i = 0; i < 16; i++)
        x[i] = load_le32(block + i * 4);

    for (unsigned i = 0; i < 64; i++) {
        uint32_t f;
        unsigned g;

        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }

        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + K[i] + x[g], S[i]);
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void CGMD5(const void *data, size_t len, unsigned char out[16])
{
    const unsigned char *p = data;
    uint32_t state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };

    /* The message length in bits, little-endian, appended after the padding. */
    uint64_t bits = (uint64_t)len * 8;

    for (size_t i = 0; i + 64 <= len; i += 64)
        md5_block(state, p + i);

    /* The tail: whatever is left, then 0x80, then zeros, then the length. */
    unsigned char tail[128];
    size_t rest = len & 63;
    size_t padlen = (rest < 56) ? 64 : 128;

    memset(tail, 0, sizeof tail);
    memcpy(tail, p + (len - rest), rest);
    tail[rest] = 0x80;
    for (unsigned i = 0; i < 8; i++)
        tail[padlen - 8 + i] = (unsigned char)(bits >> (8 * i));
    md5_block(state, tail);
    if (padlen == 128)
        md5_block(state, tail + 64);

    for (unsigned i = 0; i < 4; i++)
        store_le32(out + i * 4, state[i]);
}
