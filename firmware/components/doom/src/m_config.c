//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 1993-2008 Raven Software
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//    Configuration file interface.
//


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "config.h"

#include "doomtype.h"
#include "doomkeys.h"
#include "doomfeatures.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"

#include "z_zone.h"

//
// DEFAULTS
//

// Location where all configuration data is stored - 
// default.cfg, savegames, etc.

char *configdir;

// Default filenames for configuration files.

static char *default_main_config;
static char *default_extra_config;

typedef enum 
{
    DEFAULT_INT,
    DEFAULT_INT_HEX,
    DEFAULT_STRING,
    DEFAULT_FLOAT,
    DEFAULT_KEY,
} default_type_t;

typedef struct
{
    // Name of the variable
    char *name;

    // Pointer to the location in memory of the variable
    void *location;

    // Type of the variable
    default_type_t type;

    // If this is a key value, the original integer scancode we read from
    // the config file before translating it to the internal key value.
    // If zero, we didn't read this value from a config file.
    int untranslated;

    // The value we translated the scancode into when we read the 
    // config file on startup.  If the variable value is different from
    // this, it has been changed and needs to be converted; otherwise,
    // use the 'untranslated' value.
    int original_translated;

    // If true, this config variable has been bound to a variable
    // and is being used.
    boolean bound;
} default_t;

typedef struct
{
    default_t *defaults;
    int numdefaults;
    char *filename;
} default_collection_t;

