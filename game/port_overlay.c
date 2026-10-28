/* Port of game_overlay.c: all code is linked statically, so overlays are no-ops. */
#include "game_overlay.h"

BOOL Overlay_LoadByID(const FSOverlayID overlayID, enum OverlayLoadType loadType)
{
    (void)overlayID;
    (void)loadType;
    return TRUE;
}

void Overlay_UnloadByID(const FSOverlayID overlayID)
{
    (void)overlayID;
}

int Overlay_GetLoadDestination(const FSOverlayID overlayID)
{
    (void)overlayID;
    return 0;
}
