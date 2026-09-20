#include "badge_net.h"

#include <string.h>

#include "badge_radio.h"
#include "d_loop.h"
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
    return s_requested;
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
        ESP_LOGI(TAG, "paired, I am player %d of %d",
                 BadgeNet_ConsolePlayer() + 1, BADGE_NET_PLAYERS);

    return s_paired;
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
