// The boot lobby: the one screen that exists before a game does.
//
// Co-op used to be chosen by holding A or B during boot and confirmed by
// nothing at all -- the badge went black for ten seconds while pairing ran and
// came back as either one player or two, with no way to tell hosting from
// joining from failing. This is that decision made visible, and the only way
// to pick co-op once the engine is up.
//
// It is split in two because pairing is not ours to schedule: BadgeNet_Pair()
// runs deep inside D_CheckNetGame, so the choice must be made before that call
// and the outcome can only be reported after it.

#ifndef __BADGE_MENU__
#define __BADGE_MENU__

#include "badge_net.h"

// Must match PAIR_TIMEOUT_MS in badge_net.c; only used to draw the countdown.
#define BADGE_MENU_PAIR_SECONDS 45

// Draws the lobby and blocks until an item is chosen. Calls
// BadgeNet_RequestRole() with the answer, then leaves a "waiting for..."
// screen up so the ten seconds D_CheckNetGame is about to spend inside
// BadgeNet_Pair() are not ten seconds of black.
//
// Must be called before D_CheckNetGame. Brings up graphics, the palette and
// hu_font itself if D_DoomMain has not got to them yet.
badge_net_role_t BadgeMenu_Run(void);

// Reports what pairing decided and holds it on screen long enough to read.
// Call immediately after D_CheckNetGame. Does nothing when no role was asked
// for, so the single-player path still boots straight into the map.
void BadgeMenu_ShowPairResult(void);

// What the in-game lobby came back with.
typedef enum
{
    BADGE_MENU_RESUME = 0,   // carry on with the game that is running
    BADGE_MENU_SOLO,
    BADGE_MENU_HOST,
    BADGE_MENU_JOIN,
    BADGE_MENU_DOOM,         // hand over to Doom's own menu
} badge_menu_choice_t;

// Draws the in-game lobby and blocks until the player chooses.
badge_menu_choice_t BadgeMenu_RunInGame(void);

// Asks for the lobby. Called from the event handler, which cannot tear a game
// down where it stands; BadgeMenu_Service does the work at the top of a frame.
void BadgeMenu_Request(void);
void BadgeMenu_Service(void);

#endif
