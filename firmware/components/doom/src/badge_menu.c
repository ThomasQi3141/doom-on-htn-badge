// The boot lobby. See badge_menu.h for why it is split around D_CheckNetGame.
//
// Everything here draws with hu_font through M_WriteText rather than with
// patch lumps. The real main menu is built from M_NGAME and friends, and the
// arena WAD has none of that art -- adding it would cost flash and a build
// step to save nothing, since the STCFN* font lumps are already present for
// the HUD.
//
// Nothing in this file allocates. The zone heap has under 10 KB free during
// play and this runs before the level is loaded, so a scratch buffer here
// would be paid for by the map later.

#include <string.h>

#include "doomdef.h"
#include "doomkeys.h"
#include "doomgeneric.h"
#include "deh_str.h"
#include "hu_stuff.h"
#include "i_video.h"
#include "m_misc.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "badge_menu.h"
#include "badge_net.h"

// m_menu.c's text routines. m_menu.h does not declare them -- vanilla only
// ever called them from inside m_menu.c.
extern void M_WriteText(int x, int y, char *string);
extern int  M_StringWidth(char *string);

extern patch_t *hu_font[HU_FONTSIZE];

#define LOBBY_ITEMS 3

static const char *s_items[LOBBY_ITEMS] =
{
    "1 PLAYER",
    "HOST CO-OP",
    "JOIN CO-OP",
};

static const badge_net_role_t s_roles[LOBBY_ITEMS] =
{
    BADGE_NET_OFF,
    BADGE_NET_HOST,
    BADGE_NET_CLIENT,
};

// ------------------------------------------------------------------ drawing

// D_DoomMain has not reached I_InitGraphics when the lobby runs -- that call
// lives in D_DoomLoop, after everything here -- and HU_Init, which caches
// hu_font, comes after D_CheckNetGame. Drawing without either one paints into
// a NULL buffer through NULL patches. Bring both up now; the later calls
// repeat harmlessly, because I_InitGraphics only re-points I_VideoBuffer at
// the framebuffer that already exists and HU_Init re-caches lumps that are
// already resident.
static void EnsureVideo(void)
{
    if (I_VideoBuffer == NULL)
        I_InitGraphics();

    V_RestoreBuffer();

    if (hu_font[0] == NULL)
        HU_Init();

    // Without a palette the display driver has no colours to scan out and the
    // screen stays black however much is drawn into it. The game normally
    // gets here via ST_Init, which is also after D_CheckNetGame.
    I_SetPalette(W_CacheLumpName(DEH_String("PLAYPAL"), PU_CACHE));
}

static void Clear(void)
{
    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);
}

static void WriteCentred(int y, const char *text)
{
    char *s = (char *)text;     // M_WriteText predates const

    M_WriteText((SCREENWIDTH - M_StringWidth(s)) / 2, y, s);
}

// One frame of a screen that is nothing but centred lines. DG_DrawFrame
// clears the view area straight after presenting, so every frame is drawn
// from scratch rather than touched up.
static void Present(void)
{
    I_FinishUpdate();
}

static void DrawLobby(int cursor)
{
    int i;

    Clear();

    WriteCentred(24, "BADGE DOOM");
    WriteCentred(40, "-- CO-OP LOBBY --");

    for (i = 0; i < LOBBY_ITEMS; i++)
    {
        int y = 80 + i * 16;
        char *s = (char *)s_items[i];
        int x = (SCREENWIDTH - M_StringWidth(s)) / 2;

        M_WriteText(x, y, s);

        // A caret rather than the skull cursor: M_SKULL1 is patch art the
        // arena WAD does not carry.
        if (i == cursor)
            M_WriteText(x - 12, y, ">");
    }

    WriteCentred(150, "UP/DOWN TO MOVE");
    WriteCentred(164, "A TO CHOOSE");

    Present();
}

// A message screen held for a fixed time. Used for the pairing result, which
// nobody chooses to dismiss because nobody is watching for a prompt yet.
static void HoldMessage(const char *line1, const char *line2, int ms)
{
    uint32_t end = DG_GetTicksMs() + (uint32_t)ms;

    while ((int32_t)(DG_GetTicksMs() - end) < 0)
    {
        Clear();
        WriteCentred(88, line1);
        if (line2 != NULL)
            WriteCentred(108, line2);
        Present();
        DG_SleepMs(30);
    }
}

// ------------------------------------------------------------------- input

// Straight off DG_GetKey rather than through D_PostEvent: the event queue is
// drained by the game loop, which is not running yet, so anything posted here
// would sit in the queue and fire as soon as the level started.
static boolean PairProgress(int elapsed_ms);   // defined below, used by Run

static int PollKey(void)
{
    int pressed;
    unsigned char key;

    while (DG_GetKey(&pressed, &key))
    {
        if (pressed)
            return key;
    }

    return 0;
}

// ------------------------------------------------------------------- lobby

