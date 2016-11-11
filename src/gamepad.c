/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2016 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/*
  Copyright 2016 Michal Nowikowski. All rights reserved.
  Adjusted SDL_gamecontroller.c based on SDL2 for SDL1.
*/

// based on https://gist.github.com/meghprkh/9cdce0cd4e0f41ce93413b250a207a55
// and on http://www.linuxjournal.com/article/6429

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <unistd.h>
#include <endian.h>
#include <stdint.h>
#include <dirent.h>
#include <string.h>

#include <SDL.h>

#include "gamepad.h"


char* _concat(const char *s1, const char *s2)
{
    char *result = malloc(strlen(s1)+strlen(s2)+1);//+1 for the zero-terminator
    strcpy(result, s1);
    strcat(result, s2);
    return result;
}

#define test_bit(nr, addr) \
    (((1UL << ((nr) % (sizeof(long) * 8))) & ((addr)[(nr) / (sizeof(long) * 8)])) != 0)
#define NBITS(x) ((((x)-1)/(sizeof(long) * 8))+1)

static int
IsJoystick(int fd)
{
    struct input_id inpid;

#if !SDL_USE_LIBUDEV
    /* When udev is enabled we only get joystick devices here, so there's no need to test them */
    unsigned long evbit[NBITS(EV_MAX)] = { 0 };
    unsigned long keybit[NBITS(KEY_MAX)] = { 0 };
    unsigned long absbit[NBITS(ABS_MAX)] = { 0 };

    if ((ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) ||
        (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) ||
        (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) < 0)) {
        return (0);
    }

    if (!(test_bit(EV_KEY, evbit) && test_bit(EV_ABS, evbit) &&
          test_bit(ABS_X, absbit) && test_bit(ABS_Y, absbit))) {
        return 0;
    }
#endif

    if (ioctl(fd, EVIOCGID, &inpid) < 0) {
        return 0;
    }

#ifdef DEBUG_JOYSTICK
    printf("Joystick: %s, bustype = %d, vendor = 0x%x, product = 0x%x, version = %d\n", namebuf, inpid.bustype, inpid.vendor, inpid.product, inpid.version);
#endif

    return 1;
}


char *get_gamepad_dev_path(int index)
{
    DIR *d;
    struct dirent *dir;
    int cmp;
    char *suffix = "-event-joystick";
    size_t lenstr;
    size_t lensuffix = strlen(suffix);
    int cur_idx = 0;
    char n[4];
    int i;
    int fd = -1;

    for (i = 0; i < 32; i++) {
        sprintf(n, "%d", i);
        char *name = _concat("/dev/input/event", n);
        if (access(name, F_OK) < -1) {
            continue;
        }

        fd = open(name, O_RDONLY, 0);
        if (fd < 0) {
            continue;
        }

        if (IsJoystick(fd)) {
            return name;
        }
    }

    return NULL;
}


void _guid_to_string(uint16_t *guid, char *guidstr) {
    static const char k_rgchHexToASCII[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++) {
        unsigned char c = guid[i];

        *guidstr++ = k_rgchHexToASCII[c >> 4];
        *guidstr++ = k_rgchHexToASCII[c & 0x0F];

        c = guid[i] >> 8;
        *guidstr++ = k_rgchHexToASCII[c >> 4];
        *guidstr++ = k_rgchHexToASCII[c & 0x0F];
    }
    *guidstr = '\0';
}

int get_guid(char *dev_name, char *guidstr) {
    int fd = -1;
    char name[256]= "Unknown";
    struct input_id device_info;
    uint16_t guid[8];

    if ((fd = open(dev_name, O_RDONLY)) < 0) {
        perror("evdev open");
        return -1;
    }

    if (ioctl(fd, EVIOCGID, &device_info)) {
        perror("evdev ioctl");
        return -1;
    }
    close(fd);

#if DEBUG
    printf("bustype %04hx\nvendor %04hx\nproduct\n%04hx\nversion %04hx\n",
           device_info.bustype, device_info.vendor, device_info.product, device_info.version);
#endif

    guid[0] = htole16(device_info.bustype);
    guid[1] = 0;
    guid[2] = htole16(device_info.vendor);
    guid[3] = 0;
    guid[4] = htole16(device_info.product);
    guid[5] = 0;
    guid[6] = htole16(device_info.version);
    guid[7] = 0;

    _guid_to_string(guid, guidstr);

#if DEBUG
    printf("GUID %s\n", guidstr);
#endif

    return 0;
}



