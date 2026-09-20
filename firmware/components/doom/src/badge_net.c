// Co-op between two badges. See badge_net.h for how the pieces divide up.
//
// The shape of it:
//
//   BuildNewTic  -> BadgeNet_SendTiccmd -> lockstep_pack -> radio_send
//   radio_recv   -> lockstep_unpack     -> lockstep_next -> D_ReceiveTic
//
// d_loop.c then runs a tic only once it has both players' cmds for it, which
// is the whole of the synchronisation. Nothing about the game state crosses
// the air: both badges run the same code over the same WAD from the same
// start, and the radio's pairing already refused to connect two badges where
// either differs.
//
// Tic numbering is each badge's own maketic minus its own base, so the two
// sides do not have to agree a clock: the n-th tic of the session on one
// badge pairs with the n-th on the other, whatever the wall-clock offset
// between them, and neither simulation can get ahead of the other.

#include "badge_net.h"

#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_loop.h"
#include "i_timer.h"
#include "lockstep.h"
#include "net_defs.h"

#include "esp_log.h"
#include "radio.h"

static const char *TAG = "doom.net";

// lockstep.c sizes its frames to fit here; the radio is what decides how big
// "here" is, and the two headers cannot see each other.
_Static_assert(LOCKSTEP_MAX_FRAME <= RADIO_MAX_FRAME,
               "a tic frame does not fit in one radio frame");

static boolean    s_active;
static int        s_seat;           // our player number, 0 or 1
static int        s_peer_seat;
static int        s_base;           // maketic when the session started
static lockstep_t s_ls;

// The connect screen's start handshake.
static boolean s_start_requested;
static int     s_start_sent_ms;

// Retransmission while we are blocked: see BadgeNet_Run.
static int s_last_send_ms;
#define RESEND_MS   30          // about one tic
#define START_MS    100

// Reported once when the game ends, because the interesting numbers are the
// ones from a session that has just been played.
static void LogSession(void)
{
    radio_link_stats_t ls = radio_link_stats();
    ESP_LOGI(TAG, "session over: %lu tics run, frames rx %lu (stale %lu, dup %lu, "
                  "beyond window %lu, bad %lu), link rtt avg %lu us max %lu us, "
                  "pings lost %lu/%lu",
             (unsigned long)s_ls.delivered, (unsigned long)s_ls.rx_frames,
             (unsigned long)s_ls.rx_stale, (unsigned long)s_ls.rx_dup,
             (unsigned long)s_ls.rx_beyond, (unsigned long)s_ls.rx_bad,
             (unsigned long)ls.rtt_avg_us, (unsigned long)ls.rtt_max_us,
             (unsigned long)ls.lost, (unsigned long)ls.sent);
}

boolean BadgeNet_Active(void)
{
    return s_active;
}

// ------------------------------------------------------------ connect screen

void BadgeNet_RequestStart(void)
{
    s_start_requested = true;
    s_start_sent_ms = 0;        // send the first one on the next ticker
}

boolean BadgeNet_StartRequested(void)
{
    return s_start_requested;
}

void BadgeNet_Cancel(void)
{
    s_start_requested = false;
}

static void SendStart(void)
{
    uint8_t frame = LOCKSTEP_START;
    radio_send(&frame, sizeof frame);
    s_start_sent_ms = I_GetTimeMS();
}

boolean BadgeNet_ConnectTicker(void)
{
    uint8_t  frame[LOCKSTEP_MAX_FRAME];
    boolean  go = false;
    int      n;

    // Either badge may press START. The one that does leaves the screen at
    // once and begins sending tic frames; the other takes either the START
    // or those tic frames as its cue, so a single lost frame cannot leave one
    // badge in a level and the other still waiting on the connect screen.
    while ((n = radio_recv(frame, sizeof frame)) > 0)
    {
        // A tic frame here means the other badge is already playing and its
        // START never reached us. Its tics are dropped: it cannot advance
        // without ours, and it resends until we answer.
        if (frame[0] == LOCKSTEP_START || frame[0] == LOCKSTEP_TIC)
            go = true;
    }

    if (s_start_requested)
    {
        if (I_GetTimeMS() - s_start_sent_ms >= START_MS)
            SendStart();
        go = true;
    }
    else if (go)
    {
        // Answer, so a badge that asked first stops asking.
        SendStart();
    }

    return go;
}

// ------------------------------------------------------------ the game

