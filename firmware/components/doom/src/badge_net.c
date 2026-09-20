#include "badge_net.h"

#include <string.h>

#include "badge_radio.h"
#include "d_loop.h"
#include "doomdef.h"
#include "doomstat.h"
#include "i_timer.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "badge_net";

// The wire format below is written and read byte for byte on both badges, so
// it depends on ticcmd_t being exactly what d_ticcmd.h says it is. That header
// was trimmed for this port (the Strife and Heretic fields are gone), and if
// anyone puts them back this must be reconsidered rather than silently
// shipping two incompatible builds.
_Static_assert(sizeof(ticcmd_t) == 8, "ticcmd_t is the wire format; see d_ticcmd.h");

uint32_t Badge_WadIdentity(void);   // badge_platform.c

#ifdef BADGE_NET_LOOPBACK
// g_game.c defines this and no header in this tree declares it.
extern byte consistancy[MAXPLAYERS][BACKUPTICS];
#endif

// ---------------------------------------------------------------- wire format

typedef struct __attribute__((packed)) {
    uint8_t  proto;
    uint8_t  max_players;
    uint32_t wad_id;
    uint32_t token;        // identifies this session; random per host boot
} badge_net_announce_t;

typedef struct __attribute__((packed)) {
    uint8_t  proto;
    uint32_t wad_id;
    uint32_t token;        // echoed from the ANNOUNCE being answered
} badge_net_join_t;

typedef struct __attribute__((packed)) {
    uint8_t  proto;
    uint32_t token;
    uint32_t wad_id;
    uint8_t  num_players;
    uint8_t  consoleplayer;   // the recipient's player number, so always 1
    uint8_t  deathmatch;
    uint8_t  skill;
    uint8_t  episode;
    uint8_t  map;
    uint8_t  nomonsters;
    uint8_t  fast_monsters;
    uint8_t  respawn_monsters;
    uint8_t  ticdup;
} badge_net_start_t;

typedef struct __attribute__((packed)) {
    uint32_t token;        // the session this belongs to; stale frames die here
    int32_t  first_tic;
    uint8_t  count;
    ticcmd_t cmds[BADGE_NET_TICS_PER_PACKET];
} badge_net_ticcmd_msg_t;

typedef struct __attribute__((packed)) {
    uint32_t token;
    int32_t  first_tic;
    uint8_t  count;
    uint8_t  ingame_mask;  // reserved: today always both players, see Step 5
    ticcmd_t cmds[BADGE_NET_TICS_PER_PACKET][BADGE_NET_PLAYERS];
} badge_net_ticset_msg_t;

// These two numbers are the protocol. A compiler that pads them would put one
// badge's ticcmds at a different offset from the other's, and the game would
// desync on the first frame with no hint as to why.
_Static_assert(sizeof(badge_net_ticcmd_msg_t) == 41, "TICCMD is the wire format");
_Static_assert(sizeof(badge_net_ticset_msg_t) == 74, "TICSET is the wire format");

// ---------------------------------------------------------------- state

#define PAIR_TIMEOUT_MS      10000   // how long a badge waits for a partner
#define ANNOUNCE_PERIOD_MS     200   // host repeats itself this often
#define START_REPEATS            3   // START is unicast, but loss is cheap to cover

static badge_net_role_t   s_requested = BADGE_NET_OFF;
static boolean            s_paired;
static boolean            s_is_host;
static uint32_t           s_token;
static uint8_t            s_peer[6];
static badge_net_start_t  s_start;      // the client's copy of the host's terms

// The engine expects these. They used to live in badge_stubs.c; they belong
// here now because this file is the only thing that ever changes them.
//
// A drone is a badge that joins a game without a player attached. There is no
// such badge: every badge has a screen and eight buttons. Leaving it false
// keeps BuildNewTic generating ticcmds and keeps D_ReceiveTic's
// "don't overwrite us" branch live, which the no-substitution rule below
// depends on absolutely.
boolean drone = false;
boolean net_client_connected = false;

// ---------------------------------------------------------------- tic ring
//
// One slot per tic in flight. The slot carries its own absolute tic number, so
// a frame that arrives twice or out of order lands in the same place with the
// same contents and costs nothing -- that is what lets tic traffic go out as
// unacknowledged broadcast instead of ESP-NOW unicast, whose up-to-seven
// retries spike 20-50 ms and would blow the frame budget on this CPU.