#define SDL_InvalidParamError(param)    SDL_SetError("Parameter '%s' is invalid", (param))

#define SDL_CONTROLLER_PLATFORM_FIELD "platform:"

#define ABS(_x) ((_x) < 0 ? -(_x) : (_x))

#define SDL_zero(x) SDL_memset(&(x), 0, sizeof((x)))


/* A structure that encodes the stable unique id for a joystick device */
typedef struct {
    Uint8 data[16];
} SDL_JoystickGUID;

/* keep track of the hat and mask value that transforms this hat movement into a button/axis press */
struct _SDL_HatMapping
{
    int hat;
    Uint8 mask;
};

#define k_nMaxReverseEntries 20

/**
 * We are encoding the "HAT" as 0xhm. where h == hat ID and m == mask
 * MAX 4 hats supported
 */
#define k_nMaxHatEntries 0x3f + 1

/* our in memory mapping db between joystick objects and controller mappings */
struct _SDL_ControllerMapping
{
    SDL_JoystickGUID guid;
    const char *name;

    /* mapping of axis/button id to controller version */
    int axes[SDL_CONTROLLER_AXIS_MAX];
    int buttonasaxis[SDL_CONTROLLER_AXIS_MAX];

    int buttons[SDL_CONTROLLER_BUTTON_MAX];
    int axesasbutton[SDL_CONTROLLER_BUTTON_MAX];
    struct _SDL_HatMapping hatasbutton[SDL_CONTROLLER_BUTTON_MAX];

    /* reverse mapping, joystick indices to buttons */
    SDL_GameControllerAxis raxes[k_nMaxReverseEntries];
    SDL_GameControllerAxis rbuttonasaxis[k_nMaxReverseEntries];

    SDL_GameControllerButton rbuttons[k_nMaxReverseEntries];
    SDL_GameControllerButton raxesasbutton[k_nMaxReverseEntries];
    SDL_GameControllerButton rhatasbutton[k_nMaxHatEntries];

};

/* our hard coded list of mapping support */
typedef struct _ControllerMapping_t
{
    SDL_JoystickGUID guid;
    char *name;
    char *mapping;
    struct _ControllerMapping_t *next;
} ControllerMapping_t;

static ControllerMapping_t *s_pSupportedControllers = NULL;
static ControllerMapping_t *s_pXInputMapping = NULL;
static ControllerMapping_t *s_pEmscriptenMapping = NULL;

/* The SDL game controller structure */
struct _SDL_GameController
{
    SDL_Joystick *joystick; /* underlying joystick device */
    int ref_count;
    Uint8 hatState[4]; /* the current hat state for this controller */
    struct _SDL_ControllerMapping mapping; /* the mapping object for this controller */
    struct _SDL_GameController *next; /* pointer to next game controller we have allocated */
};

static SDL_GameController *SDL_gamecontrollers = NULL;

static const char* map_StringForControllerButton[] = {
    "a",
    "b",
    "x",
    "y",
    "back",
    "guide",
    "start",
    "leftstick",
    "rightstick",
    "leftshoulder",
    "rightshoulder",
    "dpup",
    "dpdown",
    "dpleft",
    "dpright",
    NULL
};

static const char* map_StringForControllerAxis[] = {
    "leftx",
    "lefty",
    "rightx",
    "righty",
    "lefttrigger",
    "righttrigger",
    NULL
};


/*
 * grab the name string from a mapping string
 */