badge_net_role_t BadgeMenu_Run(void)
{
    int cursor = 0;
    badge_net_role_t role = BADGE_NET_OFF;
    boolean chosen = false;

    // A role already chosen before the engine started -- by a button held at
    // power-on, or by a BADGE_NET_FORCE_ROLE bench build -- is an answer to
    // the question this screen asks, so asking again would be wrong twice
    // over: the player would have to say it a second time, and a bench build
    // driven over USB has no hand available to say it at all.
    role = BadgeNet_RequestedRole();
    if (role != BADGE_NET_OFF)
    {
        printf("lobby: role already chosen before boot; skipping the menu\n");
        EnsureVideo();
        Clear();
        WriteCentred(88, role == BADGE_NET_HOST ? "WAITING FOR PLAYER 2..."
                                                : "LOOKING FOR A HOST...");
        WriteCentred(112, "UP TO 45 SECONDS");
        Present();
        return role;
    }

    EnsureVideo();

    while (!chosen)
    {
        int key;

        DrawLobby(cursor);

        while ((key = PollKey()) != 0)
        {
            switch (key)
            {
              case KEY_UPARROW:
                cursor = (cursor + LOBBY_ITEMS - 1) % LOBBY_ITEMS;
                break;

              case KEY_DOWNARROW:
                cursor = (cursor + 1) % LOBBY_ITEMS;
                break;

              case KEY_FIRE:
              case KEY_ENTER:
                role = s_roles[cursor];
                chosen = true;
                break;

              default:
                break;
            }
        }

        DG_SleepMs(30);
    }

    BadgeNet_RequestRole(role);
    BadgeNet_SetProgress(PairProgress);

    if (role == BADGE_NET_OFF)
        return role;

    // The last thing on screen before D_CheckNetGame blocks for up to ten
    // seconds inside BadgeNet_Pair(). Drawn here, not animated, because Pair()
    // does not come back until it is done and it is not ours to change.
    Clear();
    WriteCentred(88, role == BADGE_NET_HOST ? "WAITING FOR PLAYER 2..."
                                            : "LOOKING FOR A HOST...");
    WriteCentred(112, "UP TO 45 SECONDS");
    Present();

    return role;
}

// Repainted from inside BadgeNet_Pair's wait loop. Pairing owns the game task
// for its whole duration, so without this the badge shows one frozen frame for
// up to 45 seconds with no countdown, no sign it is alive and no way out --
// which is what made a perfectly working host look like it had given up after
// "a couple of seconds".
static boolean PairProgress(int elapsed_ms)
{
    static int last_shown = -1;
    int remaining = (BADGE_MENU_PAIR_SECONDS * 1000 - elapsed_ms + 999) / 1000;
    badge_net_role_t role = BadgeNet_RequestedRole();
    char line[40];

    if (remaining < 0)
        remaining = 0;

    // Only redraw on a second boundary. Repainting at loop rate would spend
    // the whole pairing budget in M_WriteText and starve the radio drain.
    if (remaining != last_shown)
    {
        last_shown = remaining;

        Clear();
        WriteCentred(72, role == BADGE_NET_HOST ? "WAITING FOR PLAYER 2"
                                                : "LOOKING FOR A HOST");
        M_snprintf(line, sizeof(line), "%d SECONDS LEFT", remaining);
        WriteCentred(96, line);
        WriteCentred(128, "PRESS B TO PLAY ALONE");
        Present();
    }

    // B gives up now rather than making the player wait out the timeout.
    {
        int pressed;
        unsigned char key;
        while (DG_GetKey(&pressed, &key))
        {
            if (pressed && key == KEY_USE)
                return false;
        }
    }

    return true;
}

void BadgeMenu_ShowPairResult(void)
{
    if (BadgeNet_RequestedRole() == BADGE_NET_OFF)
        return;

    EnsureVideo();

    if (BadgeNet_Active())
    {
        // Player numbers are 1-based here and 0-based everywhere in the
        // engine; this is the one place a human reads them.
        static char line[32];

        M_snprintf(line, sizeof(line), "PAIRED - YOU ARE PLAYER %d OF %d",
                   BadgeNet_ConsolePlayer() + 1, BADGE_NET_PLAYERS);
        HoldMessage(line, NULL, 2000);
    }
    else
    {
        // Each of these used to read "NO PARTNER FOUND", which told the player
        // nothing they could act on. A WAD mismatch in particular is something
        // they can fix, and it is indistinguishable from bad luck otherwise.
        switch (BadgeNet_FailReason())
        {
          case BADGE_NET_FAIL_WAD_MISMATCH:
            HoldMessage("WAD MISMATCH", "REFLASH BOTH BADGES", 3000);
            break;
          case BADGE_NET_FAIL_NO_RADIO:
            HoldMessage("RADIO DID NOT START", "1 PLAYER", 3000);
            break;
          case BADGE_NET_FAIL_NO_WAD:
            HoldMessage("NO WAD ON THIS BADGE", "1 PLAYER", 3000);
            break;
          case BADGE_NET_FAIL_CANCELLED:
            HoldMessage("CANCELLED", "1 PLAYER", 1200);
            break;
          default:
            HoldMessage("NO PARTNER FOUND", "1 PLAYER", 2000);
            break;
        }
    }
}