typedef struct {
    int32_t  tic;
    ticcmd_t cmd[BADGE_NET_PLAYERS];
    uint8_t  have;          // bit per player; 0x3 means the tic is complete
} slot_t;

#define HAVE_ALL ((1u << BADGE_NET_PLAYERS) - 1u)

static slot_t s_ring[BACKUPTICS];

// Must track d_loop's recvtic exactly: D_ReceiveTic carries no tic number and
// blindly increments recvtic, so this counter is the only thing that knows
// which tic the next call will land on.
static int32_t s_next_recv;

static int32_t s_maketic;        // one past the newest tic we built locally
static int64_t s_last_rx_us;     // last frame accepted from the peer
static int64_t s_last_tx_us;
static int     s_stall_tics;
static boolean s_peer_lost;

#define PEER_TIMEOUT_MS   2000   // silence this long and the peer is gone
#define KEEPALIVE_MS       100   // resend even with nothing new, so a stalled
                                 // pair does not mistake its own stall for a
                                 // disconnect and drop the other badge

// Where the header ends and our payload begins in a received frame.
#define PAYLOAD(pkt)     ((pkt).data + sizeof(badge_radio_hdr_t))
#define PAYLOAD_LEN(pkt) ((size_t)(pkt).len - sizeof(badge_radio_hdr_t))

static uint8_t frame_type(const badge_radio_packet_t *pkt)
{
    badge_radio_hdr_t hdr;
    memcpy(&hdr, pkt->data, sizeof(hdr));
    return hdr.type;
}

// True when the frame is the type we want and carries a whole payload. Short
// frames are the shape a corrupted or foreign packet takes, and reading a
// struct out of one would be reading someone else's memory.
static boolean frame_is(const badge_radio_packet_t *pkt, badge_msg_type_t type,
                        size_t payload_len)
{
    return pkt->len >= sizeof(badge_radio_hdr_t) + payload_len
        && frame_type(pkt) == (uint8_t)type;
}

