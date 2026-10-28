/* Read-only views of game state for the test harnesses. */
#include "system.h"

void Port_DebugTouchState(u16 *x, u16 *y, u16 *pressed, u16 *held, BOOL *autoSampling)
{
    *x = gSystem.touchX;
    *y = gSystem.touchY;
    *pressed = gSystem.touchPressed;
    *held = gSystem.touchHeld;
    *autoSampling = gSystem.touchAutoSampling;
}

#include "sys_task_manager.h"

/* Free task slots and the lock flag of the VBlank task manager. */
void Port_DebugVBlankTasks(int *freeSlots, int *locked)
{
    *freeSlots = gSystem.vBlankTaskMgr->stackPointer | (gSystem.vBlankTaskMgr->maxTasks << 16);
    *locked = gSystem.vBlankTaskMgr->locked;
}

#include "field/field_system.h"
#include "player_avatar.h"

FieldSystem *Port_DebugFieldSystem(void);

/* The player's map and tile, and the way they face; FALSE outside the field. */
BOOL Port_DebugPlayerPos(int *map, int *x, int *z, int *dir)
{
    FieldSystem *fieldSystem = Port_DebugFieldSystem();
    if (fieldSystem == NULL || fieldSystem->playerAvatar == NULL || fieldSystem->location == NULL) {
        return FALSE;
    }
    *map = fieldSystem->location->mapHeaderID;
    *x = PlayerAvatar_GetXPos(fieldSystem->playerAvatar);
    *z = PlayerAvatar_GetZPos(fieldSystem->playerAvatar);
    *dir = PlayerAvatar_GetFacingDir(fieldSystem->playerAvatar);
    return TRUE;
}
