// Co-op between two badges: the glue between the radio, the lockstep tic
// exchange and Doom's main loop.
//
// lockstep.c owns the protocol and knows nothing else; this file is where it
// meets radio.c on one side and d_loop.c on the other. The division matters
// because the protocol is the part worth testing on a host (see
// tools/lockstep-test) and the glue is the part that can only be tested on
// two badges.

#ifndef BADGE_NET_H
#define BADGE_NET_H

#include "doomtype.h"
#include "d_ticcmd.h"

// True once both badges are running the same game. While it is false every
// call below is cheap and the engine behaves exactly as it did before.
boolean BadgeNet_Active(void);

// --- the connect screen -------------------------------------------------

// START was pressed on a connected badge: ask the other one to launch.
void BadgeNet_RequestStart(void);
boolean BadgeNet_StartRequested(void);

// Called once a tic from the connect screen while the radio has a session.
// Returns true when both badges should leave the screen and start playing.
boolean BadgeNet_ConnectTicker(void);

// Give up whatever is pending; the connect screen is going away.
void BadgeNet_Cancel(void);

// --- the game -----------------------------------------------------------

// Arm the lockstep session for our seat (0 or 1) and hand d_loop its
// player numbering. Called just before the level is started, on both badges.
void BadgeNet_Begin(int seat);

// Our cmd for `maketic`, straight from BuildNewTic.
void BadgeNet_SendTiccmd(ticcmd_t *cmd, int maketic);

// Move whatever has arrived into d_loop, and notice a lost peer. From
// NetUpdate, which the engine already calls several times a tic.
void BadgeNet_Run(void);

// Leave the co-op game and carry on alone. Safe to call more than once;
// `why` is shown to the player.
void BadgeNet_Drop(const char *why);

#endif