static void log_mac(const char *what, const uint8_t mac[6])
{
    ESP_LOGI(TAG, "%s %02x:%02x:%02x:%02x:%02x:%02x",
             what, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---------------------------------------------------------------- role

void BadgeNet_RequestRole(badge_net_role_t role)
{
    s_requested = role;
}

badge_net_role_t BadgeNet_RequestedRole(void)
{
#ifdef BADGE_NET_LOOPBACK
    // The harness is a two-player game with one badge in it, so the menu never
    // gets a say: D_InitNetGame asks this before any button has been read.
    return BADGE_NET_HOST;
#else
    return s_requested;
#endif
}

boolean BadgeNet_Active(void)   { return s_paired; }
boolean BadgeNet_IsHost(void)   { return s_paired && s_is_host; }
int     BadgeNet_ConsolePlayer(void) { return (s_paired && !s_is_host) ? 1 : 0; }
const uint8_t *BadgeNet_PeerMac(void) { return s_paired ? s_peer : NULL; }

// ---------------------------------------------------------------- handshake

// Host: broadcast ANNOUNCE until someone JOINs with a matching WAD, then
// unicast START and latch them as the peer.
static boolean RunHost(uint32_t wad_id)
{
    badge_net_announce_t ann = {
        .proto       = BADGE_NET_PROTO,
        .max_players = BADGE_NET_PLAYERS,
        .wad_id      = wad_id,
        .token       = s_token,
    };

    int64_t deadline = esp_timer_get_time() + (int64_t)PAIR_TIMEOUT_MS * 1000;
    int64_t next_announce = 0;

    ESP_LOGI(TAG, "hosting: waiting up to %d ms for a partner", PAIR_TIMEOUT_MS);

    while (esp_timer_get_time() < deadline)
    {
        int64_t now = esp_timer_get_time();
        if (now >= next_announce)
        {
            next_announce = now + (int64_t)ANNOUNCE_PERIOD_MS * 1000;
            badge_radio_send(BADGE_MSG_ANNOUNCE, &ann, sizeof(ann));
        }

        badge_radio_packet_t pkt;
        if (!badge_radio_recv(&pkt, 20))
            continue;

        if (!frame_is(&pkt, BADGE_MSG_JOIN, sizeof(badge_net_join_t)))
            continue;

        badge_net_join_t join;
        memcpy(&join, PAYLOAD(pkt), sizeof(join));

        if (join.proto != BADGE_NET_PROTO)
        {
            ESP_LOGW(TAG, "ignoring JOIN: protocol %u, we speak %u",
                     join.proto, BADGE_NET_PROTO);
            continue;
        }
        if (join.token != s_token)
            continue;                    // an older session, or not for us
        if (join.wad_id != wad_id)
        {
            // Refusing here is the whole point of the WAD id: two different
            // arenas would desync within seconds and look like a netcode bug.
            ESP_LOGE(TAG, "refusing JOIN: WAD 0x%08x, ours is 0x%08x -- "
                          "reflash both badges from the same doom-arena.wad",
                     (unsigned)join.wad_id, (unsigned)wad_id);
            continue;
        }

        memcpy(s_peer, pkt.mac, 6);
        badge_radio_add_peer(s_peer);

        // The host's globals are already the ones D_CheckNetGame saved, so
        // these are the terms both badges will play under.
        s_start = (badge_net_start_t){
            .proto            = BADGE_NET_PROTO,
            .token            = s_token,
            .wad_id           = wad_id,
            .num_players      = BADGE_NET_PLAYERS,
            .consoleplayer    = 1,          // the recipient is player 2
            .deathmatch       = 0,          // co-op; the arena has no DM starts
            .skill            = (uint8_t)startskill,
            .episode          = (uint8_t)startepisode,
            .map              = (uint8_t)startmap,
            .nomonsters       = (uint8_t)nomonsters,
            .fast_monsters    = (uint8_t)fastparm,
            .respawn_monsters = (uint8_t)respawnparm,
            .ticdup           = 1,
        };

        // Unicast, and repeated: this is the one message whose loss would
        // strand the client waiting while the host walks off into the game.
        for (int i = 0; i < START_REPEATS; i++)
            badge_radio_send_to(s_peer, BADGE_MSG_START, &s_start, sizeof(s_start));

        log_mac("paired with", s_peer);
        return true;
    }

    ESP_LOGW(TAG, "nobody joined; falling back to single player");
    return false;
}

// Client: listen for an ANNOUNCE, answer it, and wait for the host's terms.
static boolean RunClient(uint32_t wad_id)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)PAIR_TIMEOUT_MS * 1000;

    ESP_LOGI(TAG, "joining: listening up to %d ms for a host", PAIR_TIMEOUT_MS);

    while (esp_timer_get_time() < deadline)
    {
        badge_radio_packet_t pkt;
        if (!badge_radio_recv(&pkt, 50))
            continue;

        if (!frame_is(&pkt, BADGE_MSG_ANNOUNCE, sizeof(badge_net_announce_t)))
            continue;

        badge_net_announce_t ann;
        memcpy(&ann, PAYLOAD(pkt), sizeof(ann));

        if (ann.proto != BADGE_NET_PROTO)
        {
            ESP_LOGW(TAG, "ignoring host: protocol %u, we speak %u",
                     ann.proto, BADGE_NET_PROTO);
            continue;
        }
        if (ann.wad_id != wad_id)
        {
            ESP_LOGE(TAG, "host has WAD 0x%08x, ours is 0x%08x -- "
                          "reflash both badges from the same doom-arena.wad",
                     (unsigned)ann.wad_id, (unsigned)wad_id);
            continue;
        }

        memcpy(s_peer, pkt.mac, 6);
        badge_radio_add_peer(s_peer);
        s_token = ann.token;

        badge_net_join_t join = {
            .proto  = BADGE_NET_PROTO,
            .wad_id = wad_id,
            .token  = s_token,
        };

        // Answer, then wait for START. The host repeats START, and repeats
        // ANNOUNCE too, so a lost JOIN just means going round again.
        badge_radio_send_to(s_peer, BADGE_MSG_JOIN, &join, sizeof(join));

        int64_t start_deadline = esp_timer_get_time() + 500 * 1000;
        while (esp_timer_get_time() < start_deadline)
        {
            badge_radio_packet_t rep;
            if (!badge_radio_recv(&rep, 20))
                continue;
            if (!frame_is(&rep, BADGE_MSG_START, sizeof(badge_net_start_t)))
                continue;

            badge_net_start_t st;
            memcpy(&st, PAYLOAD(rep), sizeof(st));
            if (st.token != s_token || st.wad_id != wad_id)
                continue;

            s_start = st;
            log_mac("paired with", s_peer);
            return true;
        }

        ESP_LOGW(TAG, "host went quiet after JOIN; listening again");
    }

    ESP_LOGW(TAG, "no host answered; falling back to single player");
    return false;
}

