/*
 * MI (memory), FX/MTX (fixed point math) and FS (ROM file system) replacements.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntr_shim.h"

/* MI ------------------------------------------------------------------------ */

/* The SDK's MI_Cpu* functions are inline wrappers (mi/memory.h) around these
 * routines, which the SDK writes in assembly. */
void MI_CpuCopy8(const void *src, void *dest, u32 size) { memmove(dest, src, size); }
void MI_CpuFill8(void *dest, u8 data, u32 size) { memset(dest, data, size); }
void MIi_CpuCopy16(const void *srcp, void *destp, u32 size) { memmove(destp, srcp, size & ~1u); }
void MIi_CpuCopy32(const void *srcp, void *destp, u32 size) { memmove(destp, srcp, size & ~3u); }
void MIi_CpuCopyFast(const void *srcp, void *destp, u32 size) { memmove(destp, srcp, size & ~3u); }

void MIi_CpuClear16(u16 data, void *destp, u32 size)
{
    u16 *d = destp;
    for (u32 i = 0; i < size / 2; i++) {
        d[i] = data;
    }
}

void MIi_CpuClear32(u32 data, void *destp, u32 size)
{
    u32 *d = destp;
    for (u32 i = 0; i < size / 4; i++) {
        d[i] = data;
    }
}

void MIi_CpuClearFast(u32 data, void *destp, u32 size) { MIi_CpuClear32(data, destp, size); }

void MIi_CpuSend16(const void *srcp, volatile void *destp, u32 size)
{
    const u16 *s = srcp;
    for (u32 i = 0; i < size / 2; i++) {
        *(volatile u16 *)destp = s[i];
    }
}

void MI_Copy16B(const void *pSrc, void *pDest) { memmove(pDest, pSrc, 16); }
void MI_Copy32B(const void *pSrc, void *pDest) { memmove(pDest, pSrc, 32); }
void MI_Copy36B(const void *pSrc, void *pDest) { memmove(pDest, pSrc, 36); }
void MI_Copy48B(const void *pSrc, void *pDest) { memmove(pDest, pSrc, 48); }
void MI_Copy64B(const void *pSrc, void *pDest) { memmove(pDest, pSrc, 64); }
void MI_Copy128B(const void *pSrc, void *pDest) { memmove(pDest, pSrc, 128); }
void MI_Zero32B(void *pDest) { memset(pDest, 0, 32); }
void MI_Zero36B(void *pDest) { memset(pDest, 0, 36); }
void MI_Zero48B(void *pDest) { memset(pDest, 0, 48); }
void MI_Zero64B(void *pDest) { memset(pDest, 0, 64); }

/* Time in file reads and decompression (NtrHost_Ticks units), for profiling. */
u64 gNtrFsTicks, gNtrLzTicks;

static void UncompressLZ8(const void *src, void *dest);

/* Nintendo LZ77 (type 0x10). */
void MI_UncompressLZ8(const void *src, void *dest)
{
    u64 t0 = NtrHost_Ticks();
    UncompressLZ8(src, dest);
    gNtrLzTicks += NtrHost_Ticks() - t0;
}

static void UncompressLZ8(const void *src, void *dest)
{
    const u8 *s = src;
    u8 *d = dest;
    u32 remaining = MI_GetUncompressedSize(src);
    s += 4;

    while (remaining > 0) {
        u8 flags = *s++;
        for (int bit = 0; bit < 8 && remaining > 0; bit++, flags <<= 1) {
            if (flags & 0x80) {
                u32 len = (s[0] >> 4) + 3;
                u32 disp = (((s[0] & 0xf) << 8) | s[1]) + 1;
                s += 2;
                if (len > remaining) {
                    len = remaining;
                }
                remaining -= len;
                while (len--) {
                    *d = *(d - disp);
                    d++;
                }
            } else {
                *d++ = *s++;
                remaining--;
            }
        }
    }
}

/* CP: hardware divider and square root ----------------------------------------- */

/* Register offsets: DIVCNT 0x280, DIV_NUMER 0x290, DIV_DENOM 0x298,
 * DIV_RESULT 0x2A0, DIVREM_RESULT 0x2A8, SQRTCNT 0x2B0, SQRT_RESULT 0x2B4,
 * SQRT_PARAM 0x2B8. Results are produced when their address is taken for a read. */