char *SDL_PrivateGetControllerNameFromMappingString(const char *pMapping)
{
    const char *pFirstComma, *pSecondComma;
    char *pchName;

    pFirstComma = SDL_strchr(pMapping, ',');
    if (!pFirstComma)
        return NULL;

    pSecondComma = SDL_strchr(pFirstComma + 1, ',');
    if (!pSecondComma)
        return NULL;

    pchName = SDL_malloc(pSecondComma - pFirstComma);
    if (!pchName) {
        SDL_OutOfMemory();
        return NULL;
    }
    SDL_memcpy(pchName, pFirstComma + 1, pSecondComma - pFirstComma);
    pchName[ pSecondComma - pFirstComma - 1 ] = 0;
    return pchName;
}

/*
 * grab the button mapping string from a mapping string
 */
char *SDL_PrivateGetControllerMappingFromMappingString(const char *pMapping)
{
    const char *pFirstComma, *pSecondComma;

    pFirstComma = SDL_strchr(pMapping, ',');
    if (!pFirstComma)
        return NULL;

    pSecondComma = SDL_strchr(pFirstComma + 1, ',');
    if (!pSecondComma)
        return NULL;

    return SDL_strdup(pSecondComma + 1); /* mapping is everything after the 3rd comma */
}

/*
 * Helper function to scan the mappings database for a controller with the specified GUID
 */
ControllerMapping_t *SDL_PrivateGetControllerMappingForGUID(SDL_JoystickGUID *guid)
{
    ControllerMapping_t *pSupportedController = s_pSupportedControllers;
    while (pSupportedController) {
        if (SDL_memcmp(guid, &pSupportedController->guid, sizeof(*guid)) == 0) {
            return pSupportedController;
        }
        pSupportedController = pSupportedController->next;
    }
    return NULL;
}

/*
 * convert a string to its enum equivalent
 */
SDL_GameControllerButton SDL_GameControllerGetButtonFromString(const char *pchString)
{
    int entry;
    if (!pchString || !pchString[0])
        return SDL_CONTROLLER_BUTTON_INVALID;

    for (entry = 0; map_StringForControllerButton[entry]; ++entry) {
        if (SDL_strcasecmp(pchString, map_StringForControllerButton[entry]) == 0)
            return entry;
    }
    return SDL_CONTROLLER_BUTTON_INVALID;
}

/*
 * convert a string to its enum equivalent
 */
SDL_GameControllerAxis SDL_GameControllerGetAxisFromString(const char *pchString)
{
    int entry;
    if (!pchString || !pchString[0])
        return SDL_CONTROLLER_AXIS_INVALID;

    for (entry = 0; map_StringForControllerAxis[entry]; ++entry) {
        if (!SDL_strcasecmp(pchString, map_StringForControllerAxis[entry]))
            return entry;
    }
    return SDL_CONTROLLER_AXIS_INVALID;
}

/*
 * given a controller button name and a joystick name update our mapping structure with it
 */
void SDL_PrivateGameControllerParseButton(const char *szGameButton, const char *szJoystickButton, struct _SDL_ControllerMapping *pMapping)
{
    int iSDLButton = 0;
    SDL_GameControllerButton button;
    SDL_GameControllerAxis axis;
    button = SDL_GameControllerGetButtonFromString(szGameButton);
    axis = SDL_GameControllerGetAxisFromString(szGameButton);
    iSDLButton = SDL_atoi(&szJoystickButton[1]);

    if (szJoystickButton[0] == 'a') {
        if (iSDLButton >= k_nMaxReverseEntries) {
            SDL_SetError("Axis index too large: %d", iSDLButton);
            return;
        }
        if (axis != SDL_CONTROLLER_AXIS_INVALID) {
            pMapping->axes[ axis ] = iSDLButton;
            pMapping->raxes[ iSDLButton ] = axis;
        } else if (button != SDL_CONTROLLER_BUTTON_INVALID) {
            pMapping->axesasbutton[ button ] = iSDLButton;
            pMapping->raxesasbutton[ iSDLButton ] = button;
        } else {
          //SDL_assert(!"How did we get here?");
        }

    } else if (szJoystickButton[0] == 'b') {
        if (iSDLButton >= k_nMaxReverseEntries) {
            SDL_SetError("Button index too large: %d", iSDLButton);
            return;
        }
        if (button != SDL_CONTROLLER_BUTTON_INVALID) {
            pMapping->buttons[ button ] = iSDLButton;
            pMapping->rbuttons[ iSDLButton ] = button;
        } else if (axis != SDL_CONTROLLER_AXIS_INVALID) {
            pMapping->buttonasaxis[ axis ] = iSDLButton;
            pMapping->rbuttonasaxis[ iSDLButton ] = axis;
        } else {
            //SDL_assert(!"How did we get here?");
        }
    } else if (szJoystickButton[0] == 'h') {
        int hat = SDL_atoi(&szJoystickButton[1]);
        int mask = SDL_atoi(&szJoystickButton[3]);
        if (hat >= 4) {
            SDL_SetError("Hat index too large: %d", iSDLButton);
        }

        if (button != SDL_CONTROLLER_BUTTON_INVALID) {
            int ridx;
            pMapping->hatasbutton[ button ].hat = hat;
            pMapping->hatasbutton[ button ].mask = mask;
            ridx = (hat << 4) | mask;
            pMapping->rhatasbutton[ ridx ] = button;
        } else if (axis != SDL_CONTROLLER_AXIS_INVALID) {
            //SDL_assert(!"Support hat as axis");
        } else {
            //SDL_assert(!"How did we get here?");
        }
    }
}