boolean BadgeNet_Pair(void)
{
    // The ring outlives a failed pairing attempt, and a stale tic stamp would
    // be read as a real tic by the next session.
    memset(s_ring, 0, sizeof(s_ring));
    for (int i = 0; i < BACKUPTICS; i++)
        s_ring[i].tic = -1;
    s_next_recv  = 0;
    s_maketic    = 0;
    s_stall_tics = 0;
    s_peer_lost  = false;
    s_last_rx_us = s_last_tx_us = esp_timer_get_time();

#ifdef BADGE_NET_LOOPBACK
    // No radio, no peer, no handshake -- just the two-player plumbing, so that
    // a desync found here is a bug in the ring or the drain and cannot be
    // blamed on a lost frame.
    s_is_host = true;
    s_paired  = true;
    s_token   = 0;
    ESP_LOGW(TAG, "BADGE_NET_LOOPBACK: player 2 mirrors player 1, radio off");
    return true;
#else
    if (s_requested == BADGE_NET_OFF)
        return false;

    if (!badge_radio_ready())
    {
        ESP_LOGE(TAG, "radio is not up; single player");
        return false;
    }

    uint32_t wad_id = Badge_WadIdentity();
    if (wad_id == 0)
    {
        ESP_LOGE(TAG, "no WAD identity; refusing to pair blind");
        return false;
    }
    ESP_LOGI(TAG, "WAD id 0x%08x", (unsigned)wad_id);

    badge_radio_flush();          // anything queued predates this session

    s_is_host = (s_requested == BADGE_NET_HOST);
    if (s_is_host)
        s_token = esp_random();

    s_paired = s_is_host ? RunHost(wad_id) : RunClient(wad_id);

    if (s_paired)
    {
        ESP_LOGI(TAG, "paired, I am player %d of %d",
                 BadgeNet_ConsolePlayer() + 1, BADGE_NET_PLAYERS);
        s_last_rx_us = s_last_tx_us = esp_timer_get_time();
    }

    return s_paired;
#endif
}

// ---------------------------------------------------------------- settings

void BadgeNet_FillSettings(net_gamesettings_t *settings)
{
    if (!s_paired)
    {
        settings->num_players   = 1;
        settings->consoleplayer = 0;
        return;
    }

    settings->num_players   = BADGE_NET_PLAYERS;
    settings->consoleplayer = BadgeNet_ConsolePlayer();

    // The host keeps the globals D_CheckNetGame already saved. The client
    // takes the host's, so that LoadGameSettings pushes them into startskill
    // and friends before d_main calls G_InitNew -- which is the whole reason
    // this runs inside D_StartNetGame rather than later.
    if (!s_is_host)
    {
        settings->skill            = s_start.skill;
        settings->episode          = s_start.episode;
        settings->map              = s_start.map;
        settings->deathmatch       = s_start.deathmatch;
        settings->nomonsters       = s_start.nomonsters;
        settings->fast_monsters    = s_start.fast_monsters;
        settings->respawn_monsters = s_start.respawn_monsters;
    }
}

// ---------------------------------------------------------------- tic exchange

static slot_t *SlotFor(int32_t tic)
{
    slot_t *s = &s_ring[(uint32_t)tic % BACKUPTICS];

    // A slot whose stamp is some older tic is a slot the ring has wrapped past.
    // Reusing it without clearing would hand the engine last lap's input.
    if (s->tic != tic)
    {
        memset(s, 0, sizeof(*s));
        s->tic = tic;
    }
    return s;
}

