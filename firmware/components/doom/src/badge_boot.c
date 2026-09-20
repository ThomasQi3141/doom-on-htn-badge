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
// Multiplayer is the connect screen: it lists the badges the radio can hear,
// offers a game to the one under the cursor, and once both sides agree it
// shows who is player 1 and how the link is doing. START there hands over to
// badge_net.c, which launches the same level on both badges and keeps them in
// lockstep from that tic on.
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
#include "m_misc.h"
#include "s_sound.h"
#include "sounds.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "hu_stuff.h"
#include "i_video.h"

#include <stdio.h>
#include "esp_app_desc.h"
#include "radio.h"
#include "badge_net.h"

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

// Connect screen: the peer list as of the last tic, and the cursor in it.
static radio_peer_t peers[RADIO_MAX_PEERS];
static int          peer_count;
static int          peer_on;

// How long a refusal message stays up before the list comes back.
#define MESSAGE_TICS (3 * TICRATE)
static int message_tic;

static uint32_t my_build_id;
static uint32_t my_wad_id;

// Who the outstanding offer went to: the list can reorder underneath it.
static char invited[RADIO_NAME_LEN];

static void Boot_ShowMenu(void)
{
    bootscreen = BOOT_MENU;
    skull_tic = SKULL_TICS;
    skull_frame = 0;
    radio_set_discoverable(false);
}

// What the other badge has to match before it may pair with us: this exact
// build, and a WAD with the same directory. The ELF hash changes with every
// link; the directory hash catches a different arena or a different IWAD.
static void Boot_SetRadioIdentity(void)
{
    static boolean done;
    if (done) return;
    done = true;

    uint32_t build_id;
    memcpy(&build_id, esp_app_get_description()->app_elf_sha256, sizeof build_id);

    // FNV-1a over each lump's name, position and size -- the fields, not
    // the hash-chain index, which depends on load order rather than content.
    uint32_t wad_id = 2166136261u;
    for (unsigned i = 0; i < numlumps; i++)
    {
        const unsigned char *p = (const unsigned char *)lumpinfo[i].name;
        for (int k = 0; k < 8; k++) { wad_id ^= p[k]; wad_id *= 16777619u; }
        const unsigned char *q = (const unsigned char *)&lumpinfo[i].position;
        for (int k = 0; k < 8; k++) { wad_id ^= q[k]; wad_id *= 16777619u; }
    }

    my_build_id = build_id;
    my_wad_id = wad_id;
    radio_set_identity(build_id, wad_id);
}

void Boot_Start(void)
{
    // End Game can land here straight out of a co-op level. Hang up before
    // anything else, so the other badge hears about it now rather than
    // discovering it two seconds later as a lost peer.
    if (BadgeNet_Active())
        BadgeNet_Drop("GAME ENDED");
    BadgeNet_Cancel();
    radio_disconnect();

    // Back to one player, so Singleplayer behaves as it always did and a
    // second co-op game starts from a clean slate.
    netgame = false;
    deathmatch = 0;
    // The offerer's settings are adopted wholesale by the accepter, so a
    // co-op game can leave this set on a badge that never chose it. Nothing
    // else on this build sets it -- D_DoomMain is handed no arguments.
    nomonsters = false;
    consoleplayer = displayplayer = 0;
    playeringame[0] = true;
    playeringame[1] = playeringame[2] = playeringame[3] = false;

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
    Boot_SetRadioIdentity();
    Boot_ShowMenu();
}

static void Boot_StartSingleplayer(void)
{
    bootscreen = BOOT_STARTING;
    G_DeferedInitNew(startskill, startepisode, startmap);
}

// Both badges leave the connect screen here, each with the seat the radio
// gave it and the settings the offerer chose. Everything the simulation needs
// to match is set before the level is asked for: the two badges then run the
// same code over the same WAD from the same tic, and only ticcmds cross the
// air.
static void Boot_StartCoop(void)
{
    radio_session_t sess;

    if (!radio_session(&sess))
    {
        // The link went away between the ticker and here.
        BadgeNet_Cancel();
        return;
    }

    bootscreen = BOOT_STARTING;
    radio_set_discoverable(false);

    netgame = true;
    deathmatch = sess.settings.deathmatch;
    nomonsters = sess.settings.nomonsters;
    consoleplayer = displayplayer = sess.player;
    playeringame[0] = playeringame[1] = true;
    playeringame[2] = playeringame[3] = false;

    BadgeNet_Begin(sess.player);

    // Not G_DeferedInitNew: that one resets every line above.
    G_DeferedInitNetGame((skill_t)sess.settings.skill,
                         sess.settings.episode, sess.settings.map);
}

static void Boot_StartConnect(void)
{
    bootscreen = BOOT_CONNECT;
    connect_tic = 0;
    peer_on = 0;
    message_tic = 0;
    radio_clear_refusal();
    radio_set_discoverable(true);
}