/*
 * given a controller mapping string update our mapping object
 */
static void
SDL_PrivateGameControllerParseControllerConfigString(struct _SDL_ControllerMapping *pMapping, const char *pchString)
{
    char szGameButton[20];
    char szJoystickButton[20];
    SDL_bool bGameButton = SDL_TRUE;
    int i = 0;
    const char *pchPos = pchString;

    SDL_memset(szGameButton, 0x0, sizeof(szGameButton));
    SDL_memset(szJoystickButton, 0x0, sizeof(szJoystickButton));

    while (pchPos && *pchPos) {
        if (*pchPos == ':') {
            i = 0;
            bGameButton = SDL_FALSE;
        } else if (*pchPos == ' ') {

        } else if (*pchPos == ',') {
            i = 0;
            bGameButton = SDL_TRUE;
            SDL_PrivateGameControllerParseButton(szGameButton, szJoystickButton, pMapping);
            SDL_memset(szGameButton, 0x0, sizeof(szGameButton));
            SDL_memset(szJoystickButton, 0x0, sizeof(szJoystickButton));

        } else if (bGameButton) {
            if (i >= sizeof(szGameButton)) {
                SDL_SetError("Button name too large: %s", szGameButton);
                return;
            }
            szGameButton[i] = *pchPos;
            i++;
        } else {
            if (i >= sizeof(szJoystickButton)) {
                SDL_SetError("Joystick button name too large: %s", szJoystickButton);
                return;
            }
            szJoystickButton[i] = *pchPos;
            i++;
        }
        pchPos++;
    }

    SDL_PrivateGameControllerParseButton(szGameButton, szJoystickButton, pMapping);

}

/*
 * Make a new button mapping struct
 */
void SDL_PrivateLoadButtonMapping(struct _SDL_ControllerMapping *pMapping, SDL_JoystickGUID guid, const char *pchName, const char *pchMapping)
{
    int j;

    pMapping->guid = guid;
    pMapping->name = pchName;

    /* set all the button mappings to non defaults */
    for (j = 0; j < SDL_CONTROLLER_AXIS_MAX; j++) {
        pMapping->axes[j] = -1;
        pMapping->buttonasaxis[j] = -1;
    }
    for (j = 0; j < SDL_CONTROLLER_BUTTON_MAX; j++) {
        pMapping->buttons[j] = -1;
        pMapping->axesasbutton[j] = -1;
        pMapping->hatasbutton[j].hat = -1;
    }

    for (j = 0; j < k_nMaxReverseEntries; j++) {
        pMapping->raxes[j] = SDL_CONTROLLER_AXIS_INVALID;
        pMapping->rbuttonasaxis[j] = SDL_CONTROLLER_AXIS_INVALID;
        pMapping->rbuttons[j] = SDL_CONTROLLER_BUTTON_INVALID;
        pMapping->raxesasbutton[j] = SDL_CONTROLLER_BUTTON_INVALID;
    }

    for (j = 0; j < k_nMaxHatEntries; j++) {
        pMapping->rhatasbutton[j] = SDL_CONTROLLER_BUTTON_INVALID;
    }

    SDL_PrivateGameControllerParseControllerConfigString(pMapping, pchMapping);
}