// Tics outside this window cannot be used: below it they have already been
// handed to the engine, above it they would overwrite a slot still in flight.
static boolean TicInWindow(int32_t tic)
{
    return tic >= s_next_recv && tic < s_next_recv + BACKUPTICS;
}

static void Deposit(int32_t tic, int player, const ticcmd_t *cmd)
{
    slot_t *s;

    if (!TicInWindow(tic))
        return;

    s = SlotFor(tic);
    s->cmd[player] = *cmd;
    s->have |= (uint8_t)(1u << player);
}

// Hands completed tics to the engine in strict ascending order and steps the
// counter that mirrors recvtic. This is the ONLY place D_ReceiveTic is called
// while the peer is alive: it carries no tic number, so a second caller would
// silently attribute its cmds to the wrong tic.
static void DeliverComplete(void)
{
    while (1)
    {
        slot_t *s = &s_ring[(uint32_t)s_next_recv % BACKUPTICS];
        ticcmd_t cmds[NET_MAXPLAYERS];
        boolean  ingame[NET_MAXPLAYERS];
        int i;

        if (s->tic != s_next_recv || s->have != HAVE_ALL)
        {
            // Only count it as a stall when the peer is what we are waiting on.
            // Waiting because we have not built that far ourselves is just the
            // frame rate, and logging it as a stall would hide the real thing.
            if (s_next_recv < s_maketic)
                s_stall_tics++;
            return;
        }

        memset(cmds, 0, sizeof(cmds));
        for (i = 0; i < NET_MAXPLAYERS; i++)
            ingame[i] = (i < BADGE_NET_PLAYERS);
        for (i = 0; i < BADGE_NET_PLAYERS; i++)
            cmds[i] = s->cmd[i];

        D_ReceiveTic(cmds, ingame);
        s_next_recv++;
    }
}

#ifndef BADGE_NET_LOOPBACK

// Both directions send the newest up-to-four tics every time, so a dropped
// frame is repaired by the next one. Asking for a retransmit would cost a
// round trip, and a round trip here is a whole render period on both badges.
static int32_t PacketStart(int32_t last_tic, uint8_t *count)
{
    int32_t first = last_tic - (BADGE_NET_TICS_PER_PACKET - 1);

    if (first < 0)
        first = 0;

    *count = (uint8_t)(last_tic - first + 1);
    return first;
}

static void ClientSendTiccmd(void)
{
    badge_net_ticcmd_msg_t msg;
    int32_t first, t;
    uint8_t count = 0;
    int n;

    if (s_maketic <= 0)
        return;

    first = PacketStart(s_maketic - 1, &count);

    memset(&msg, 0, sizeof(msg));
    msg.token     = s_token;
    msg.first_tic = first;

    n = 0;
    for (t = first; t < first + count; t++)
    {
        slot_t *s = &s_ring[(uint32_t)t % BACKUPTICS];

        // Stop at the first hole rather than skipping it: the receiver reads
        // cmds[i] as first_tic + i, so a gap would shift every later cmd onto
        // the wrong tic.
        if (s->tic != t || !(s->have & 0x2))
            break;
        msg.cmds[n++] = s->cmd[1];
    }
    if (n == 0)
        return;

    msg.count = (uint8_t)n;
    badge_radio_send(BADGE_MSG_TICCMD, &msg, sizeof(msg));
    s_last_tx_us = esp_timer_get_time();
}

static void HostSendTicset(void)
{
    badge_net_ticset_msg_t msg;
    int32_t first, t;
    uint8_t count = 0;
    int n, p;

    if (s_next_recv <= 0)
        return;

    first = PacketStart(s_next_recv - 1, &count);

    memset(&msg, 0, sizeof(msg));
    msg.token       = s_token;
    msg.first_tic   = first;
    msg.ingame_mask = HAVE_ALL;

    n = 0;
    for (t = first; t < first + count; t++)
    {
        slot_t *s = &s_ring[(uint32_t)t % BACKUPTICS];

        if (s->tic != t || s->have != HAVE_ALL)
            break;
        for (p = 0; p < BADGE_NET_PLAYERS; p++)
            msg.cmds[n][p] = s->cmd[p];
        n++;
    }
    if (n == 0)
        return;

    msg.count = (uint8_t)n;
    badge_radio_send(BADGE_MSG_TICSET, &msg, sizeof(msg));
    s_last_tx_us = esp_timer_get_time();
}