static void Divide(void)
{
    int mode = NTR_IO16(0x280) & 3;
    s64 numer = *(s64 *)&gNtrIoRegs[0x290];
    s64 denom = *(s64 *)&gNtrIoRegs[0x298];
    s64 n = mode == 0 ? (s64)(s32)numer : numer;
    s64 d = mode >= 2 ? denom : (s64)(s32)denom;
    s64 q, r;

    if (d == 0) {
        /* The divider's result for a zero divisor. */
        q = n < 0 ? 1 : -1;
        r = n;
        if (mode == 0) {
            q = (s64)((u64)q ^ 0xffffffff00000000ull);
        }
    } else if (n == INT64_MIN && d == -1) {
        q = n;
        r = 0;
    } else {
        q = n / d;
        r = n % d;
    }
    *(s64 *)&gNtrIoRegs[0x2a0] = q;
    *(s64 *)&gNtrIoRegs[0x2a8] = r;
}

u32 NtrCp_DivResultAddr(void)
{
    Divide();
    return (u32)&gNtrIoRegs[0x2a0];
}

u32 NtrCp_DivRemResultAddr(void)
{
    Divide();
    return (u32)&gNtrIoRegs[0x2a8];
}

u32 NtrCp_SqrtResultAddr(void)
{
    u64 v = (NTR_IO16(0x2b0) & 1) ? *(u64 *)&gNtrIoRegs[0x2b8] : *(u32 *)&gNtrIoRegs[0x2b8];
    u64 r = 0, bit = (u64)1 << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    NTR_IO32(0x2b4) = (u32)r;
    return (u32)&gNtrIoRegs[0x2b4];
}

/* OS ------------------------------------------------------------------------ */

void *OS_AllocFromHeap(OSArenaId id, OSHeapHandle heap, u32 size)
{
    (void)id;
    (void)heap;
    return malloc(size);
}

void OS_FreeToHeap(OSArenaId id, OSHeapHandle heap, void *ptr)
{
    (void)id;
    (void)heap;
    free(ptr);
}

/* SDK_FINALROM panics (OS_Panic and friends) end up here. */
void OS_Terminate(void)
{
    fprintf(stderr, "OS_Terminate: the game stopped on a fatal error\n");
    abort();
}

/* FS --------------------------------------------------------------------- */

static char sRoot[256] = "romfs";

void NtrFS_SetRoot(const char *root)
{
    snprintf(sRoot, sizeof(sRoot), "%s", root);
}

size_t Ntr_ReadPieces(void *dst, size_t len, FILE *f)
{
    size_t done = 0;
    while (done < len) {
        size_t n = len - done < NTR_FILE_PIECE ? len - done : NTR_FILE_PIECE;
        size_t got = fread((u8 *)dst + done, 1, n, f);
        done += got;
        if (got != n) {
            break;
        }
    }
    return done;
}

size_t Ntr_WritePieces(const void *src, size_t len, FILE *f)
{
    size_t done = 0;
    while (done < len) {
        size_t n = len - done < NTR_FILE_PIECE ? len - done : NTR_FILE_PIECE;
        size_t put = fwrite((const u8 *)src + done, 1, n, f);
        done += put;
        if (put != n) {
            break;
        }
    }
    return done;
}

void FS_Init(u32 default_dma_no)
{
    (void)default_dma_no;
}

void FS_InitFile(FSFile *file)
{
    file->handle = NULL;
    file->size = 0;
    file->pos = 0;
    file->block = NULL;
}

/* Files of the extracted ROM stay open once opened, and reads go through a
 * cache of FS_BLOCK_SIZE blocks (least recently used ones are dropped): the
 * game opens its archives and reads small pieces of them all the time, which
 * on the 3DS would each be a request to the SD card. Blocks are filled
 * FS_PIECE_SIZE at a time, as the game reads them: in the Azahar emulator a
 * request of up to 8KB takes about a millisecond, a bigger one at least a
 * frame (it completes at the next frame). Reads of FS_DIRECT_MIN and more go
 * straight to the destination, as one request (about 10MB/s). */
