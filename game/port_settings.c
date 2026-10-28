/*
 * The port's own options (port_settings.h), as "name=value" lines.
 */
#include "port_settings.h"

#include <stdio.h>
#include <string.h>

#include "constants/charcode.h"
#include "string_gf.h"

static const char *const sNames[PORT_SETTING_COUNT] = {
    "3d_edges",
};

static int sValues[PORT_SETTING_COUNT];
static char sPath[256];

void Port_SetSettingsFile(const char *path)
{
    snprintf(sPath, sizeof(sPath), "%s", path);
    FILE *f = fopen(sPath, "r");
    if (f == NULL) {
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        for (int i = 0; i < PORT_SETTING_COUNT; i++) {
            if (strcmp(line, sNames[i]) == 0) {
                sscanf(eq + 1, "%d", &sValues[i]);
            }
        }
    }
    fclose(f);
}

int Port_SettingByName(const char *name)
{
    for (int i = 0; i < PORT_SETTING_COUNT; i++) {
        if (strcmp(name, sNames[i]) == 0) {
            return i;
        }
    }
    return -1;
}

int Port_GetSetting(int setting)
{
    return sValues[setting];
}

void Port_SetSetting(int setting, int value)
{
    if (sValues[setting] == value) {
        return;
    }
    sValues[setting] = value;
    if (sPath[0] == '\0') {
        return;
    }
    FILE *f = fopen(sPath, "w");
    if (f == NULL) {
        return;
    }
    for (int i = 0; i < PORT_SETTING_COUNT; i++) {
        fprintf(f, "%s=%d\n", sNames[i], sValues[i]);
    }
    fclose(f);
}

static charcode_t CharCodeOf(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return CHAR_A + (c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return CHAR_a + (c - 'a');
    }
    if (c >= '0' && c <= '9') {
        return CHAR_0 + (c - '0');
    }
    switch (c) {
    case '\n':
        return CHAR_CR;
    case '.':
        return CHAR_PERIOD;
    case ',':
        return CHAR_COMMA;
    case '!':
        return 0x01AB;
    case '?':
        return 0x01AC;
    case '/':
        return 0x01B1;
    case '\'':
        return 0x01B3;
    case '-':
        return 0x01BE;
    case ':':
        return 0x01C4;
    default:
        return CHAR_SPACE;
    }
}

String *Port_NewString(const char *ascii, u32 heapID)
{
    size_t len = strlen(ascii);
    String *string = String_Init((u32)len + 1, heapID);
    for (size_t i = 0; i < len; i++) {
        String_AppendChar(string, CharCodeOf(ascii[i]));
    }
    return string;
}
