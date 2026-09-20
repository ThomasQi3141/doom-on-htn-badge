// Two-badge lockstep: the tic exchange, with no radio and no engine in it.
//
// Both badges run the same simulation from the same starting state, so the
// only thing that has to cross the air is each player's ticcmd for each tic.
// A tic runs when both cmds for it are in hand -- that is the whole protocol.
//
// The transport underneath (ESP-NOW) is datagrams: they can be lost,
// duplicated or reordered, and there are no acknowledgements. So:
//
//   - every frame carries several consecutive tics, not just the newest, and
//     a burst of lost frames inside that span costs nothing at all;
//   - every frame also says which of the other badge's tics the sender is
//     still waiting for, which is the only acknowledgement there is. It is
//     what a retransmission aims at: once a badge has run a few tics ahead,
//     the tic its peer is stuck on is no longer among the newest ones, and
//     resending the newest would never unstick it.
//
// This file is deliberately free of ESP-IDF and of Doom: it is plain C over a
// byte buffer, so tools/lockstep-test can run two of these against a lossy
// channel on the host for the ten minutes the ticket asks about.

#ifndef BADGE_LOCKSTEP_H
#define BADGE_LOCKSTEP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Frames must fit RADIO_MAX_FRAME. Three tics is 31 bytes.
#define LOCKSTEP_SPAN       3       // tics carried by one frame
#define LOCKSTEP_WINDOW     16      // peer tics held out of order
#define LOCKSTEP_HISTORY    16      // our tics kept for retransmission
#define LOCKSTEP_MAX_FRAME  32

// Frame types on the radio's data channel.
enum
{
    LOCKSTEP_TIC   = 1,     // the tic frame this file packs and unpacks
    LOCKSTEP_START = 2,     // "leave the connect screen and play"
};

// The parts of Doom's ticcmd_t that a co-op game has to agree on. The rest of
// ticcmd_t is Strife and Heretic baggage the engine never fills in here.
typedef struct
{
    int8_t   forwardmove;
    int8_t   sidemove;
    int16_t  angleturn;
    uint8_t  chatchar;
    uint8_t  buttons;
    uint8_t  consistancy;
} lockstep_cmd_t;

#define LOCKSTEP_CMD_BYTES  7
#define LOCKSTEP_HDR_BYTES  10

typedef struct
{
    // Our tics, by index, for retransmission.
    lockstep_cmd_t history[LOCKSTEP_HISTORY];
    uint32_t       hist_first;      // oldest tic still held
    uint32_t       hist_next;       // one past the newest tic held
    uint32_t       peer_need;       // the oldest of ours the peer is missing

    // The peer's tics, held by index until the game is ready for them.
    lockstep_cmd_t window[LOCKSTEP_WINDOW];
    bool           have[LOCKSTEP_WINDOW];

    uint32_t next;                  // next peer tic the game wants
    uint32_t highest;               // one past the highest peer tic seen

    // Counters, for the link report and for the host test to assert on.
    uint32_t rx_frames;
    uint32_t rx_bad;
    uint32_t rx_stale;              // a tic we had already run
    uint32_t rx_dup;                // a tic already in the window
    uint32_t rx_beyond;             // past the window: would have been a gap
    uint32_t delivered;
} lockstep_t;

void lockstep_reset(lockstep_t *ls);

// Record our cmd for `tic` and write the frame to send. `out` must have room
// for LOCKSTEP_MAX_FRAME bytes; returns the number written.
//
// Tics must be recorded in order, one at a time. Out of order or with a gap,
// the history is restarted from `tic` rather than sending a frame that claims
// tics it does not hold.
int lockstep_pack(lockstep_t *ls, uint32_t tic, const lockstep_cmd_t *cmd,
                  void *out);

// Build a frame from the history alone -- the retransmission that breaks a
// stall where both sides are waiting and neither is making tics. Returns 0
// when there is nothing held to resend.
int lockstep_repack(const lockstep_t *ls, void *out);

// Take a frame off the air. False if it is not a well-formed tic frame.
bool lockstep_unpack(lockstep_t *ls, const void *in, int len);

// The peer's cmd for the next tic, if it has arrived. Tics come out strictly
// in order and exactly once.
bool lockstep_next(lockstep_t *ls, lockstep_cmd_t *out);


#endif
