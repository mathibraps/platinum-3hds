/*
 * Options of the 3DS port itself (not in the DS game), shown in the game's
 * OPTIONS menu and kept in a text file of their own beside the save data, so
 * saves stay as the DS game writes them.
 */
#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

#include <nitro.h>

enum PortSetting {
    PORT_SETTING_3D_EDGES, /* 0: as on the DS, 1: smooth (3D drawn at twice the size) */
    PORT_SETTING_COUNT,
};

/* The settings file; read now, written when a setting changes. */
void Port_SetSettingsFile(const char *path);

int Port_GetSetting(int setting);
void Port_SetSetting(int setting, int value);
/* The setting a settings file calls `name` ("3d_edges"), or -1. */
int Port_SettingByName(const char *name);

/* A game string from plain ASCII text (letters, digits, space, newline and
 * the punctuation .,!?-:/'), for the port's own menu text. */
struct String;
struct String *Port_NewString(const char *ascii, u32 heapID);

#endif