// Only frames from the badge we paired with. A third badge running its own
// game on the same channel is otherwise perfectly capable of feeding us
// ticcmds, and its tic numbering would have nothing to do with ours.
static boolean FromPeer(const badge_radio_packet_t *pkt)
{
    return memcmp(pkt->mac, s_peer, 6) == 0;
}

static void DrainRadio(void)
{
    badge_radio_packet_t pkt;

    while (badge_radio_recv(&pkt, 0))
    {
        if (!FromPeer(&pkt))
            continue;

        if (s_is_host && frame_is(&pkt, BADGE_MSG_TICCMD, sizeof(badge_net_ticcmd_msg_t)))
        {
            badge_net_ticcmd_msg_t msg;
            int i;

            memcpy(&msg, PAYLOAD(pkt), sizeof(msg));
            if (msg.token != s_token || msg.count == 0
             || msg.count > BADGE_NET_TICS_PER_PACKET)
                continue;

            // Copied out first: msg is packed, so &msg.cmds[i] is an
            // unaligned pointer and the C3 traps on some unaligned loads.
            for (i = 0; i < msg.count; i++)
            {
                ticcmd_t cmd;
                memcpy(&cmd, &msg.cmds[i], sizeof(cmd));
                Deposit(msg.first_tic + i, 1, &cmd);
            }

            s_last_rx_us = esp_timer_get_time();
        }
        else if (!s_is_host && frame_is(&pkt, BADGE_MSG_TICSET, sizeof(badge_net_ticset_msg_t)))
        {
            badge_net_ticset_msg_t msg;
            int i, p;

            memcpy(&msg, PAYLOAD(pkt), sizeof(msg));
            if (msg.token != s_token || msg.count == 0
             || msg.count > BADGE_NET_TICS_PER_PACKET)
                continue;

            for (i = 0; i < msg.count; i++)
                for (p = 0; p < BADGE_NET_PLAYERS; p++)
                {
                    ticcmd_t cmd;
                    memcpy(&cmd, &msg.cmds[i][p], sizeof(cmd));
                    Deposit(msg.first_tic + i, p, &cmd);
                }

            s_last_rx_us = esp_timer_get_time();
        }
    }
}

// Feeds tics forward with the peer marked absent and its cmd zeroed. This is
// the one place zero-filling is correct: the peer is gone, not late, so there
// is no longer another simulation to diverge from.
static void FeedForwardSolo(void)
{
    ticcmd_t cmds[NET_MAXPLAYERS];
    boolean  ingame[NET_MAXPLAYERS];
    int me = BadgeNet_ConsolePlayer();
    int i;

    memset(cmds, 0, sizeof(cmds));
    for (i = 0; i < NET_MAXPLAYERS; i++)
        ingame[i] = false;
    ingame[me] = true;

    while (s_next_recv < s_maketic)
    {
        slot_t *s = SlotFor(s_next_recv);
        cmds[me] = s->cmd[me];
        D_ReceiveTic(cmds, ingame);
        s_next_recv++;
    }
}

// The peer stopped talking. Everything here exists so the survivor keeps
// playing the same game it was playing a second ago, rather than dropping into
// a single-player session with different rules mid-level.
static void HandlePeerLoss(void)
{
    s_peer_lost = true;
    ESP_LOGW(TAG, "peer silent for %d ms; continuing solo", PEER_TIMEOUT_MS);

    // The engine's own disconnect notice, on the D_Disconnected path it
    // already had.
    D_ReceiveTic(NULL, NULL);

    // RunTic now sees playeringame[1] && !ingame[1] exactly once, which runs
    // PlayerQuitGame and prints the vanilla "Player 2 left the game".
    FeedForwardSolo();

    // Last, and it must be last. d_loop only calls BadgeNet_Run while this is
    // set, so clearing it freezes recvtic wherever it stands -- while
    // GetLowTic stops clamping to it. Clear it with recvtic still below
    // maketic and TryRunTics waits forever on a tic nothing will ever deliver.
    // The catch-up above is what makes this line safe.
    net_client_connected = false;

    // netgame deliberately stays true: co-op item respawn, the "monsters
    // remember" flags and G_DoReborn's respawn path must not change halfway
    // through a level just because someone's battery died.
}

