// Boot menu for the badge.
//
// Vanilla Doom boots into the attract loop: TITLEPIC, then DEMO1, which warps
// to E1M5 and needs more level data than this board can hold. The badge used
// to sidestep that by calling G_InitNew straight from D_DoomMain. This menu
// takes that slot instead: it sits in GS_DEMOSCREEN with the page ticker
// parked, so the rest of the engine sees the same state it would during the
// title screen, and Singleplayer runs the exact G_DeferedInitNew the old
// autostart did.
//
// Everything is drawn with the menu code's own routines -- M_DOOM and the
// skull cursor via V_DrawPatchDirect, text via M_WriteText on hu_font -- all
// straight out of the flash-mapped WAD, so no framebuffer or scratch RAM is
// added.

#include "badge_boot.h"

#include "doomdef.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "d_main.h"
#include "deh_main.h"
#include "g_game.h"
#include "i_swap.h"
#include "m_menu.h"
#include "s_sound.h"
#include "sounds.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "hu_stuff.h"
#include "i_video.h"

bootscreen_t bootscreen = BOOT_NONE;

// D_DoomMain's autostart parameters. Singleplayer launches with these
// untouched, so the arena, skill and controls match the previous build.
extern skill_t startskill;
extern int     startepisode;
extern int     startmap;

extern patch_t *hu_font[HU_FONTSIZE];

// m_menu.c and d_main.c keep these file-local in spirit but not in linkage.
extern char   *skullName[2];
void           M_WriteText(int x, int y, char *string);
int            M_StringWidth(char *string);
extern int     pagetic;
extern int     demosequence;
extern boolean advancedemo;

enum { ITEM_SINGLE, ITEM_MULTI, ITEM_COUNT };

static const char *const item_names[ITEM_COUNT] = {
    "SINGLEPLAYER",
    "MULTIPLAYER",
};

// Same geometry as M_DrawMainMenu: logo at (94,2), first item at (97,64).
#define LOGO_X      94
#define LOGO_Y      2
#define ITEM_X      97
#define ITEM_Y      72
#define LINE_H      16
#define SKULL_X_OFF 32
#define SKULL_Y_OFF (-5)

// Skull cursor: two frames, swapped every 8 tics, like m_menu.c does.
#define SKULL_TICS  8

static int  item_on;
static int  skull_tic;
static int  skull_frame;
static int  connect_tic;

static void Boot_ShowMenu(void)
{
    bootscreen = BOOT_MENU;
    skull_tic = SKULL_TICS;
    skull_frame = 0;
}

void Boot_Start(void)
{
    // Mirror D_DoAdvanceDemo's reset of the bits that would otherwise carry
    // over from a game the player just ended.
    players[consoleplayer].playerstate = PST_LIVE;
    usergame = false;
    paused = false;
    gameaction = ga_nothing;
    advancedemo = false;
    demosequence = -1;

    // GS_DEMOSCREEN keeps G_Ticker and D_Display in their title-screen paths.
    // The page ticker still runs there, so park it out of reach: at -1 it
    // would call D_AdvanceDemo and start the demo loop this board cannot play.
    gamestate = GS_DEMOSCREEN;
    pagetic = INT_MAX;

    item_on = ITEM_SINGLE;
    Boot_ShowMenu();
}

static void Boot_StartSingleplayer(void)
{
    bootscreen = BOOT_NONE;
    G_DeferedInitNew(startskill, startepisode, startmap);
}

static void Boot_StartConnect(void)
{
    bootscreen = BOOT_CONNECT;
    connect_tic = 0;
    // The radio hand-off (discovery, pairing, D_InitNetGame) plugs in here.
}

static boolean Boot_MenuResponder(int key)
{
    switch (key)
    {
      case KEY_UPARROW:
        item_on = (item_on + ITEM_COUNT - 1) % ITEM_COUNT;
        S_StartSound(NULL, sfx_pstop);
        return true;

      case KEY_DOWNARROW:
        item_on = (item_on + 1) % ITEM_COUNT;
        S_StartSound(NULL, sfx_pstop);
        return true;

      case KEY_ENTER:
        S_StartSound(NULL, sfx_pistol);
        if (item_on == ITEM_SINGLE)
            Boot_StartSingleplayer();
        else
            Boot_StartConnect();
        return true;
    }

    return true;    // nothing else means anything here
}

static boolean Boot_ConnectResponder(int key)
{
    if (key == KEY_ESCAPE)
    {
        S_StartSound(NULL, sfx_swtchx);
        Boot_ShowMenu();
    }
    return true;
}

boolean Boot_Responder(event_t *ev)
{
    if (bootscreen == BOOT_NONE)
        return false;

    // Swallow releases too, so nothing leaks through to M_Responder or
    // G_Responder while a boot screen owns the buttons.
    if (ev->type != ev_keydown)
        return true;

    if (bootscreen == BOOT_MENU)
        return Boot_MenuResponder(ev->data1);

    return Boot_ConnectResponder(ev->data1);
}

void Boot_Ticker(void)
{
    if (bootscreen == BOOT_NONE)
        return;

    if (--skull_tic <= 0)
    {
        skull_frame ^= 1;
        skull_tic = SKULL_TICS;
    }

    if (bootscreen == BOOT_CONNECT)
        connect_tic++;
}

static void Boot_DrawCentered(int y, const char *s)
{
    M_WriteText(SCREENWIDTH / 2 - M_StringWidth((char *)s) / 2, y, (char *)s);
}

static void Boot_DrawMenu(void)
{
    int i;

    V_DrawPatchDirect(LOGO_X, LOGO_Y,
                      W_CacheLumpName(DEH_String("M_DOOM"), PU_CACHE));

    for (i = 0; i < ITEM_COUNT; i++)
        M_WriteText(ITEM_X, ITEM_Y + i * LINE_H, (char *)item_names[i]);

    V_DrawPatchDirect(ITEM_X - SKULL_X_OFF,
                      ITEM_Y + item_on * LINE_H + SKULL_Y_OFF,
                      W_CacheLumpName(DEH_String(skullName[skull_frame]),
                                      PU_CACHE));

    Boot_DrawCentered(SCREENHEIGHT - 24, "UP/DOWN TO CHOOSE, START TO PLAY");
}

static void Boot_DrawConnect(void)
{
    // A three-step ellipsis so the screen visibly ticks while it waits.
    static const char *const waiting[3] = {
        "SEARCHING FOR A BADGE.",
        "SEARCHING FOR A BADGE..",
        "SEARCHING FOR A BADGE...",
    };
    int line_h = SHORT(hu_font[0]->height) + 4;
    int y = 64;

    V_DrawPatchDirect(LOGO_X, LOGO_Y,
                      W_CacheLumpName(DEH_String("M_DOOM"), PU_CACHE));

    Boot_DrawCentered(y, "MULTIPLAYER");
    y += line_h * 2;
    Boot_DrawCentered(y, waiting[(connect_tic / TICRATE) % 3]);

    Boot_DrawCentered(SCREENHEIGHT - 24, "HOME TO GO BACK");
}

void Boot_Drawer(void)
{
    if (bootscreen == BOOT_NONE)
        return;

    // Black ground, painted into the buffer the engine already owns.
    V_DrawFilledBox(0, 0, SCREENWIDTH, SCREENHEIGHT, 0);

    if (bootscreen == BOOT_MENU)
        Boot_DrawMenu();
    else
        Boot_DrawConnect();
}
