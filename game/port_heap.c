/*
 * Heap API for the port. The DS game carves fixed-size heaps out of 4MB of
 * main RAM; here every heap is backed by the system allocator. Blocks carry a
 * small header recording their heap, which Heap_Free checks.
 *
 * Freed memory is reused only by the same heap, as on the DS, and keeps its
 * contents until then: the game reads some of it after freeing it, which on
 * the DS is harmless. (Crossing from Twinleaf Town to Route 201, the map's
 * sprites free an animation sequence that a sprite still draws with for a
 * few frames; on the DS the sequences have a heap of their own, so their
 * memory is never given to anything but another sequence.) Each heap keeps
 * up to POOL_MAX of freed memory, all heaps together up to POOLS_MAX; the
 * rest goes back to the system.
 *
 * For finding reads of freed memory, freed blocks can be held back for a
 * while, filled with 0xdd (PortHeap_SetQuarantine; the test script line
 * "heap quarantine <KB>", or PORT_HEAP_POISON in debug builds, which never
 * frees): the game then reads the filler there.
 */
#include <stdlib.h>
#include <string.h>

#include "heap.h"

#define HEAP_MAGIC 0x48454150u /* 'HEAP' */

typedef union BlockHeader {
    struct {
        u32 magic;
        u32 heapID;
        u32 size; /* what the block holds (reused, it may be given out for less) */
        union BlockHeader *next; /* freed: the next in its heap's list */
    };
    max_align_t align;
} BlockHeader;

static BlockHeader *HeaderOf(void *ptr)
{
    BlockHeader *h = (BlockHeader *)ptr - 1;
    return h->magic == HEAP_MAGIC ? h : NULL;
}

/* A heap's freed blocks, in lists by size (list b: sizes from 2^b up to
 * 2^(b+1) - 1). */
#define HEAP_IDS 256
#define BINS 32
#define POOL_MAX (4u << 20)
#define POOLS_MAX (12u << 20)

typedef struct Pool {
    BlockHeader *bin[BINS];
    u32 bytes;
} Pool;

static Pool sPools[HEAP_IDS];
u32 gPortHeapPooled; /* bytes in all of them */

static int BinOf(u32 size)
{
    return size != 0 ? 31 - __builtin_clz(size) : 0;
}

/* A freed block of the heap that holds `size` bytes: from the list of its
 * size, or else any from the next list (at most four times as big). */
static BlockHeader *TakeFreed(u32 heapID, u32 size)
{
    Pool *pool = &sPools[heapID % HEAP_IDS];
    int b = BinOf(size);
    BlockHeader **link = &pool->bin[b];
    for (int tries = 0; *link != NULL && (*link)->size < size && tries < 32; tries++) {
        link = &(*link)->next;
    }
    if ((*link == NULL || (*link)->size < size) && b + 1 < BINS) {
        link = &pool->bin[b + 1];
    }
    BlockHeader *h = *link;
    if (h == NULL || h->size < size) {
        return NULL;
    }
    *link = h->next;
    pool->bytes -= h->size;
    gPortHeapPooled -= h->size;
    return h;
}

/* Gives a heap's freed blocks back to the system. */
static void ReleaseFreed(Pool *pool)
{
    for (int b = 0; b < BINS; b++) {
        while (pool->bin[b] != NULL) {
            BlockHeader *h = pool->bin[b];
            pool->bin[b] = h->next;
            gPortHeapPooled -= h->size;
            free(h);
        }
    }
    pool->bytes = 0;
}

void Heap_InitSystem(const HeapParam *templates, u32 nTemplates, u32 totalNumHeaps, u32 preSize)
{
    (void)templates;
    (void)nTemplates;
    (void)totalNumHeaps;
    (void)preSize;
}

BOOL Heap_Create(enum HeapID parent, enum HeapID child, u32 size)
{
    (void)parent;
    (void)child;
    (void)size;
    return TRUE;
}

BOOL Heap_CreateAtEnd(enum HeapID parent, enum HeapID child, u32 size)
{
    return Heap_Create(parent, child, size);
}

void Heap_Destroy(enum HeapID heapID)
{
    ReleaseFreed(&sPools[heapID % HEAP_IDS]);
}

u32 gPortHeapFailures; /* allocations that found no memory (the game does not expect any) */

