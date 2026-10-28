/* Minimal PNG writer (RGBA, stored deflate) for the desktop test tools. */
#include <stdio.h>
#include <stdlib.h>

#include "ntr_shim.h"

static u32 Crc32(u32 crc, const u8 *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
        }
    }
    return ~crc;
}

static void PutBE32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static void WriteChunk(FILE *f, const char *type, const u8 *data, u32 len)
{
    u8 hdr[8];
    PutBE32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    fwrite(data, 1, len, f);
    u32 crc = Crc32(Crc32(0, (const u8 *)type, 4), data, len);
    PutBE32(hdr, crc);
    fwrite(hdr, 1, 4, f);
}

/* Uncompressed (stored-deflate) RGBA PNG. */
void WritePngFile(const char *path, const u32 *rgba, int w, int h)
{
    size_t rowLen = (size_t)w * 4 + 1;
    size_t rawLen = rowLen * h;
    u8 *raw = malloc(rawLen);
    for (int y = 0; y < h; y++) {
        u8 *row = raw + y * rowLen;
        row[0] = 0;
        for (int x = 0; x < w; x++) {
            PutBE32(row + 1 + x * 4, rgba[y * w + x]);
        }
    }

    size_t blocks = (rawLen + 65534) / 65535;
    size_t zLen = 2 + rawLen + blocks * 5 + 4;
    u8 *z = malloc(zLen), *p = z;
    *p++ = 0x78;
    *p++ = 0x01;
    u32 a = 1, b = 0;
    for (size_t off = 0; off < rawLen; off += 65535) {
        size_t n = rawLen - off < 65535 ? rawLen - off : 65535;
        *p++ = off + n == rawLen;
        *p++ = (u8)n;
        *p++ = (u8)(n >> 8);
        *p++ = (u8)~n;
        *p++ = (u8)(~n >> 8);
        memcpy(p, raw + off, n);
        p += n;
    }
    for (size_t i = 0; i < rawLen; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    PutBE32(p, (b << 16) | a);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        exit(1);
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    u8 ihdr[13];
    PutBE32(ihdr, (u32)w);
    PutBE32(ihdr + 4, (u32)h);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    WriteChunk(f, "IHDR", ihdr, 13);
    WriteChunk(f, "IDAT", z, (u32)zLen);
    WriteChunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}