static void Boot_Offer(void)
{
    if (peer_count == 0) return;

    radio_settings_t s = {
        .skill      = (uint8_t)startskill,
        .episode    = (uint8_t)startepisode,
        .map        = (uint8_t)startmap,
        .deathmatch = (uint8_t)deathmatch,
        .nomonsters = (uint8_t)nomonsters,
    };

    if (radio_offer(peers[peer_on].mac, &s))
    {
        S_StartSound(NULL, sfx_pistol);
        M_StringCopy(invited, peers[peer_on].name, sizeof invited);
    }
    else if (radio_refusal() != RADIO_REFUSED_NONE)
    {
        // Incompatible: radio_offer has set the refusal for the drawer.
        S_StartSound(NULL, sfx_oof);
        message_tic = MESSAGE_TICS;
    }
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
    radio_state_t st = radio_state();

    switch (key)
    {
      case KEY_ESCAPE:
        S_StartSound(NULL, sfx_swtchx);
        BadgeNet_Cancel();
        if (st == RADIO_SCANNING || st == RADIO_OFF)
            Boot_ShowMenu();            // back out of the connect screen
        else
            radio_disconnect();         // cancel the offer, decline, or hang up
        return true;

      case KEY_UPARROW:
        if (st == RADIO_SCANNING && peer_count > 0)
        {
            peer_on = (peer_on + peer_count - 1) % peer_count;
            S_StartSound(NULL, sfx_pstop);
        }
        return true;

      case KEY_DOWNARROW:
        if (st == RADIO_SCANNING && peer_count > 0)
        {
            peer_on = (peer_on + 1) % peer_count;
            S_StartSound(NULL, sfx_pstop);
        }
        return true;

      case KEY_ENTER:
        if (st == RADIO_SCANNING)
            Boot_Offer();
        else if (st == RADIO_INCOMING)
        {
            S_StartSound(NULL, sfx_pistol);
            radio_accept();
        }
        else if (st == RADIO_CONNECTED && !BadgeNet_StartRequested())
        {
            // Either badge may start the game; the other one follows.
            S_StartSound(NULL, sfx_pistol);
            BadgeNet_RequestStart();
        }
        return true;
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

    // Nothing to press while a level is being started, and least of all HOME:
    // hanging up here would strand the other badge in a game it cannot leave.
    if (bootscreen == BOOT_STARTING)
        return true;

    return Boot_ConnectResponder(ev->data1);
}

void Boot_Finish(void)
{
    bootscreen = BOOT_NONE;
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

    if (bootscreen == BOOT_STARTING)
    {
        connect_tic++;      // keeps the ellipsis moving while we wait
        return;
    }

    if (bootscreen == BOOT_CONNECT)
    {
        connect_tic++;

        peer_count = radio_peers(peers, RADIO_MAX_PEERS);
        if (peer_on >= peer_count)
            peer_on = peer_count > 0 ? peer_count - 1 : 0;

        // A refusal that arrived over the air (declined, timed out, lost)
        // gets the same three seconds on screen as a local one.
        if (message_tic == 0 && radio_refusal() != RADIO_REFUSED_NONE)
            message_tic = MESSAGE_TICS;
        if (message_tic > 0 && --message_tic == 0)
            radio_clear_refusal();

        // The start handshake: either badge asking is enough, and both leave
        // the screen together.
        if (radio_state() != RADIO_CONNECTED)
            BadgeNet_Cancel();
        else if (BadgeNet_ConnectTicker())
            Boot_StartCoop();
    }
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

// Shown between asking for a level and having one. In singleplayer that is
// a single frame; in co-op it lasts until the other badge's first tic lands,
// which is the whole reason this screen has to exist.
static void Boot_DrawStarting(void)
{
    static const char *const dots[3] = { ".", "..", "..." };
    char line[40];

    V_DrawPatchDirect(LOGO_X, LOGO_Y,
                      W_CacheLumpName(DEH_String("M_DOOM"), PU_CACHE));

    snprintf(line, sizeof line, "STARTING%s", dots[(connect_tic / TICRATE) % 3]);
    Boot_DrawCentered(SCREENHEIGHT / 2, line);

    if (BadgeNet_Active())
        Boot_DrawCentered(SCREENHEIGHT - 24, "WAITING FOR THE OTHER BADGE");
}

static const char *Boot_RefusalText(radio_refusal_t why)
{
    switch (why)
    {
      case RADIO_REFUSED_DECLINED: return "THEY SAID NO";
      case RADIO_REFUSED_BUSY:     return "THAT BADGE IS BUSY";
      case RADIO_REFUSED_FIRMWARE: return "THAT BADGE RUNS DIFFERENT FIRMWARE";
      case RADIO_REFUSED_WAD:      return "THAT BADGE HAS A DIFFERENT WAD";
      case RADIO_REFUSED_TIMEOUT:  return "NO ANSWER FROM THAT BADGE";
      case RADIO_REFUSED_LOST:     return "LOST THE OTHER BADGE";
      default:                     return "";
    }
}

static void Boot_DrawSettings(int y, const radio_settings_t *s)
{
    char line[40];
    snprintf(line, sizeof line, "E%dM%d  SKILL %d  %s",
             s->episode, s->map, s->skill + 1,
             s->deathmatch ? "DEATHMATCH" : "CO-OP");
    Boot_DrawCentered(y, line);
}

static void Boot_DrawConnect(void)
{
    // A three-step ellipsis so the screen visibly ticks while it waits.
    static const char *const dots[3] = { ".", "..", "..." };
    const char *ell = dots[(connect_tic / TICRATE) % 3];
    int line_h = SHORT(hu_font[0]->height) + 4;
    int y = 48;
    char line[48];

    V_DrawPatchDirect(LOGO_X, LOGO_Y,
                      W_CacheLumpName(DEH_String("M_DOOM"), PU_CACHE));

    Boot_DrawCentered(y, "MULTIPLAYER");
    y += line_h * 2;

    radio_state_t st = radio_state();
    radio_session_t sess;
    radio_peer_t from;
    radio_settings_t proposed;

    switch (st)
    {
      case RADIO_OFF:
        Boot_DrawCentered(y, "THIS BADGE HAS NO RADIO");
        Boot_DrawCentered(SCREENHEIGHT - 24, "HOME TO GO BACK");
        break;

      case RADIO_SCANNING:
        if (message_tic > 0)
        {
            Boot_DrawCentered(y, Boot_RefusalText(radio_refusal()));
            y += line_h * 2;
        }

        if (peer_count == 0)
        {
            snprintf(line, sizeof line, "SEARCHING FOR A BADGE%s", ell);
            Boot_DrawCentered(y, line);
        }
        else
        {
            for (int i = 0; i < peer_count; i++)
            {
                const char *note = peers[i].compatible          ? "" :
                                   peers[i].build_id != my_build_id ? "  OTHER FIRMWARE"
                                                                    : "  OTHER WAD";
                snprintf(line, sizeof line, "%s%s", peers[i].name, note);
                M_WriteText(ITEM_X, y + i * LINE_H, line);
            }
            V_DrawPatchDirect(ITEM_X - SKULL_X_OFF,
                              y + peer_on * LINE_H + SKULL_Y_OFF,
                              W_CacheLumpName(DEH_String(skullName[skull_frame]),
                                              PU_CACHE));
        }

        snprintf(line, sizeof line, "YOU ARE %s", radio_name());
        Boot_DrawCentered(SCREENHEIGHT - 40, line);
        Boot_DrawCentered(SCREENHEIGHT - 24, peer_count > 0
                          ? "START TO INVITE, HOME TO GO BACK"
                          : "HOME TO GO BACK");
        break;

      case RADIO_OFFERING:
        snprintf(line, sizeof line, "WAITING FOR %s%s", invited, ell);
        Boot_DrawCentered(y, line);
        Boot_DrawCentered(SCREENHEIGHT - 24, "HOME TO CANCEL");
        break;

      case RADIO_INCOMING:
        if (radio_incoming(&from, &proposed))
        {
            snprintf(line, sizeof line, "%s WANTS TO PLAY", from.name);
            Boot_DrawCentered(y, line);
            Boot_DrawSettings(y + line_h * 2, &proposed);
        }
        Boot_DrawCentered(SCREENHEIGHT - 24, "START TO ACCEPT, HOME TO DECLINE");
        break;

      case RADIO_CONNECTED:
        if (radio_session(&sess))
        {
            radio_link_stats_t ls = radio_link_stats();

            snprintf(line, sizeof line, "CONNECTED TO %s", sess.peer.name);
            Boot_DrawCentered(y, line);
            y += line_h * 2;
            snprintf(line, sizeof line, "YOU ARE PLAYER %d", sess.player + 1);
            Boot_DrawCentered(y, line);
            y += line_h;
            Boot_DrawSettings(y, &sess.settings);
            y += line_h * 2;
            snprintf(line, sizeof line, "LINK %lu MS  LOSS %lu/%lu",
                     (unsigned long)((ls.rtt_avg_us + 500) / 1000),
                     (unsigned long)ls.lost, (unsigned long)ls.sent);
            Boot_DrawCentered(y, line);
        }
        if (BadgeNet_StartRequested())
        {
            snprintf(line, sizeof line, "STARTING%s", ell);
            Boot_DrawCentered(SCREENHEIGHT - 40, line);
        }
        else
        {
            Boot_DrawCentered(SCREENHEIGHT - 40, "START TO PLAY");
        }
        Boot_DrawCentered(SCREENHEIGHT - 24, "HOME TO DISCONNECT");
        break;
    }
}

void Boot_Drawer(void)
{
    if (bootscreen == BOOT_NONE)
        return;

    // Black ground, painted into the buffer the engine already owns.
    V_DrawFilledBox(0, 0, SCREENWIDTH, SCREENHEIGHT, 0);

    if (bootscreen == BOOT_MENU)
        Boot_DrawMenu();
    else if (bootscreen == BOOT_STARTING)
        Boot_DrawStarting();
    else
        Boot_DrawConnect();
}