#define FS_BLOCK_SIZE 0x4000 /* small, so scattered reads leave little of a block unused */
#define FS_PIECE_SIZE 0x2000
#define FS_DIRECT_MIN 0x18000
#define FS_MAX_FILES 400

typedef struct FsEntry {
    char path[128];
    FILE *f; /* opened when the game first opens the file */
    u32 size;
} FsEntry;

typedef struct FsBlock {
    int entry; /* -1: free */
    u32 index;
    u32 length;
    u32 lastUse;
    u32 pieces; /* the pieces read in so far, a bit each */
    int next; /* the next block in its hash bucket, -1 at the end */
    u8 *data;
} FsBlock;

static FsEntry sFiles[FS_MAX_FILES];
static int sNumFiles;
static FsBlock *sBlocks;
static int sNumBlocks, sBlocksUsed;
static int *sBuckets; /* the first block of each hash bucket, by (file, block index) */
static u32 sBucketMask;
static u32 sFsClock;
static u32 sCacheBytes = 16u << 20;

/* The cache is shared with a prefetching thread when the frontend runs one
 * (NtrFS_Prefetch): these then lock it. */
static void (*sFsLock)(void), (*sFsUnlock)(void);

void NtrFS_SetLock(void (*lock)(void), void (*unlock)(void))
{
    sFsLock = lock;
    sFsUnlock = unlock;
}

static inline void Lock(void)
{
    if (sFsLock != NULL) {
        sFsLock();
    }
}

static inline void Unlock(void)
{
    if (sFsUnlock != NULL) {
        sFsUnlock();
    }
}
u32 gNtrFsHostReads, gNtrFsHostBytes;
/* Problems, for diagnosis over the emulator's RPC: files the game could not
 * open, reads that came back short, the last errno and path of either, and
 * how many files the game has open. */
struct {
    u32 openFailures, shortReads, lastErrno, openFiles;
    char lastPath[128];
} gNtrFsTrouble;

static void NoteTrouble(u32 *counter, const char *path)
{
    (*counter)++;
    gNtrFsTrouble.lastErrno = (u32)errno;
    snprintf(gNtrFsTrouble.lastPath, sizeof(gNtrFsTrouble.lastPath), "%s", path);
}
u32 gNtrFsGameOpens, gNtrFsGameReads; /* what the game asked for */

void NtrFS_SetCacheSize(u32 bytes)
{
    if (sBlocks == NULL) {
        sCacheBytes = bytes;
    }
}

/* The blocks' memory, in one piece taken at the start: allocated block by
 * block, interleaved with the game's allocations, it fragmented the heap
 * until a big allocation of the game's failed. */
static u8 *sBlockMemory;

static void InitCache(void)
{
    if (sBlocks != NULL) {
        return;
    }
    sNumBlocks = (int)(sCacheBytes / FS_BLOCK_SIZE);
    sNumBlocks = sNumBlocks < 4 ? 4 : sNumBlocks;
    while ((sBlockMemory = malloc((size_t)sNumBlocks * FS_BLOCK_SIZE)) == NULL && sNumBlocks > 4) {
        sNumBlocks /= 2;
    }
    sBlocks = calloc((size_t)sNumBlocks, sizeof(FsBlock));
    u32 buckets = 1;
    while (buckets < 2u * (u32)sNumBlocks) {
        buckets <<= 1;
    }
    sBuckets = malloc(buckets * sizeof(int));
    sBucketMask = buckets - 1;
    for (u32 i = 0; i < buckets; i++) {
        sBuckets[i] = -1;
    }
    for (int b = 0; b < sNumBlocks; b++) {
        sBlocks[b].entry = -1;
        sBlocks[b].next = -1;
        sBlocks[b].data = sBlockMemory != NULL ? sBlockMemory + (size_t)b * FS_BLOCK_SIZE : NULL;
    }
}

static inline u32 BucketOf(int entry, u32 index)
{
    return ((u32)entry * 0x9e3779b1u ^ index * 0x85ebca77u) & sBucketMask;
}

static FsBlock *FindBlock(int entry, u32 index)
{
    for (int b = sBuckets[BucketOf(entry, index)]; b >= 0; b = sBlocks[b].next) {
        if (sBlocks[b].entry == entry && sBlocks[b].index == index) {
            return &sBlocks[b];
        }
    }
    return NULL;
}