void BadgeNet_Begin(int seat)
{
    uint8_t scratch[LOCKSTEP_MAX_FRAME];

    s_seat = seat;
    s_peer_seat = seat ^ 1;
    s_start_requested = false;
    lockstep_reset(&s_ls);

    // Anything still in the radio's ring belongs to the handshake, not to
    // tic 0 of this session.
    while (radio_recv(scratch, sizeof scratch) > 0)
        ;

    D_LockstepStart(seat);
    s_base = D_MakeTic();
    s_last_send_ms = I_GetTimeMS();
    s_active = true;

    ESP_LOGI(TAG, "co-op started, we are player %d, base tic %d",
             seat + 1, s_base);
}

void BadgeNet_SendTiccmd(ticcmd_t *cmd, int maketic)
{
    uint8_t frame[LOCKSTEP_MAX_FRAME];
    lockstep_cmd_t c;
    int len;

    if (!s_active)
        return;

    c.forwardmove = cmd->forwardmove;
    c.sidemove    = cmd->sidemove;
    c.angleturn   = cmd->angleturn;
    c.chatchar    = cmd->chatchar;
    c.buttons     = cmd->buttons;
    c.consistancy = cmd->consistancy;

    len = lockstep_pack(&s_ls, (uint32_t)(maketic - s_base), &c, frame);
    radio_send(frame, (size_t)len);
    s_last_send_ms = I_GetTimeMS();
}

static void DeliverTics(void)
{
    ticcmd_t cmds[NET_MAXPLAYERS];
    boolean  ingame[NET_MAXPLAYERS];
    lockstep_cmd_t c;

    // D_ReceiveTic writes into the slot for recvtic, which holds our own cmd
    // for that tic only once BuildNewTic has made it. Handing over a peer tic
    // we have not matched yet would pair it with a stale local cmd, so the
    // local side gates the exchange.
    while (D_RecvTic() < D_MakeTic() && lockstep_next(&s_ls, &c))
    {
        memset(cmds, 0, sizeof cmds);
        memset(ingame, 0, sizeof ingame);

        cmds[s_peer_seat].forwardmove = c.forwardmove;
        cmds[s_peer_seat].sidemove    = c.sidemove;
        cmds[s_peer_seat].angleturn   = c.angleturn;
        cmds[s_peer_seat].chatchar    = c.chatchar;
        cmds[s_peer_seat].buttons     = c.buttons;
        cmds[s_peer_seat].consistancy = c.consistancy;

        ingame[s_seat] = true;
        ingame[s_peer_seat] = true;

        D_ReceiveTic(cmds, ingame);
    }
}

void BadgeNet_Run(void)
{
    uint8_t frame[LOCKSTEP_MAX_FRAME];
    int n;

    if (!s_active)
        return;

    if (radio_state() != RADIO_CONNECTED)
    {
        BadgeNet_Drop("THE OTHER BADGE IS GONE");
        return;
    }

    while ((n = radio_recv(frame, sizeof frame)) > 0)
    {
        if (frame[0] == LOCKSTEP_TIC)
            lockstep_unpack(&s_ls, frame, n);
        // A late START from the other badge: it is already playing with us.
    }

    DeliverTics();

    // If the game is blocked waiting for the peer, our newest tics may be the
    // ones that went missing -- and while we are blocked BuildNewTic stops
    // making tics, so nothing new would be sent to replace them. Resend.
    if (D_RecvTic() >= D_MakeTic()
        && I_GetTimeMS() - s_last_send_ms >= RESEND_MS)
    {
        int len = lockstep_repack(&s_ls, frame);
        if (len > 0)
            radio_send(frame, (size_t)len);
        s_last_send_ms = I_GetTimeMS();
    }
}

void BadgeNet_Drop(const char *why)
{
    if (!s_active)
        return;

    s_active = false;
    LogSession();
    ESP_LOGW(TAG, "dropping to singleplayer: %s", why);

    // d_loop stops waiting for a second player at once, and RunTic sees the
    // seat leave the game the same way a quit in a real netgame looks: the
    // other player's "left the game" message, and their body left behind.
    D_LockstepStop();

    // netgame stays set deliberately. Clearing it would make G_DoReborn
    // reload the whole level on the next death instead of respawning at a
    // start, which is a jarring thing to do to someone in the middle of a
    // fight they did not know had become singleplayer.

    players[consoleplayer].message = (char *)why;
}