/*
 * Helper function to refresh a mapping
 */
void SDL_PrivateGameControllerRefreshMapping(ControllerMapping_t *pControllerMapping)
{
    SDL_GameController *gamecontrollerlist = SDL_gamecontrollers;
    while (gamecontrollerlist) {
        if (!SDL_memcmp(&gamecontrollerlist->mapping.guid, &pControllerMapping->guid, sizeof(pControllerMapping->guid))) {
            //SDL_Event event;
            //event.type = SDL_CONTROLLERDEVICEREMAPPED;
            //event.cdevice.which = gamecontrollerlist->joystick->instance_id;
            //SDL_PushEvent(&event);

            /* Not really threadsafe.  Should this lock access within SDL_GameControllerEventWatcher? */
            SDL_PrivateLoadButtonMapping(&gamecontrollerlist->mapping, pControllerMapping->guid, pControllerMapping->name, pControllerMapping->mapping);
        }

        gamecontrollerlist = gamecontrollerlist->next;
    }
}

/*
 * Helper function to add a mapping for a guid
 */
static ControllerMapping_t *
SDL_PrivateAddMappingForGUID(SDL_JoystickGUID jGUID, const char *mappingString, SDL_bool *existing)
{
    char *pchName;
    char *pchMapping;
    ControllerMapping_t *pControllerMapping;

    pchName = SDL_PrivateGetControllerNameFromMappingString(mappingString);
    if (!pchName) {
        SDL_SetError("Couldn't parse name from %s", mappingString);
        return NULL;
    }

    pchMapping = SDL_PrivateGetControllerMappingFromMappingString(mappingString);
    if (!pchMapping) {
        SDL_free(pchName);
        SDL_SetError("Couldn't parse %s", mappingString);
        return NULL;
    }

    pControllerMapping = SDL_PrivateGetControllerMappingForGUID(&jGUID);
    if (pControllerMapping) {
        /* Update existing mapping */
        SDL_free(pControllerMapping->name);
        pControllerMapping->name = pchName;
        SDL_free(pControllerMapping->mapping);
        pControllerMapping->mapping = pchMapping;
        /* refresh open controllers */
        SDL_PrivateGameControllerRefreshMapping(pControllerMapping);
        *existing = SDL_TRUE;
    } else {
        pControllerMapping = SDL_malloc(sizeof(*pControllerMapping));
        if (!pControllerMapping) {
            SDL_free(pchName);
            SDL_free(pchMapping);
            SDL_OutOfMemory();
            return NULL;
        }
        pControllerMapping->guid = jGUID;
        pControllerMapping->name = pchName;
        pControllerMapping->mapping = pchMapping;
        pControllerMapping->next = s_pSupportedControllers;
        s_pSupportedControllers = pControllerMapping;
        *existing = SDL_FALSE;
    }
    return pControllerMapping;
}

/*-----------------------------------------------------------------------------
 * Purpose: Returns the 4 bit nibble for a hex character
 * Input  : c -
 * Output : unsigned char
 *-----------------------------------------------------------------------------*/
static unsigned char nibble(char c)
{
    if ((c >= '0') && (c <= '9')) {
        return (unsigned char)(c - '0');
    }

    if ((c >= 'A') && (c <= 'F')) {
        return (unsigned char)(c - 'A' + 0x0a);
    }

    if ((c >= 'a') && (c <= 'f')) {
        return (unsigned char)(c - 'a' + 0x0a);
    }

    /* received an invalid character, and no real way to return an error */
    /* AssertMsg1(false, "Q_nibble invalid hex character '%c' ", c); */
    return 0;
}