/* Gives a block a new key (entry -1: free), moving it between buckets. */
static void Rekey(FsBlock *k, int entry, u32 index)
{
    int self = (int)(k - sBlocks);
    if (k->entry >= 0) {
        int *link = &sBuckets[BucketOf(k->entry, k->index)];
        while (*link != self) {
            link = &sBlocks[*link].next;
        }
        *link = k->next;
        sBlocksUsed--;
    }
    k->entry = entry;
    k->index = index;
    k->pieces = 0;
    k->next = -1;
    if (entry >= 0) {
        u32 h = BucketOf(entry, index);
        k->next = sBuckets[h];
        sBuckets[h] = self;
        k->length = sFiles[entry].size - index * FS_BLOCK_SIZE < FS_BLOCK_SIZE ? sFiles[entry].size - index * FS_BLOCK_SIZE : FS_BLOCK_SIZE;
        sBlocksUsed++;
    }
}

static int OpenEntry(const char *path, BOOL forGame);

BOOL FS_OpenFile(FSFile *file, const char *path)
{
    Lock();
    int i = OpenEntry(path, TRUE);
    Unlock();
    if (i < 0) {
        return FALSE;
    }
    return FS_OpenFileFast(file, (FSFileID){ NULL, (u32)i });
}

BOOL FS_ConvertPathToFileID(FSFileID *id, const char *path)
{
    Lock();
    int i = OpenEntry(path, TRUE);
    Unlock();
    if (i < 0) {
        return FALSE;
    }
    id->arc = NULL;
    id->file_id = (u32)i;
    return TRUE;
}

/* The table entry of a ROM file, made the first time. The game's entries
 * keep the file open (the prefetching thread opens its own). */
static int OpenEntry(const char *path, BOOL forGame)
{
    gNtrFsGameOpens += forGame;
    while (*path == '/') {
        path++;
    }
    int i;
    for (i = 0; i < sNumFiles && strcmp(sFiles[i].path, path) != 0; i++) {
    }
    if (i < sNumFiles && (sFiles[i].f != NULL || !forGame)) {
        return i;
    }
    char full[512];
    snprintf(full, sizeof(full), "%s/%s", sRoot, path);
    FILE *f = fopen(full, "rb");
    if (f == NULL || (i == sNumFiles && sNumFiles == FS_MAX_FILES) || strlen(path) >= sizeof(sFiles[0].path)) {
        if (forGame) {
            fprintf(stderr, "FS_OpenFile: cannot open %s\n", full);
            NoteTrouble(&gNtrFsTrouble.openFailures, full);
        }
        if (f != NULL) {
            fclose(f);
        }
        return -1;
    }
    setvbuf(f, NULL, _IONBF, 0); /* reads are whole pieces */
    fseek(f, 0, SEEK_END);
    u32 size = (u32)ftell(f);
    if (!forGame) {
        fclose(f);
        f = NULL;
    } else {
        gNtrFsTrouble.openFiles++;
    }
    FsEntry *e = &sFiles[i];
    if (i == sNumFiles) {
        sNumFiles++;
        snprintf(e->path, sizeof(e->path), "%s", path);
    }
    e->f = f;
    e->size = size;
    return i;
}

BOOL FS_OpenFileFast(FSFile *file, FSFileID id)
{
    if (id.file_id >= (u32)sNumFiles) {
        return FALSE;
    }
    int i = (int)id.file_id;
    Lock();
    InitCache();
    if (sFiles[i].f == NULL) { /* an entry the prefetcher made: open it for the game */
        char path[sizeof(sFiles[0].path)];
        snprintf(path, sizeof(path), "%s", sFiles[i].path);
        i = OpenEntry(path, TRUE);
    }
    Unlock();
    if (i < 0) {
        return FALSE;
    }
    file->handle = &sFiles[i];
    file->size = sFiles[i].size;
    file->pos = 0;
    file->block = NULL;
    return TRUE;
}

BOOL FS_CloseFile(FSFile *file)
{
    file->handle = NULL; /* the file stays open for the next time */
    file->block = NULL;
    return TRUE;
}

