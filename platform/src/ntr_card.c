/*
 * The game card's save memory: a 512KB (4 Mbit) flash chip, kept in memory
 * and written to a host file. Requests finish immediately; their completion
 * callbacks run before the request returns. With a writer thread
 * (Ntr_SetBackupWriter) the file is written behind the game, in one go for
 * all the requests of a save; otherwise each request writes through.
 */
#include <stdio.h>
#include <string.h>

#include "ntr_shim.h"

#define BACKUP_SIZE 0x80000

static u8 sBackup[BACKUP_SIZE];
static char sBackupPath[512];
static BOOL sLoaded;
static CARDResult sResult = CARD_RESULT_SUCCESS;

void Ntr_SetBackupFile(const char *path)
{
    snprintf(sBackupPath, sizeof(sBackupPath), "%s", path);
    sLoaded = FALSE;
}

static void Load(void)
{
    if (sLoaded) {
        return;
    }
    sLoaded = TRUE;
    memset(sBackup, 0xff, sizeof(sBackup)); /* erased flash */
    FILE *f = sBackupPath[0] ? fopen(sBackupPath, "rb") : NULL;
    if (f != NULL) {
        size_t n = Ntr_ReadPieces(sBackup, sizeof(sBackup), f);
        (void)n;
        fclose(f);
    }
}

static void (*sWriterWake)(void), (*sWriterLock)(void), (*sWriterUnlock)(void);
static u32 sDirtyLo = BACKUP_SIZE, sDirtyHi; /* changed since the writer last wrote */

void Ntr_SetBackupWriter(void (*wake)(void), void (*lock)(void), void (*unlock)(void))
{
    sWriterWake = wake;
    sWriterLock = lock;
    sWriterUnlock = unlock;
}

static void WriterLock(void)
{
    if (sWriterLock != NULL) {
        sWriterLock();
    }
}

static void WriterUnlock(void)
{
    if (sWriterUnlock != NULL) {
        sWriterUnlock();
    }
}

static void WriteFile(const u8 *data, u32 offset, u32 len)
{
    FILE *f = fopen(sBackupPath, "r+b");
    if (f == NULL) {
        f = fopen(sBackupPath, "wb");
        if (f == NULL) {
            return;
        }
        Ntr_WritePieces(data, BACKUP_SIZE, f);
    } else {
        fseek(f, (long)offset, SEEK_SET);
        Ntr_WritePieces(data + offset, len, f);
    }
    fclose(f);
}

/* (with the writer lock held) */
static void Store(u32 offset, u32 len)
{
    if (!sBackupPath[0]) {
        return;
    }
    if (sWriterWake == NULL) {
        WriteFile(sBackup, offset, len);
        return;
    }
    sDirtyLo = offset < sDirtyLo ? offset : sDirtyLo;
    sDirtyHi = offset + len > sDirtyHi ? offset + len : sDirtyHi;
}

BOOL Ntr_BackupWriteChanges(void)
{
    static u8 copy[BACKUP_SIZE];
    WriterLock();
    u32 lo = sDirtyLo, hi = sDirtyHi;
    if (hi <= lo) {
        WriterUnlock();
        return FALSE;
    }
    memcpy(copy, sBackup, BACKUP_SIZE); /* (the whole image, in case the file is new) */
    sDirtyLo = BACKUP_SIZE;
    sDirtyHi = 0;
    WriterUnlock();
    WriteFile(copy, lo, hi - lo);
    return TRUE;
}

static BOOL InRange(u32 offset, u32 len)
{
    return offset <= BACKUP_SIZE && len <= BACKUP_SIZE - offset;
}

BOOL CARDi_RequestStreamCommand(u32 src, u32 dst, u32 len, MIDmaCallback callback, void *arg, BOOL is_async,
                                CARDRequest req_type, int req_retry, CARDRequestMode req_mode)
{
    (void)is_async;
    (void)req_retry;
    (void)req_mode;
    WriterLock();
    Load();
    sResult = CARD_RESULT_SUCCESS;

    switch (req_type) {
    case CARD_REQ_READ_BACKUP:
        if (!InRange(src, len)) {
            sResult = CARD_RESULT_INVALID_PARAM;
            break;
        }
        memcpy((void *)dst, sBackup + src, len);
        break;
    case CARD_REQ_WRITE_BACKUP:
    case CARD_REQ_PROGRAM_BACKUP:
        if (!InRange(dst, len)) {
            sResult = CARD_RESULT_INVALID_PARAM;
            break;
        }
        memcpy(sBackup + dst, (const void *)src, len);
        Store(dst, len);
        break;
    case CARD_REQ_VERIFY_BACKUP:
        if (!InRange(dst, len) || memcmp(sBackup + dst, (const void *)src, len) != 0) {
            sResult = CARD_RESULT_FAILURE;
        }
        break;
    case CARD_REQ_ERASE_PAGE_BACKUP:
    case CARD_REQ_ERASE_SECTOR_BACKUP:
    case CARD_REQ_ERASE_SUBSECTOR_BACKUP:
        if (!InRange(dst, len)) {
            sResult = CARD_RESULT_INVALID_PARAM;
            break;
        }
        memset(sBackup + dst, 0xff, len);
        Store(dst, len);
        break;
    default:
        sResult = CARD_RESULT_UNSUPPORTED;
        break;
    }
    WriterUnlock();
    if (sWriterWake != NULL && sDirtyHi > sDirtyLo) {
        sWriterWake();
    }

    if (callback != NULL) {
        callback(arg);
    }
    return sResult == CARD_RESULT_SUCCESS;
}

BOOL CARD_IdentifyBackup(CARDBackupType type)
{
    return type == CARD_BACKUP_TYPE_FLASH_4MBITS;
}

void CARD_Init(void) {}
void CARD_LockBackup(u16 lock_id) { (void)lock_id; }
void CARD_UnlockBackup(u16 lock_id) { (void)lock_id; }
CARDResult CARD_GetResultCode(void) { return sResult; }
BOOL CARD_WaitBackupAsync(void) { return sResult == CARD_RESULT_SUCCESS; }
BOOL CARD_TryWaitBackupAsync(void) { return TRUE; }
void CARD_CancelBackupAsync(void) {}
BOOL CARD_IsPulledOut(void) { return FALSE; }
void CARD_SetCacheFlushThreshold(u32 icache, u32 dcache) { (void)icache; (void)dcache; }