/* convert the string version of a joystick guid to the struct */
SDL_JoystickGUID SDL_JoystickGetGUIDFromString(const char *pchGUID)
{
    SDL_JoystickGUID guid;
    int maxoutputbytes= sizeof(guid);
    size_t len = SDL_strlen(pchGUID);
    Uint8 *p;
    size_t i;

    /* Make sure it's even */
    len = (len) & ~0x1;

    SDL_memset(&guid, 0x00, sizeof(guid));

    p = (Uint8 *)&guid;
    for (i = 0; (i < len) && ((p - (Uint8 *)&guid) < maxoutputbytes); i+=2, p++) {
        *p = (nibble(pchGUID[i]) << 4) | nibble(pchGUID[i+1]);
    }

    return guid;
}

/*
 * grab the guid string from a mapping string
 */
char *SDL_PrivateGetControllerGUIDFromMappingString(const char *pMapping)
{
    const char *pFirstComma = SDL_strchr(pMapping, ',');
    if (pFirstComma) {
        char *pchGUID = SDL_malloc(pFirstComma - pMapping + 1);
        if (!pchGUID) {
            SDL_OutOfMemory();
            return NULL;
        }
        SDL_memcpy(pchGUID, pMapping, pFirstComma - pMapping);
        pchGUID[ pFirstComma - pMapping ] = 0;
        return pchGUID;
    }
    return NULL;
}

/*
 * Add or update an entry into the Mappings Database
 */
int
SDL_GameControllerAddMapping(const char *mappingString)
{
    char *pchGUID;
    SDL_JoystickGUID jGUID;
    SDL_bool is_xinput_mapping = SDL_FALSE;
    SDL_bool is_emscripten_mapping = SDL_FALSE;
    SDL_bool existing = SDL_FALSE;
    ControllerMapping_t *pControllerMapping;

    if (!mappingString) {
        SDL_InvalidParamError("mappingString");
        return -1;
    }

    pchGUID = SDL_PrivateGetControllerGUIDFromMappingString(mappingString);
    if (!pchGUID) {
        SDL_SetError("Couldn't parse GUID from %s", mappingString);
        return -1;
    }
    if (!SDL_strcasecmp(pchGUID, "xinput")) {
        is_xinput_mapping = SDL_TRUE;
    }
    if (!SDL_strcasecmp(pchGUID, "emscripten")) {
        is_emscripten_mapping = SDL_TRUE;
    }
    jGUID = SDL_JoystickGetGUIDFromString(pchGUID);
    SDL_free(pchGUID);

    pControllerMapping = SDL_PrivateAddMappingForGUID(jGUID, mappingString, &existing);
    if (!pControllerMapping) {
        return -1;
    }

    if (existing) {
        return 0;
    } else {
        if (is_xinput_mapping) {
            s_pXInputMapping = pControllerMapping;
        }
        if (is_emscripten_mapping) {
            s_pEmscriptenMapping = pControllerMapping;
        }
        return 1;
    }
}

/*
 * Add or update an entry into the Mappings Database
 */
Uint8 SDL_GameControllerAddMappingsFromFile(char *db_path, Uint8 freerw)
{
    const char *platform = "linux";
    int controllers = 0;
    char *buf, *line, *line_end, *tmp, *comma, line_platform[64];
    size_t db_size, platform_len;
    SDL_RWops* rw = SDL_RWFromFile(db_path, "rb");

    //db_size = (size_t)SDL_RWsize(rw);
    fseek(rw->hidden.stdio.fp, 0L, SEEK_END);
    db_size = ftell(rw->hidden.stdio.fp);
    fseek(rw->hidden.stdio.fp, 0L, SEEK_SET);

    buf = (char *)SDL_malloc(db_size + 1);
    if (buf == NULL) {
        if (freerw) {
            SDL_RWclose(rw);
        }
        SDL_SetError("Could not allocate space to read DB into memory");
        return -1;
    }

    if (SDL_RWread(rw, buf, db_size, 1) != 1) {
        if (freerw) {
            SDL_RWclose(rw);
        }
        SDL_free(buf);
        SDL_SetError("Could not read DB");
        return -1;
    }

    if (freerw) {
        SDL_RWclose(rw);
    }

    buf[db_size] = '\0';
    line = buf;

    while (line < buf + db_size) {
        line_end = SDL_strchr(line, '\n');
        if (line_end != NULL) {
            *line_end = '\0';
        } else {
            line_end = buf + db_size;
        }

        /* Extract and verify the platform */
        tmp = SDL_strstr(line, SDL_CONTROLLER_PLATFORM_FIELD);
        if (tmp != NULL) {
            tmp += SDL_strlen(SDL_CONTROLLER_PLATFORM_FIELD);
            comma = SDL_strchr(tmp, ',');
            if (comma != NULL) {
                platform_len = comma - tmp + 1;
                if (platform_len + 1 < SDL_arraysize(line_platform)) {
                    SDL_strlcpy(line_platform, tmp, platform_len);
                    if (SDL_strncasecmp(line_platform, platform, platform_len) == 0 &&
                        SDL_GameControllerAddMapping(line) > 0) {
                        controllers++;
                    }
                }
            }
        }

        line = line_end + 1;
    }

    SDL_free(buf);
    return controllers;
}

SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int device_index)
{
    char guidstr[33];
    SDL_JoystickGUID emptyGUID;
    char *dev_path = get_gamepad_dev_path(0);
    if (!dev_path) {
      SDL_zero(emptyGUID);
      return emptyGUID;
    }

    get_guid(dev_path, guidstr);
    free(dev_path);

    return SDL_JoystickGetGUIDFromString(guidstr);
}

/*
 * Helper function to determine pre-calculated offset to certain joystick mappings
 */
ControllerMapping_t *SDL_PrivateGetControllerMapping(int device_index)
{
    SDL_JoystickGUID jGUID = SDL_JoystickGetDeviceGUID(device_index);
    ControllerMapping_t *mapping;

    mapping = SDL_PrivateGetControllerMappingForGUID(&jGUID);
#if SDL_JOYSTICK_XINPUT
    if (!mapping && SDL_SYS_IsXInputGamepad_DeviceIndex(device_index)) {
        mapping = s_pXInputMapping;
    }
#endif
#if defined(SDL_JOYSTICK_EMSCRIPTEN)
    if (!mapping && s_pEmscriptenMapping) {
        mapping = s_pEmscriptenMapping;
    }
#endif

#if 0  // TODO
#ifdef __LINUX__
    if (!mapping) {
        const char *name = SDL_JoystickNameForIndex(device_index);
        if (name) {
            if (SDL_strstr(name, "Xbox 360 Wireless Receiver")) {
                /* The Linux driver xpad.c maps the wireless dpad to buttons */
                SDL_bool existing;
                mapping = SDL_PrivateAddMappingForGUID(jGUID,
"none,X360 Wireless Controller,a:b0,b:b1,back:b6,dpdown:b14,dpleft:b11,dpright:b12,dpup:b13,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,",
                              &existing);
            }
        }
    }
#endif /* __LINUX__ */

    if (!mapping) {
        const char *name = SDL_JoystickNameForIndex(device_index);
        if (name) {
            if (SDL_strstr(name, "Xbox") || SDL_strstr(name, "X-Box")) {
                mapping = s_pXInputMapping;
            }
        }
    }
#endif
    return mapping;
}

/*
 * Open a controller for use - the index passed as an argument refers to
 * the N'th controller on the system.  This index is the value which will
 * identify this controller in future controller events.
 *
 * This function returns a controller identifier, or NULL if an error occurred.
 */