BOOL FS_SeekFile(FSFile *file, s32 offset, FSSeekFileMode origin)
{
    if (file->handle == NULL) {
        return FALSE;
    }
    s64 base = origin == FS_SEEK_SET ? 0 : origin == FS_SEEK_CUR ? (s64)file->pos : (s64)file->size;
    s64 pos = base + offset;
    if (pos < 0 || pos > (s64)file->size) {
        return FALSE;
    }
    file->pos = (u32)pos;
    return TRUE;
}

/* Block `index` of a file, from the cache or else made empty (the least
 * recently used block gives way). */
static FsBlock *GetBlock(FsEntry *e, u32 index, FsBlock *hint)
{
    int entry = (int)(e - sFiles);
    FsBlock *k = hint != NULL && hint->entry == entry && hint->index == index ? hint : FindBlock(entry, index);
    if (k != NULL) {
        k->lastUse = ++sFsClock;
        return k;
    }
    FsBlock *victim = NULL;
    for (int b = 0; b < sNumBlocks; b++) {
        k = &sBlocks[b];
        if (victim == NULL || k->entry < 0 || (victim->entry >= 0 && k->lastUse < victim->lastUse)) {
            victim = k;
            if (k->entry < 0) {
                break;
            }
        }
    }
    if (victim->data == NULL) {
        victim->data = malloc(FS_BLOCK_SIZE);
        if (victim->data == NULL) {
            return NULL;
        }
    }
    Rekey(victim, entry, index);
    victim->lastUse = ++sFsClock;
    return victim;
}

/* Reads in the pieces of a block that bytes [from, to) of it lie in. */
static BOOL FillBlock(FsEntry *e, FsBlock *b, u32 from, u32 to)
{
    for (u32 p = from / FS_PIECE_SIZE; p * FS_PIECE_SIZE < to; p++) {
        if (b->pieces & (1u << p)) {
            continue;
        }
        u32 at = p * FS_PIECE_SIZE;
        u32 length = b->length - at < FS_PIECE_SIZE ? b->length - at : FS_PIECE_SIZE;
        fseek(e->f, (long)(b->index * FS_BLOCK_SIZE + at), SEEK_SET);
        size_t got = fread(b->data + at, 1, length, e->f);
#ifdef NTR_DEBUG_HOOKS
        if (getenv("PORT_FSLOG")) {
            printf("fs read %s block %u piece %u (%u bytes)\n", e->path, b->index, p, length);
        }
#endif
        gNtrFsHostReads++;
        gNtrFsHostBytes += (u32)got;
        if (got != length) {
            NoteTrouble(&gNtrFsTrouble.shortReads, e->path);
            return FALSE;
        }
        b->pieces |= 1u << p;
    }
    return TRUE;
}

static s32 ReadFile(FSFile *file, void *dest, s32 size);

s32 FS_ReadFile(FSFile *file, void *dest, s32 size)
{
    u64 t0 = NtrHost_Ticks();
    Lock();
    s32 n = ReadFile(file, dest, size);
    Unlock();
    gNtrFsTicks += NtrHost_Ticks() - t0;
    return n;
}

static s32 ReadFile(FSFile *file, void *dest, s32 size)
{
    gNtrFsGameReads++;
    FsEntry *e = file->handle;
    if (e == NULL || size < 0) {
        return -1;
    }
    u32 left = file->size - file->pos < (u32)size ? file->size - file->pos : (u32)size;
    u8 *out = dest;
    s32 done = 0;
    while (left > 0) {
        u32 index = file->pos / FS_BLOCK_SIZE, at = file->pos % FS_BLOCK_SIZE;
        /* The stretch ahead with none of its blocks cached: read straight to
         * the destination if it is long. */
        u32 n = 0;
        int entry = (int)(e - sFiles);
        while (n < left && n < FS_DIRECT_MIN + FS_BLOCK_SIZE && FindBlock(entry, (file->pos + n) / FS_BLOCK_SIZE) == NULL) {
            n += FS_BLOCK_SIZE - (file->pos + n) % FS_BLOCK_SIZE;
        }
        if (n >= FS_DIRECT_MIN) {
            while (n < left && FindBlock(entry, (file->pos + n) / FS_BLOCK_SIZE) == NULL) {
                n += FS_BLOCK_SIZE;
            }
            n = n < left ? n : left;
            fseek(e->f, (long)file->pos, SEEK_SET);
            size_t got = Ntr_ReadPieces(out, n, e->f);
#ifdef NTR_DEBUG_HOOKS
            if (getenv("PORT_FSLOG")) {
                printf("fs read %s at %u (%u bytes, direct)\n", e->path, file->pos, n);
            }
#endif
            gNtrFsHostReads++;
            gNtrFsHostBytes += (u32)got;
            file->pos += (u32)got, out += got, done += (s32)got, left -= (u32)got;
            if (got != n) {
                NoteTrouble(&gNtrFsTrouble.shortReads, e->path);
                break;
            }
            continue;
        }
        FsBlock *b = GetBlock(e, index, file->block);
        if (b == NULL) {
            break;
        }
        file->block = b;
        n = b->length - at < left ? b->length - at : left;
        if (!FillBlock(e, b, at, at + n)) {
            Rekey(b, -1, 0);
            break;
        }
        memcpy(out, b->data + at, n);
        file->pos += n, out += n, done += (s32)n, left -= n;
    }
    return done;
}