#define CONFIG_VARIABLE_GENERIC(name, type) \
    { #name, NULL, type, 0, 0, false }

#define CONFIG_VARIABLE_KEY(name) \
    CONFIG_VARIABLE_GENERIC(name, DEFAULT_KEY)
#define CONFIG_VARIABLE_INT(name) \
    CONFIG_VARIABLE_GENERIC(name, DEFAULT_INT)
#define CONFIG_VARIABLE_INT_HEX(name) \
    CONFIG_VARIABLE_GENERIC(name, DEFAULT_INT_HEX)
#define CONFIG_VARIABLE_FLOAT(name) \
    CONFIG_VARIABLE_GENERIC(name, DEFAULT_FLOAT)
#define CONFIG_VARIABLE_STRING(name) \
    CONFIG_VARIABLE_GENERIC(name, DEFAULT_STRING)

//! @begin_config_file default

// The badge has no filesystem: there is no default.cfg to read and nowhere to
// write one, and nothing outside this file ever calls M_BindIntVariable, so
// every entry in these two lists was permanently unbound. They cost 4,680
// bytes of .bss between them -- and .bss ends where the heap region the zone
// is carved from begins, so this is 4,680 bytes handed straight to Doom.
//
// The variables they named still exist and keep their compiled-in defaults;
// only the config-file metadata is gone. Restore from git if a filesystem
// ever appears. badge_unused is a placeholder: C has no empty array.
static default_t	doom_defaults_list[] =
{
    CONFIG_VARIABLE_INT(badge_unused),
};

static default_collection_t doom_defaults =
{
    doom_defaults_list,
    arrlen(doom_defaults_list),
    NULL,
};

//! @begin_config_file extended

static default_t extra_defaults_list[] =
{
    CONFIG_VARIABLE_INT(badge_unused),
};

static default_collection_t extra_defaults =
{
    extra_defaults_list,
    arrlen(extra_defaults_list),
    NULL,
};

// Search a collection for a variable

static default_t *SearchCollection(default_collection_t *collection, char *name)
{
    int i;

    for (i=0; i<collection->numdefaults; ++i) 
    {
        if (!strcmp(name, collection->defaults[i].name))
        {
            return &collection->defaults[i];
        }
    }

    return NULL;
}

// Mapping from DOS keyboard scan code to internal key code (as defined
// in doomkey.h). I think I (fraggle) reused this from somewhere else
// but I can't find where. Anyway, notes:
//  * KEY_PAUSE is wrong - it's in the KEY_NUMLOCK spot. This shouldn't
//    matter in terms of Vanilla compatibility because neither of
//    those were valid for key bindings.
//  * There is no proper scan code for PrintScreen (on DOS machines it
//    sends an interrupt). So I added a fake scan code of 126 for it.
//    The presence of this is important so we can bind PrintScreen as
//    a screenshot key.
static const int scantokey[128] =
{
    0  ,    27,     '1',    '2',    '3',    '4',    '5',    '6',
    '7',    '8',    '9',    '0',    '-',    '=',    KEY_BACKSPACE, 9,
    'q',    'w',    'e',    'r',    't',    'y',    'u',    'i',
    'o',    'p',    '[',    ']',    13,		KEY_RCTRL, 'a',    's',
    'd',    'f',    'g',    'h',    'j',    'k',    'l',    ';',
    '\'',   '`',    KEY_RSHIFT,'\\',   'z',    'x',    'c',    'v',
    'b',    'n',    'm',    ',',    '.',    '/',    KEY_RSHIFT,KEYP_MULTIPLY,
    KEY_RALT,  ' ',  KEY_CAPSLOCK,KEY_F1,  KEY_F2,   KEY_F3,   KEY_F4,   KEY_F5,
    KEY_F6,   KEY_F7,   KEY_F8,   KEY_F9,   KEY_F10,  /*KEY_NUMLOCK?*/KEY_PAUSE,KEY_SCRLCK,KEY_HOME,
    KEY_UPARROW,KEY_PGUP,KEY_MINUS,KEY_LEFTARROW,KEYP_5,KEY_RIGHTARROW,KEYP_PLUS,KEY_END,
    KEY_DOWNARROW,KEY_PGDN,KEY_INS,KEY_DEL,0,   0,      0,      KEY_F11,
    KEY_F12,  0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      KEY_PRTSCR, 0
};


static void SaveDefaultCollection(default_collection_t *collection)
{
#if ORIGCODE
    default_t *defaults;
    int i, v;
    FILE *f;
	
    f = fopen (collection->filename, "w");
    if (!f)
	return; // can't write the file, but don't complain

    defaults = collection->defaults;
		
    for (i=0 ; i<collection->numdefaults ; i++)
    {
        int chars_written;

        // Ignore unbound variables

        if (!defaults[i].bound)
        {
            continue;
        }

        // Print the name and line up all values at 30 characters

        chars_written = fprintf(f, "%s ", defaults[i].name);

        for (; chars_written < 30; ++chars_written)
            fprintf(f, " ");

        // Print the value

        switch (defaults[i].type) 
        {
            case DEFAULT_KEY:

                // use the untranslated version if we can, to reduce
                // the possibility of screwing up the user's config
                // file
                
                v = * (int *) defaults[i].location;

                if (v == KEY_RSHIFT)
                {
                    // Special case: for shift, force scan code for
                    // right shift, as this is what Vanilla uses.
                    // This overrides the change check below, to fix
                    // configuration files made by old versions that
                    // mistakenly used the scan code for left shift.

                    v = 54;
                }
                else if (defaults[i].untranslated
                      && v == defaults[i].original_translated)
                {
                    // Has not been changed since the last time we
                    // read the config file.

                    v = defaults[i].untranslated;
                }
                else
                {
                    // search for a reverse mapping back to a scancode
                    // in the scantokey table

                    int s;

                    for (s=0; s<128; ++s)
                    {
                        if (scantokey[s] == v)
                        {
                            v = s;
                            break;
                        }
                    }
                }

	        fprintf(f, "%i", v);
                break;

            case DEFAULT_INT:
	        fprintf(f, "%i", * (int *) defaults[i].location);
                break;

            case DEFAULT_INT_HEX:
	        fprintf(f, "0x%x", * (int *) defaults[i].location);
                break;

            case DEFAULT_FLOAT:
                fprintf(f, "%f", * (float *) defaults[i].location);
                break;

            case DEFAULT_STRING:
	        fprintf(f,"\"%s\"", * (char **) (defaults[i].location));
                break;
        }

        fprintf(f, "\n");
    }

    fclose (f);
#endif
}

// Parses integer values in the configuration file

static int ParseIntParameter(char *strparm)
{
    int parm;

    if (strparm[0] == '0' && strparm[1] == 'x')
        sscanf(strparm+2, "%x", &parm);
    else
        sscanf(strparm, "%i", &parm);

    return parm;
}

static void SetVariable(default_t *def, char *value)
{
    int intparm;

    // parameter found

    switch (def->type)
    {
        case DEFAULT_STRING:
            * (char **) def->location = strdup(value);
            break;

        case DEFAULT_INT:
        case DEFAULT_INT_HEX:
            * (int *) def->location = ParseIntParameter(value);
            break;

        case DEFAULT_KEY:

            // translate scancodes read from config
            // file (save the old value in untranslated)

            intparm = ParseIntParameter(value);
            def->untranslated = intparm;
            if (intparm >= 0 && intparm < 128)
            {
                intparm = scantokey[intparm];
            }
            else
            {
                intparm = 0;
            }

            def->original_translated = intparm;
            * (int *) def->location = intparm;
            break;

        case DEFAULT_FLOAT:
            * (float *) def->location = (float) atof(value);
            break;
    }
}

static void LoadDefaultCollection(default_collection_t *collection)
{
#if ORIGCODE
    FILE *f;
    default_t *def;
    char defname[80];
    char strparm[100];

    // read the file in, overriding any set defaults
    f = fopen(collection->filename, "r");

    if (f == NULL)
    {
        // File not opened, but don't complain. 
        // It's probably just the first time they ran the game.

        return;
    }

    while (!feof(f))
    {
        if (fscanf(f, "%79s %99[^\n]\n", defname, strparm) != 2)
        {
            // This line doesn't match

            continue;
        }

        // Find the setting in the list

        def = SearchCollection(collection, defname);

        if (def == NULL || !def->bound)
        {
            // Unknown variable?  Unbound variables are also treated
            // as unknown.

            continue;
        }

        // Strip off trailing non-printable characters (\r characters
        // from DOS text files)

        while (strlen(strparm) > 0 && !isprint(strparm[strlen(strparm)-1]))
        {
            strparm[strlen(strparm)-1] = '\0';
        }

        // Surrounded by quotes? If so, remove them.
        if (strlen(strparm) >= 2
         && strparm[0] == '"' && strparm[strlen(strparm) - 1] == '"')
        {
            strparm[strlen(strparm) - 1] = '\0';
            memmove(strparm, strparm + 1, sizeof(strparm) - 1);
        }

        SetVariable(def, strparm);
    }

    fclose (f);
#endif
}

// Set the default filenames to use for configuration files.

void M_SetConfigFilenames(char *main_config, char *extra_config)
{
    default_main_config = main_config;
    default_extra_config = extra_config;
}

//
// M_SaveDefaults
//

void M_SaveDefaults (void)
{
    SaveDefaultCollection(&doom_defaults);
    SaveDefaultCollection(&extra_defaults);
}

//
// Save defaults to alternate filenames
//

void M_SaveDefaultsAlternate(char *main, char *extra)
{
    char *orig_main;
    char *orig_extra;

    // Temporarily change the filenames

    orig_main = doom_defaults.filename;
    orig_extra = extra_defaults.filename;

    doom_defaults.filename = main;
    extra_defaults.filename = extra;

    M_SaveDefaults();

    // Restore normal filenames

    doom_defaults.filename = orig_main;
    extra_defaults.filename = orig_extra;
}

//
// M_LoadDefaults
//

void M_LoadDefaults (void)
{
    int i;
 
    // check for a custom default file

    //!
    // @arg <file>
    // @vanilla
    //
    // Load main configuration from the specified file, instead of the
    // default.
    //

    i = M_CheckParmWithArgs("-config", 1);

    if (i)
    {
	doom_defaults.filename = myargv[i+1];
	printf ("	default file: %s\n",doom_defaults.filename);
    }
    else
    {
        doom_defaults.filename
            = M_StringJoin(configdir, default_main_config, NULL);
    }

    printf("saving config in %s\n", doom_defaults.filename);

    //!
    // @arg <file>
    //
    // Load additional configuration from the specified file, instead of
    // the default.
    //

    i = M_CheckParmWithArgs("-extraconfig", 1);

    if (i)
    {
        extra_defaults.filename = myargv[i+1];
        printf("        extra configuration file: %s\n", 
               extra_defaults.filename);
    }
    else
    {
        extra_defaults.filename
            = M_StringJoin(configdir, default_extra_config, NULL);
    }

    LoadDefaultCollection(&doom_defaults);
    LoadDefaultCollection(&extra_defaults);
}

// Get a configuration file variable by its name

static default_t *GetDefaultForName(char *name)
{
    default_t *result;

    // Try the main list and the extras

    result = SearchCollection(&doom_defaults, name);

    if (result == NULL)
    {
        result = SearchCollection(&extra_defaults, name);
    }

    // Upstream treats a miss as an internal error, because upstream has a
    // config file and every name in it is in one of the two lists. Both lists
    // are empty here (see the note above doom_defaults_list), so every name
    // misses -- m_controls.c alone binds around forty of them at startup.
    //
    // Returning NULL is correct rather than merely quiet: with no filesystem
    // there is nothing to load a value from and nowhere to save one to, so a
    // binding would have no effect even if it succeeded. The variable keeps
    // its compiled-in default, which is the value the badge wants anyway.
    // Every caller below is NULL-guarded.
    return result;
}

//
// Bind a variable to a given configuration file variable, by name.
//

void M_BindVariable(char *name, void *location)
{
    default_t *variable;

    variable = GetDefaultForName(name);

    if (variable == NULL)
        return;

    variable->location = location;
    variable->bound = true;
}

// Set the value of a particular variable; an API function for other
// parts of the program to assign values to config variables by name.

boolean M_SetVariable(char *name, char *value)
{
    default_t *variable;

    variable = GetDefaultForName(name);

    if (variable == NULL || !variable->bound)
    {
        return false;
    }

    SetVariable(variable, value);

    return true;
}

// Get the value of a variable.

int M_GetIntVariable(char *name)
{
    default_t *variable;

    variable = GetDefaultForName(name);

    if (variable == NULL || !variable->bound
     || (variable->type != DEFAULT_INT && variable->type != DEFAULT_INT_HEX))
    {
        return 0;
    }

    return *((int *) variable->location);
}

const char *M_GetStrVariable(char *name)
{
    default_t *variable;

    variable = GetDefaultForName(name);

    if (variable == NULL || !variable->bound
     || variable->type != DEFAULT_STRING)
    {
        return NULL;
    }

    return *((const char **) variable->location);
}

float M_GetFloatVariable(char *name)
{
    default_t *variable;

    variable = GetDefaultForName(name);

    if (variable == NULL || !variable->bound
     || variable->type != DEFAULT_FLOAT)
    {
        return 0;
    }

    return *((float *) variable->location);
}

// Get the path to the default configuration dir to use, if NULL
// is passed to M_SetConfigDir.

static char *GetDefaultConfigDir(void)
{
    char *result = (char *)malloc(2);
    result[0] = '.';
    result[1] = '\0';

    return result;
}

// 
// SetConfigDir:
//
// Sets the location of the configuration directory, where configuration
// files are stored - default.cfg, chocolate-doom.cfg, savegames, etc.
//

void M_SetConfigDir(char *dir)
{
    // Use the directory that was passed, or find the default.

    if (dir != NULL)
    {
        configdir = dir;
    }
    else
    {
        configdir = GetDefaultConfigDir();
    }

    if (strcmp(configdir, "") != 0)
    {
        printf("Using %s for configuration and saves\n", configdir);
    }

    // Make the directory if it doesn't already exist:

    M_MakeDirectory(configdir);
}

//
// Calculate the path to the directory to use to store save games.
// Creates the directory as necessary.
//

char *M_GetSaveGameDir(char *iwadname)
{
    char *savegamedir;
#if ORIGCODE
    char *topdir;
#endif

    // If not "doing" a configuration directory (Windows), don't "do"
    // a savegame directory, either.

    if (!strcmp(configdir, ""))
    {
    	savegamedir = strdup("");
    }
    else
    {
#if ORIGCODE
        // ~/.chocolate-doom/savegames

        topdir = M_StringJoin(configdir, "savegame", NULL);
        M_MakeDirectory(topdir);

        // eg. ~/.chocolate-doom/savegames/doom2.wad/

        savegamedir = M_StringJoin(topdir, DIR_SEPARATOR_S, iwadname,
                                   DIR_SEPARATOR_S, NULL);

        M_MakeDirectory(savegamedir);

        free(topdir);
#else
        savegamedir = M_StringJoin(configdir, DIR_SEPARATOR_S, ".savegame/", NULL);

        M_MakeDirectory(savegamedir);

        printf ("Using %s for savegames\n", savegamedir);
#endif
    }

    return savegamedir;
}