void *Heap_Alloc(u32 heapID, u32 size)
{
    BlockHeader *h = TakeFreed(heapID, size);
    if (h == NULL) {
        h = malloc(sizeof(BlockHeader) + size);
        if (h == NULL) { /* the freed memory of all heaps back to the system, and again */
            for (int i = 0; i < HEAP_IDS; i++) {
                ReleaseFreed(&sPools[i]);
            }
            h = malloc(sizeof(BlockHeader) + size);
        }
        if (h == NULL) {
            gPortHeapFailures++;
            return NULL;
        }
        h->size = size;
    }
    h->magic = HEAP_MAGIC;
    h->heapID = heapID;
    h->next = NULL;
    return h + 1;
}

void *Heap_AllocAtEnd(u32 heapID, u32 size)
{
    return Heap_Alloc(heapID, size);
}

/* Freed blocks held back, oldest first, and their bytes; ~0: never freed. */
#define QUARANTINE_BLOCKS 8192
static BlockHeader *sQuarantine[QUARANTINE_BLOCKS];
static u32 sQuarantineFirst, sQuarantineCount, sQuarantineBytes, sQuarantineMax;

void PortHeap_SetQuarantine(u32 bytes)
{
    sQuarantineMax = bytes;
}

static BOOL Quarantine(BlockHeader *h)
{
#ifdef NTR_DEBUG_HOOKS
    static int poison = -1;
    if (poison < 0) {
        poison = getenv("PORT_HEAP_POISON") != NULL;
        sQuarantineMax = poison ? ~0u : sQuarantineMax;
    }
#endif
    if (sQuarantineMax == 0) {
        return FALSE;
    }
    memset(h + 1, 0xdd, h->size);
    if (sQuarantineMax == ~0u) {
        return TRUE;
    }
    while (sQuarantineCount > 0 && (sQuarantineCount == QUARANTINE_BLOCKS || sQuarantineBytes + h->size > sQuarantineMax)) {
        BlockHeader *old = sQuarantine[sQuarantineFirst];
        sQuarantineFirst = (sQuarantineFirst + 1) % QUARANTINE_BLOCKS;
        sQuarantineCount--;
        sQuarantineBytes -= old->size;
        free(old);
    }
    sQuarantine[(sQuarantineFirst + sQuarantineCount++) % QUARANTINE_BLOCKS] = h;
    sQuarantineBytes += h->size;
    return TRUE;
}

void Heap_Free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    BlockHeader *h = HeaderOf(ptr);
    if (h == NULL) {
        return;
    }
    h->magic = 0;
    if (Quarantine(h)) {
        return;
    }
    Pool *pool = &sPools[h->heapID % HEAP_IDS];
    if (pool->bytes + h->size > POOL_MAX || gPortHeapPooled + h->size > POOLS_MAX) {
        free(h);
        return;
    }
    int b = BinOf(h->size);
    h->next = pool->bin[b];
    pool->bin[b] = h;
    pool->bytes += h->size;
    gPortHeapPooled += h->size;
}

void Heap_FreeExplicit(u32 heapID, void *ptr)
{
    (void)heapID;
    Heap_Free(ptr);
}

u32 HeapExp_FndGetTotalFreeSize(u32 heapID)
{
    (void)heapID;
    return 0x400000;
}

static void *AllocatorAlloc(NNSFndAllocator *allocator, u32 size)
{
    return Heap_Alloc(allocator->heapParam1, size);
}

static void AllocatorFree(NNSFndAllocator *allocator, void *mem)
{
    (void)allocator;
    Heap_Free(mem);
}

static const NNSFndAllocatorFunc sAllocatorFuncs = { AllocatorAlloc, AllocatorFree };

void HeapExp_FndInitAllocator(NNSFndAllocator *allocator, u32 heapID, int alignment)
{
    allocator->pFunc = &sAllocatorFuncs;
    allocator->pHeap = NULL;
    allocator->heapParam1 = heapID;
    allocator->heapParam2 = (u32)alignment;
}

void Heap_Realloc(void *ptr, u32 newSize)
{
    /* The game only ever shrinks blocks in place, so there is nothing to do. */
    (void)ptr;
    (void)newSize;
}

BOOL GF_heap_c_dummy_return_true(u32 heapID)
{
    (void)heapID;
    return TRUE;
}