#endif  // !BADGE_NET_LOOPBACK

void BadgeNet_SendTiccmd(ticcmd_t *cmd, int tic)
{
    if (!s_paired)
        return;

    s_maketic = tic + 1;

#ifdef BADGE_NET_LOOPBACK
    {
        // Determinism harness: the local cmd stands in for both players, so
        // the ring, the ordered drain, D_ReceiveTic, GetLowTic, OldNetSync,
        // PlayersInGame, player 2's spawn and the consistancy check all run
        // exactly as they will on the air -- with the air removed, so any
        // divergence can only be ours.
        slot_t *s = SlotFor(tic);
        s->cmd[0] = *cmd;
        s->cmd[1] = *cmd;

        // consistancy is the one field that cannot be mirrored. G_BuildTiccmd
        // stamps it with consistancy[consoleplayer][...], and G_Ticker checks
        // each player's cmd against consistancy[that player][...], which
        // tracks that player's own mobj->x. Player 2 stands somewhere else, so
        // a copied byte is wrong by construction and I_Errors within a few
        // hundred tics -- measured, "consistency failure (0 should be 65)" at
        // gametic 485.
        //
        // This is an artefact of one badge playing both parts, not a flaw in
        // the exchange: on the air each badge stamps its own player's byte and
        // D_ReceiveTic never overwrites localplayer's cmd, so both sides
        // compare like with like. Stamping player 2's own byte here keeps the
        // check live for player 1 rather than defeating it for both.
        s->cmd[1].consistancy = consistancy[1][tic % BACKUPTICS];

        s->have   = HAVE_ALL;
    }
#else
    if (s_is_host)
    {
        // The host's own input is authoritative the moment it exists; nothing
        // has to go on the air for it.
        Deposit(tic, 0, cmd);
    }
    else
    {
        Deposit(tic, 1, cmd);
        ClientSendTiccmd();
    }
#endif
}

void BadgeNet_Run(void)
{
    if (!s_paired)
        return;

#ifdef BADGE_NET_LOOPBACK
    DeliverComplete();
#else
    if (s_peer_lost)
    {
        // Unreachable in the normal path: HandlePeerLoss clears
        // net_client_connected, and d_loop stops calling us. Kept because it
        // costs one branch and the alternative, if anything ever calls us
        // again, is a permanent stall.
        FeedForwardSolo();
        return;
    }

    DrainRadio();

    // The no-substitution rule, and the reason this whole file is careful. A
    // tic the peer has not sent yet simply does not advance s_next_recv -- we
    // never invent a cmd for it. D_ReceiveTic skips i == localplayer, so the
    // peer's own BuildNewTic output is never overwritten on its side; a cmd we
    // made up would be simulated here and not there, and the two games would
    // part company silently and instantly. Stalling costs frames.
    // Substituting costs the game.
    DeliverComplete();

    if (s_is_host)
        HostSendTicset();

    {
        int64_t now = esp_timer_get_time();

        // A keepalive, not padding: while the pair is stalled neither side has
        // anything new to say, and without this each would read the other's
        // silence as a disconnect and drop a badge that is sitting right there.
        if (now - s_last_tx_us > (int64_t)KEEPALIVE_MS * 1000)
        {
            if (s_is_host)
                HostSendTicset();
            else
                ClientSendTiccmd();
            s_last_tx_us = now;
        }

        if (now - s_last_rx_us > (int64_t)PEER_TIMEOUT_MS * 1000)
            HandlePeerLoss();
    }
#endif
}

void BadgeNet_Shutdown(void)
{
    if (!s_paired)
        return;

    s_paired = false;
    net_client_connected = false;
    badge_radio_flush();
    ESP_LOGI(TAG, "net shut down; %d tics were spent waiting on the peer",
             s_stall_tics);
}

int BadgeNet_LastRecvTic(void) { return (int)s_next_recv - 1; }
int BadgeNet_StallTics(void)   { return s_stall_tics; }
