/*
 * Port of boot.c. The ROM header checks have nothing to check (the file
 * system is read from extracted files), and rebooting into another program
 * (the GBA migration's download play) is not supported.
 */
#include "boot.h"

#include <nitro.h>

void CheckForMemoryTampering(void) {}

void RebootAndLoadROM(const char *filesystemPath)
{
    (void)filesystemPath;
}