/* A free cache block for the prefetcher, which never pushes out a block in
 * use; NULL if there is none. */
static FsBlock *FreeBlock(int entry, u32 index)
{
    static int next; /* blocks before it were all in use when last looked at */
    for (int b = next; b < sNumBlocks; b++) {
        FsBlock *k = &sBlocks[b];
        next = b;
        if (k->entry >= 0) {
            continue;
        }
        if (k->data == NULL && (k->data = malloc(FS_BLOCK_SIZE)) == NULL) {
            return NULL;
        }
        Rekey(k, entry, index);
        k->lastUse = ++sFsClock;
        return k;
    }
    return NULL;
}

u32 gNtrFsPrefetched; /* bytes the prefetcher read */

void NtrFS_Prefetch(const char *const *paths, int count, u32 budget)
{
    static u8 piece[FS_PIECE_SIZE];
    Lock();
    InitCache();
    Unlock();
    for (int i = 0; i < count; i++) {
        Lock();
        int entry = OpenEntry(paths[i], FALSE);
        u32 size = entry >= 0 ? sFiles[entry].size : 0;
        Unlock();
        if (entry < 0) {
            continue;
        }
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", sRoot, sFiles[entry].path);
        FILE *f = fopen(full, "rb");
        if (f == NULL) {
            continue;
        }
        setvbuf(f, NULL, _IONBF, 0);
        for (u32 at = 0; at < size; at += FS_PIECE_SIZE) {
            u32 index = at / FS_BLOCK_SIZE, bit = 1u << ((at % FS_BLOCK_SIZE) / FS_PIECE_SIZE);
            Lock();
            FsBlock *b = FindBlock(entry, index);
            BOOL have = b != NULL && (b->pieces & bit);
            BOOL full = b == NULL && (u32)sBlocksUsed * FS_BLOCK_SIZE >= budget;
            Unlock();
            if (full) {
                fclose(f);
                return;
            }
            if (have) {
                continue;
            }
            u32 length = size - at < FS_PIECE_SIZE ? size - at : FS_PIECE_SIZE;
            fseek(f, (long)at, SEEK_SET);
            if (fread(piece, 1, length, f) != length) {
                break;
            }
            Lock();
            b = FindBlock(entry, index);
            if (b == NULL) {
                b = FreeBlock(entry, index);
            }
            if (b != NULL && !(b->pieces & bit)) {
                memcpy(b->data + at % FS_BLOCK_SIZE, piece, length);
                b->pieces |= bit;
                gNtrFsPrefetched += length;
            }
            Unlock();
            if (b == NULL) {
                fclose(f);
                return;
            }
        }
        fclose(f);
    }
}

u32 FS_GetLength(FSFile *file) { return file->size; }
u32 FS_GetPosition(FSFile *file) { return file->pos; }

/* Cache maintenance: host memory is coherent, nothing to do. */
void DC_FlushRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_FlushAll(void) {}
void DC_StoreRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_StoreAll(void) {}
void DC_InvalidateRange(void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_InvalidateAll(void) {}
void DC_FlushRangeAll(void) {}
void IC_InvalidateRange(void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void IC_InvalidateAll(void) {}