SDL_GameController *
SDL_GameControllerOpen(int device_index)
{
    SDL_GameController *gamecontroller;
    SDL_GameController *gamecontrollerlist;
    ControllerMapping_t *pSupportedController = NULL;

    if ((device_index < 0) || (device_index >= SDL_NumJoysticks())) {
        SDL_SetError("There are %d joysticks available", SDL_NumJoysticks());
        return (NULL);
    }

    gamecontrollerlist = SDL_gamecontrollers;
    /* If the controller is already open, return it */
    /* TODO
    while (gamecontrollerlist) {
        if (SDL_SYS_GetInstanceIdOfDeviceIndex(device_index) == gamecontrollerlist->joystick->instance_id) {
                gamecontroller = gamecontrollerlist;
                ++gamecontroller->ref_count;
                return (gamecontroller);
        }
        gamecontrollerlist = gamecontrollerlist->next;
    }
    */

    /* Find a controller mapping */
    pSupportedController =  SDL_PrivateGetControllerMapping(device_index);
    if (!pSupportedController) {
        SDL_SetError("Couldn't find mapping for device (%d)", device_index);
        return (NULL);
    }

    /* Create and initialize the joystick */
    gamecontroller = (SDL_GameController *) SDL_malloc((sizeof *gamecontroller));
    if (gamecontroller == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }

    SDL_memset(gamecontroller, 0, (sizeof *gamecontroller));
    gamecontroller->joystick = SDL_JoystickOpen(device_index);
    if (!gamecontroller->joystick) {
        SDL_free(gamecontroller);
        return NULL;
    }

    SDL_PrivateLoadButtonMapping(&gamecontroller->mapping, pSupportedController->guid, pSupportedController->name, pSupportedController->mapping);

    printf("Gamecontroller: %s\n", pSupportedController->name);

    /* Add joystick to list */
    ++gamecontroller->ref_count;
    /* Link the joystick in the list */
    gamecontroller->next = SDL_gamecontrollers;
    SDL_gamecontrollers = gamecontroller;

    //SDL_SYS_JoystickUpdate(gamecontroller->joystick);
    SDL_JoystickUpdate(); // TODO

    return (gamecontroller);
}

/*
 * Get the current state of an axis control on a controller
 */
Sint16
SDL_GameControllerGetAxis(SDL_GameController * gamecontroller, SDL_GameControllerAxis axis)
{
    if (!gamecontroller)
        return 0;

    if (gamecontroller->mapping.axes[axis] >= 0) {
        Sint16 value = (SDL_JoystickGetAxis(gamecontroller->joystick, gamecontroller->mapping.axes[axis]));
        switch (axis) {
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                /* Shift it to be 0 - 32767. */
                value = value / 2 + 16384;
            default:
                break;
        }
        return value;
    } else if (gamecontroller->mapping.buttonasaxis[axis] >= 0) {
        Uint8 value;
        value = SDL_JoystickGetButton(gamecontroller->joystick, gamecontroller->mapping.buttonasaxis[axis]);
        if (value > 0)
            return 32767;
        return 0;
    }
    return 0;
}


/*
 * Get the current state of a button on a controller
 */
Uint8
SDL_GameControllerGetButton(SDL_GameController * gamecontroller, SDL_GameControllerButton button)
{
    if (!gamecontroller)
        return 0;

    if (gamecontroller->mapping.buttons[button] >= 0) {
        return (SDL_JoystickGetButton(gamecontroller->joystick, gamecontroller->mapping.buttons[button]));
    } else if (gamecontroller->mapping.axesasbutton[button] >= 0) {
        Sint16 value;
        value = SDL_JoystickGetAxis(gamecontroller->joystick, gamecontroller->mapping.axesasbutton[button]);
        if (ABS(value) > 32768/2)
            return 1;
        return 0;
    } else if (gamecontroller->mapping.hatasbutton[button].hat >= 0) {
        Uint8 value;
        value = SDL_JoystickGetHat(gamecontroller->joystick, gamecontroller->mapping.hatasbutton[button].hat);

        if (value & gamecontroller->mapping.hatasbutton[button].mask)
            return 1;
        return 0;
    }

    return 0;
}

#if 0
int main(int argc, char* argv[]) {
    char guidstr[33];
    SDL_GameController *gc;
    char *dev_path = get_gamepad_dev_path(0);
    if (!dev_path) {
        return -1;
    }
    get_guid(dev_path, guidstr);
    free(dev_path);

    if (SDL_Init(SDL_INIT_JOYSTICK) < 0) {
      fprintf(stderr, "Unable to initialize SDL: %s\n", SDL_GetError());
      exit(1);
    }

    SDL_GameControllerAddMappingsFromFile("/home/godfryd/repos/gof-console/gamecontrollerdb.txt", 1);

    gc = SDL_GameControllerOpen(0);
    printf("gc %p %s\n", gc, SDL_GetError());

    while (1) {
      SDL_JoystickUpdate();
      int i = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A);
      printf("pressed %d\n", i);
    }

    return 0;
}
#endif
