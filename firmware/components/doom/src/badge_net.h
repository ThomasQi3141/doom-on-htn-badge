// Two-badge co-op: the part that knows about Doom.
//
// badge_radio.c (in the badge component) carries bytes. This carries tics.
//
// The model is host-authoritative lockstep. Each badge builds its own ticcmd,
// the host decides which inputs land in which tic and echoes both players'
// inputs back, and both badges simulate that tic. Doom is deterministic given
// identical inputs, so they stay in step by construction. There is no
// prediction and no rollback -- on hardware this slow, a mispredicted tic
// costs more to re-simulate than it could ever save.

#ifndef __BADGE_NET__
#define __BADGE_NET__

#include "doomtype.h"
#include "d_ticcmd.h"
#include "net_defs.h"

// Bumped whenever the wire format changes. Two badges with different values
// refuse to pair rather than desync in a way that looks like a game bug.
#define BADGE_NET_PROTO 1

#define BADGE_NET_PLAYERS 2

// How many tics each packet carries. Every send repeats the last few tics, so
// one lost frame is repaired by the next packet rather than by a retransmit
// request -- a round trip costs a whole render period on each side, and at
// 23 fps that is far more expensive than 32 spare bytes.
#define BADGE_NET_TICS_PER_PACKET 4

typedef enum
{
    BADGE_NET_OFF = 0,   // single player, exactly as before
    BADGE_NET_HOST,      // player 0; owns the tic schedule
    BADGE_NET_CLIENT,    // player 1
} badge_net_role_t;

// Ask for a role. Called before D_DoomMain, from whatever chooses the mode --
// today the boot buttons, later the lobby screen.
void BadgeNet_RequestRole(badge_net_role_t role);
badge_net_role_t BadgeNet_RequestedRole(void);

// Runs the pairing handshake if a role was requested. Returns false when no
// role was asked for, the radio would not start, the peer disagreed about the
// WAD, or nobody answered in time -- in every one of those cases the caller
// falls back to single player. Bounded: it never blocks longer than the
// timeout below.
//
// Called from D_StartNetGame, which is late enough that the WAD is mapped and
// its directory can be hashed, and early enough that the answer still shapes
// the game settings.
boolean BadgeNet_Pair(void);

// True once paired and still hearing from the peer.
boolean BadgeNet_Active(void);
boolean BadgeNet_IsHost(void);

// 0 for the host, 1 for the client, 0 when not paired.
int BadgeNet_ConsolePlayer(void);

// The peer's identity, for logging. NULL when not paired.
const uint8_t *BadgeNet_PeerMac(void);

// ---------------------------------------------------------------- tic exchange
//
// The contract between d_loop.c and badge_net.c. Both sides of it are written
// independently, so it is fixed here first.

// Called from BuildNewTic once a local ticcmd exists for `tic`, before it is
// stored into ticdata[]. The host records it as player 0; the client
// broadcasts it. Never blocks.
void BadgeNet_SendTiccmd(ticcmd_t *cmd, int tic);

// Called from NetUpdate. Drains the radio, merges what has arrived, and calls
// D_ReceiveTic() zero or more times -- always in ascending tic order with no
// gaps, because D_ReceiveTic carries no tic number and blindly increments
// recvtic. Also owns the peer-timeout check. Never blocks.
void BadgeNet_Run(void);

// Called from D_QuitNetGame.
void BadgeNet_Shutdown(void);

// Diagnostics for the once-a-second bring-up log.
int  BadgeNet_LastRecvTic(void);
int  BadgeNet_StallTics(void);     // tics spent waiting on the peer

// Fills in the parts of `settings` that pairing decided: num_players,
// consoleplayer, and -- on the client -- the skill, episode and map the host
// chose. Safe to call unpaired, in which case it sets the single-player
// values the engine used before any of this existed.
void BadgeNet_FillSettings(net_gamesettings_t *settings);

#endif
