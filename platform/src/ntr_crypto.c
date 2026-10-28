/*
 * Hash and cipher routines that the SDK and the crypto library ship as
 * assembly or prebuilt code: SHA-1 (DGT "Hash2") and RC4.
 */
#include <string.h>

#include "ntr_shim.h"

#include <crypto/rc4.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* MD5 (DGT "Hash1") ------------------------------------------------------ */

static void Md5Block(DGTHash1Context *ctx, const u8 *block)
{
    static const u32 K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static const u8 R[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    u32 m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = block[i * 4] | (u32)block[i * 4 + 1] << 8 | (u32)block[i * 4 + 2] << 16 | (u32)block[i * 4 + 3] << 24;
    }
    u32 a = (u32)ctx->a, b = (u32)ctx->b, c = (u32)ctx->c, d = (u32)ctx->d;
    for (int i = 0; i < 64; i++) {
        u32 f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d), g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d, g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d), g = (7 * i) & 15;
        }
        u32 t = d;
        d = c;
        c = b;
        b = b + ROL(a + f + K[i] + m[g], R[i]);
        a = t;
    }
    ctx->a += a, ctx->b += b, ctx->c += c, ctx->d += d;
}

void DGT_Hash1Reset(DGTHash1Context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->a = 0x67452301;
    ctx->b = 0xefcdab89;
    ctx->c = 0x98badcfe;
    ctx->d = 0x10325476;
}

void DGT_Hash1SetSource(DGTHash1Context *ctx, const void *src, unsigned long len)
{
    const u8 *p = src;
    while (len > 0) {
        u32 used = (u32)(ctx->length & 63);
        u32 n = 64 - used;
        if (n > len) {
            n = (u32)len;
        }
        memcpy(ctx->buffer8 + used, p, n);
        ctx->length += n;
        p += n;
        len -= n;
        if ((ctx->length & 63) == 0) {
            Md5Block(ctx, ctx->buffer8);
        }
    }
}

void DGT_Hash1GetDigest_R(void *digest, DGTHash1Context *ctx)
{
    u64 bits = ctx->length * 8;
    static const u8 pad[64] = { 0x80 };
    u32 used = (u32)(ctx->length & 63);
    DGT_Hash1SetSource(ctx, pad, used < 56 ? 56 - used : 120 - used);
    u8 lenBytes[8];
    for (int i = 0; i < 8; i++) {
        lenBytes[i] = (u8)(bits >> (i * 8));
    }
    DGT_Hash1SetSource(ctx, lenBytes, 8);
    u32 h[4] = { (u32)ctx->a, (u32)ctx->b, (u32)ctx->c, (u32)ctx->d };
    for (int i = 0; i < 16; i++) {
        ((u8 *)digest)[i] = (u8)(h[i / 4] >> ((i % 4) * 8));
    }
}

/* SHA-1 (DGT "Hash2") ---------------------------------------------------- */

static void Sha1Block(DGTHash2Context *ctx, const u8 *block)
{
    u32 w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (u32)block[i * 4] << 24 | (u32)block[i * 4 + 1] << 16 | (u32)block[i * 4 + 2] << 8 | block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    u32 a = ctx->h0, b = ctx->h1, c = ctx->h2, d = ctx->h3, e = ctx->h4;
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) {
            f = (b & c) | (~b & d), k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d, k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d, k = 0xCA62C1D6;
        }
        u32 t = ROL(a, 5) + f + e + k + w[i];
        e = d, d = c, c = ROL(b, 30), b = a, a = t;
    }
    ctx->h0 += a, ctx->h1 += b, ctx->h2 += c, ctx->h3 += d, ctx->h4 += e;
}

void DGT_Hash2Reset(DGTHash2Context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->h0 = 0x67452301;
    ctx->h1 = 0xEFCDAB89;
    ctx->h2 = 0x98BADCFE;
    ctx->h3 = 0x10325476;
    ctx->h4 = 0xC3D2E1F0;
}

void DGT_Hash2SetSource(DGTHash2Context *ctx, const unsigned char *src, unsigned long len)
{
    u8 *buf = (u8 *)ctx->data;
    u32 bits = (u32)len << 3;
    ctx->Nh += (u32)(len >> 29) + (ctx->Nl + bits < ctx->Nl);
    ctx->Nl += bits;
    while (len > 0) {
        u32 n = 64 - (u32)ctx->num;
        if (n > len) {
            n = (u32)len;
        }
        memcpy(buf + ctx->num, src, n);
        ctx->num += (int)n;
        src += n;
        len -= n;
        if (ctx->num == 64) {
            Sha1Block(ctx, buf);
            ctx->num = 0;
        }
    }
}

void DGT_Hash2GetDigest(DGTHash2Context *ctx, unsigned char *digest)
{
    u8 *buf = (u8 *)ctx->data;
    u32 hi = ctx->Nh, lo = ctx->Nl;
    buf[ctx->num++] = 0x80;
    if (ctx->num > 56) {
        memset(buf + ctx->num, 0, 64 - ctx->num);
        Sha1Block(ctx, buf);
        ctx->num = 0;
    }
    memset(buf + ctx->num, 0, 56 - ctx->num);
    for (int i = 0; i < 4; i++) {
        buf[56 + i] = (u8)(hi >> (24 - i * 8));
        buf[60 + i] = (u8)(lo >> (24 - i * 8));
    }
    Sha1Block(ctx, buf);
    u32 h[5] = { ctx->h0, ctx->h1, ctx->h2, ctx->h3, ctx->h4 };
    for (int i = 0; i < 20; i++) {
        digest[i] = (u8)(h[i / 4] >> (24 - (i % 4) * 8));
    }
}

void CRYPTO_RC4Encrypt(CRYPTORC4Context *ctx, const void *in, u32 length, void *out)
{
    const u8 *src = in;
    u8 *dst = out;
    u8 i = ctx->i, j = ctx->j;
    for (u32 n = 0; n < length; n++) {
        i = (u8)(i + 1);
        u8 si = ctx->s[i];
        j = (u8)(j + si);
        ctx->s[i] = ctx->s[j];
        ctx->s[j] = si;
        dst[n] = src[n] ^ ctx->s[(u8)(ctx->s[i] + si)];
    }
    ctx->i = i;
    ctx->j = j;
}
