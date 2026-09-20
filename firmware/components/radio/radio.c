// Badge-to-badge radio over ESP-NOW. See radio.h for the model.
//
// Three contexts touch this file:
//   - the main task, through the public API;
//   - the Wi-Fi driver's task, through the two ESP-NOW callbacks, which do
//     nothing but copy: protocol messages onto a queue, game frames into the
//     receive ring;
//   - the radio task, which drains that queue, runs the beacon/offer/session
//     state machine and answers pings itself, so a round trip measures the
//     air and the driver rather than Doom's frame time.
//
// Everything shared is behind one spinlock and is held for a copy at most.

#include "radio.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "radio";

// ------------------------------------------------------------- wire format
//
// Every packet: 'D' 'M' proto type, then a type-specific body. Anything with
// a different magic or proto is not ours and is dropped before it reaches the
// queue.

#define PROTO_VERSION   1
#define RADIO_CHANNEL   1

enum
{
    MSG_BEACON = 1,     // broadcast: here I am
    MSG_OFFER,          // unicast: play with me, with these settings
    MSG_ACCEPT,         // unicast: yes
    MSG_REFUSE,         // unicast: no, and why
    MSG_BYE,            // unicast: session over
    MSG_PING,           // unicast: 20 bytes, answered at once
    MSG_PONG,           // unicast: the same 20 bytes back
    MSG_DATA,           // unicast: opaque game frame
};

typedef struct __attribute__((packed))
{
    uint8_t magic[2];
    uint8_t proto;
    uint8_t type;
} hdr_t;

typedef struct __attribute__((packed))
{
    hdr_t    h;
    uint32_t build_id;
    uint32_t wad_id;
    uint8_t  state;
    char     name[RADIO_NAME_LEN];
} msg_beacon_t;

typedef struct __attribute__((packed))
{
    hdr_t            h;
    uint32_t         build_id;
    uint32_t         wad_id;
    radio_settings_t settings;
} msg_offer_t;

typedef struct __attribute__((packed))
{
    hdr_t    h;
    uint32_t build_id;
    uint32_t wad_id;
} msg_accept_t;

typedef struct __attribute__((packed))
{
    hdr_t   h;
    uint8_t reason;     // radio_refusal_t
} msg_refuse_t;

// The 20-byte frame the ticket asks to round-trip. A ticcmd fits in the
// padding, so this is also the size a lockstep tic will be.
typedef struct __attribute__((packed))
{
    hdr_t    h;
    uint16_t seq;
    uint32_t t_us;      // sender's clock; only the sender interprets it
    uint8_t  pad[10];
} msg_ping_t;

_Static_assert(sizeof(msg_ping_t) == 20, "ping frame is 20 bytes");

#define MAX_MSG   (sizeof(hdr_t) + RADIO_MAX_FRAME)

// ------------------------------------------------------------- timing

#define BEACON_PERIOD_US    250000      // 4 Hz: listed within a second
#define PEER_TTL_US         3000000     // drop a peer 3 s after its last beacon
#define OFFER_TIMEOUT_US    5000000

// The link test pings at TICRATE while a session is idle. Once a game is
// running it drops to 2 Hz, because the game's own frames are already
// proving the link every tic and answering them is pure duplicated airtime.
//
// This is a power decision as much as a bandwidth one. The badge runs off two
// AA cells through a boost converter (see HARDWARE.md), and a transmit burst
// is the largest current the board ever draws. Pinging at 35 Hz *and* sending
// a tic every 35 Hz doubles the number of those bursts at exactly the moment
// the game starts -- which is where the rail was collapsing and resetting the
// badge on battery while USB power hid it.
#define PING_PERIOD_US      (1000000 / 35)
#define PING_PERIOD_GAME_US 500000
#define GAME_QUIET_US       200000      // no game frame for this long: idle

// Transmit power, in units of 0.25 dBm. The default is the radio's maximum,
// which is meant for reaching an access point across a building; two badges
// are in the same hand. 11 dBm is still tens of metres between them and costs
// a fraction of the peak current.
#define RADIO_TX_POWER_QDBM 44
#define SESSION_TTL_US      2000000     // nothing from the peer for 2 s: lost
#define PING_SLOTS          16          // outstanding pings; ~457 ms at 35 Hz
#define STATS_LOG_PERIOD_US 1000000

// ------------------------------------------------------------- state

typedef struct
{
    uint8_t  mac[RADIO_MAC_LEN];
    uint8_t  len;
    uint8_t  data[MAX_MSG];
} queued_msg_t;

typedef struct
{
    radio_peer_t peer;
    int64_t      last_seen_us;
    bool         used;
} peer_slot_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;

static bool     s_up;
static uint8_t  s_mac[RADIO_MAC_LEN];
static char     s_name[RADIO_NAME_LEN];
static uint32_t s_build_id;
static uint32_t s_wad_id;
static bool     s_discoverable;

static radio_state_t   s_state = RADIO_OFF;
static radio_refusal_t s_refusal;
static peer_slot_t     s_peers[RADIO_MAX_PEERS];

// The peer an offer, request or session is with.
static radio_peer_t     s_partner;
static radio_settings_t s_settings;
static int64_t          s_offer_sent_us;
static int64_t          s_partner_heard_us;

// Receive ring for game frames: written by the driver callback, read by
// the main task. Eight frames is over 200 ms at TICRATE.
#define RX_RING 8
static struct { uint8_t len; uint8_t data[RADIO_MAX_FRAME]; } s_rx[RX_RING];
static volatile unsigned s_rx_head, s_rx_tail;
static uint32_t s_rx_dropped;

// When the game last put a frame on the air, so the link test can stay out
// of its way.
static int64_t s_last_data_tx_us;

// Link test.
static radio_link_stats_t s_stats;
static struct { int64_t t_us; bool pending; } s_pings[PING_SLOTS];
static uint16_t s_ping_seq;
static uint32_t s_rtt_window[32];
static unsigned s_rtt_n;

static const uint8_t BROADCAST[RADIO_MAC_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

// ------------------------------------------------------------- helpers

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, RADIO_MAC_LEN) == 0;
}

static void fill_hdr(hdr_t *h, uint8_t type)
{
    h->magic[0] = 'D';
    h->magic[1] = 'M';
    h->proto = PROTO_VERSION;
    h->type = type;
}

// ESP-NOW refuses unicast to a MAC it has not been told about.
static void ensure_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;

    esp_now_peer_info_t p = { 0 };
    memcpy(p.peer_addr, mac, RADIO_MAC_LEN);
    p.channel = RADIO_CHANNEL;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    esp_err_t err = esp_now_add_peer(&p);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "add peer: %s", esp_err_to_name(err));
}

static void forget_peer(const uint8_t *mac)
{
    if (!mac_eq(mac, BROADCAST) && esp_now_is_peer_exist(mac))
        esp_now_del_peer(mac);
}

static bool send_raw(const uint8_t *mac, const void *msg, size_t len)
{
    ensure_peer(mac);
    esp_err_t err = esp_now_send(mac, msg, len);
    if (err != ESP_OK)
    {
        portENTER_CRITICAL(&s_lock);
        s_stats.sends_failed++;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    return true;
}

static void send_refuse(const uint8_t *mac, radio_refusal_t why)
{
    msg_refuse_t m;
    fill_hdr(&m.h, MSG_REFUSE);
    m.reason = (uint8_t)why;
    send_raw(mac, &m, sizeof m);
}

static void send_simple(const uint8_t *mac, uint8_t type)
{
    hdr_t h;
    fill_hdr(&h, type);
    send_raw(mac, &h, sizeof h);
}

static radio_refusal_t compat_check(uint32_t build_id, uint32_t wad_id)
{
    if (build_id != s_build_id) return RADIO_REFUSED_FIRMWARE;
    if (wad_id != s_wad_id)     return RADIO_REFUSED_WAD;
    return RADIO_REFUSED_NONE;
}

// Lower MAC takes seat 0. Both sides compute it from the same two numbers,
// so no message has to carry it.
static uint8_t seat_for(const uint8_t *peer_mac)
{
    return memcmp(s_mac, peer_mac, RADIO_MAC_LEN) < 0 ? 0 : 1;
}

static void reset_link_stats(void)
{
    memset(&s_stats, 0, sizeof s_stats);
    memset(s_pings, 0, sizeof s_pings);
    s_ping_seq = 0;
    s_rtt_n = 0;
    s_rx_head = s_rx_tail = 0;
}

// Leave whatever we were doing. Lock held by the caller.
static void back_to_scanning_locked(radio_refusal_t why)
{
    s_state = RADIO_SCANNING;
    if (why != RADIO_REFUSED_NONE)
        s_refusal = why;
}

// ------------------------------------------------------------- peers

static peer_slot_t *find_peer_locked(const uint8_t *mac)
{
    for (int i = 0; i < RADIO_MAX_PEERS; i++)
        if (s_peers[i].used && mac_eq(s_peers[i].peer.mac, mac))
            return &s_peers[i];
    return NULL;
}

static void note_beacon(const uint8_t *mac, const msg_beacon_t *b, int64_t now)
{
    portENTER_CRITICAL(&s_lock);
    peer_slot_t *p = find_peer_locked(mac);
    if (p == NULL)
    {
        // Take a free slot, else evict the one heard from longest ago.
        peer_slot_t *oldest = &s_peers[0];
        for (int i = 0; i < RADIO_MAX_PEERS; i++)
        {
            if (!s_peers[i].used) { p = &s_peers[i]; break; }
            if (s_peers[i].last_seen_us < oldest->last_seen_us) oldest = &s_peers[i];
        }
        if (p == NULL) p = oldest;
        memset(p, 0, sizeof *p);
        p->used = true;
        memcpy(p->peer.mac, mac, RADIO_MAC_LEN);
    }
    memcpy(p->peer.name, b->name, RADIO_NAME_LEN);
    p->peer.name[RADIO_NAME_LEN - 1] = 0;
    p->peer.build_id = b->build_id;
    p->peer.wad_id = b->wad_id;
    p->peer.compatible = compat_check(b->build_id, b->wad_id) == RADIO_REFUSED_NONE;
    p->last_seen_us = now;
    portEXIT_CRITICAL(&s_lock);
}

static void expire_peers(int64_t now)
{
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < RADIO_MAX_PEERS; i++)
        if (s_peers[i].used && now - s_peers[i].last_seen_us > PEER_TTL_US)
            s_peers[i].used = false;
    portEXIT_CRITICAL(&s_lock);
}

// ------------------------------------------------------------- callbacks
//
// Run on the Wi-Fi driver's task. Copy and return.

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < (int)sizeof(hdr_t) || len > (int)MAX_MSG) return;

    const hdr_t *h = (const hdr_t *)data;
    if (h->magic[0] != 'D' || h->magic[1] != 'M' || h->proto != PROTO_VERSION)
        return;

    if (h->type == MSG_DATA)
    {
        // Straight into the ring: only frames from the session partner,
        // and only while there is a session.
        portENTER_CRITICAL(&s_lock);
        if (s_state == RADIO_CONNECTED && mac_eq(info->src_addr, s_partner.mac))
        {
            unsigned next = (s_rx_head + 1) % RX_RING;
            if (next == s_rx_tail)
            {
                s_rx_dropped++;
            }
            else
            {
                s_rx[s_rx_head].len = (uint8_t)(len - sizeof(hdr_t));
                memcpy(s_rx[s_rx_head].data, data + sizeof(hdr_t), s_rx[s_rx_head].len);
                s_rx_head = next;
            }
            s_partner_heard_us = esp_timer_get_time();
        }
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    queued_msg_t m;
    memcpy(m.mac, info->src_addr, RADIO_MAC_LEN);
    m.len = (uint8_t)len;
    memcpy(m.data, data, len);
    xQueueSend(s_queue, &m, 0);     // full queue: the beacon comes again
}

static void on_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status != ESP_NOW_SEND_SUCCESS)
    {
        portENTER_CRITICAL(&s_lock);
        s_stats.sends_failed++;
        portEXIT_CRITICAL(&s_lock);
    }
}

// ------------------------------------------------------------- protocol

static void send_beacon(void)
{
    msg_beacon_t b;
    fill_hdr(&b.h, MSG_BEACON);
    portENTER_CRITICAL(&s_lock);
    b.build_id = s_build_id;
    b.wad_id = s_wad_id;
    b.state = (uint8_t)s_state;
    memcpy(b.name, s_name, RADIO_NAME_LEN);
    portEXIT_CRITICAL(&s_lock);
    send_raw(BROADCAST, &b, sizeof b);
}

static void send_ping(int64_t now)
{
    msg_ping_t p;
    memset(&p, 0, sizeof p);
    fill_hdr(&p.h, MSG_PING);

    portENTER_CRITICAL(&s_lock);
    uint16_t seq = s_ping_seq++;
    unsigned slot = seq % PING_SLOTS;
    if (s_pings[slot].pending)
        s_stats.lost++;             // never answered before its slot came round
    s_pings[slot].pending = true;
    s_pings[slot].t_us = now;
    s_stats.sent++;
    uint8_t mac[RADIO_MAC_LEN];
    memcpy(mac, s_partner.mac, RADIO_MAC_LEN);
    portEXIT_CRITICAL(&s_lock);

    p.seq = seq;
    p.t_us = (uint32_t)now;
    send_raw(mac, &p, sizeof p);
}

static void note_pong(const msg_ping_t *p, int64_t now)
{
    portENTER_CRITICAL(&s_lock);
    unsigned slot = p->seq % PING_SLOTS;
    if (s_pings[slot].pending && (uint32_t)s_pings[slot].t_us == p->t_us)
    {
        uint32_t rtt = (uint32_t)(now - s_pings[slot].t_us);
        s_pings[slot].pending = false;
        s_stats.answered++;
        s_stats.rtt_last_us = rtt;
        if (rtt > s_stats.rtt_max_us) s_stats.rtt_max_us = rtt;
        s_rtt_window[s_rtt_n % 32] = rtt;
        s_rtt_n++;
        unsigned n = s_rtt_n < 32 ? s_rtt_n : 32;
        uint64_t sum = 0;
        for (unsigned i = 0; i < n; i++) sum += s_rtt_window[i];
        s_stats.rtt_avg_us = (uint32_t)(sum / n);
    }
    s_partner_heard_us = now;
    portEXIT_CRITICAL(&s_lock);
}

static void handle_msg(const queued_msg_t *m, int64_t now)
{
    const hdr_t *h = (const hdr_t *)m->data;

    switch (h->type)
    {
      case MSG_BEACON:
        if (m->len == sizeof(msg_beacon_t))
            note_beacon(m->mac, (const msg_beacon_t *)m->data, now);
        break;

      case MSG_OFFER:
      {
        if (m->len != sizeof(msg_offer_t)) break;
        const msg_offer_t *o = (const msg_offer_t *)m->data;

        radio_refusal_t why = compat_check(o->build_id, o->wad_id);
        if (why != RADIO_REFUSED_NONE)
        {
            ESP_LOGW(TAG, "refused offer from " MACSTR ": %s",
                     MAC2STR(m->mac), why == RADIO_REFUSED_FIRMWARE
                                      ? "different firmware" : "different WAD");
            send_refuse(m->mac, why);
            forget_peer(m->mac);
            break;
        }

        // A peer we have no beacon from yet (they may have found us first)
        // still gets a name, made the same way ours is.
        radio_peer_t unknown = { .compatible = true };
        memcpy(unknown.mac, m->mac, RADIO_MAC_LEN);
        snprintf(unknown.name, RADIO_NAME_LEN, "BADGE-%02X%02X",
                 m->mac[4], m->mac[5]);

        // Both pressed START at once: each is offering when the other's
        // offer lands. Rather than trade "busy" refusals, the badge that
        // would be player 2 yields and takes the incoming offer instead.
        bool taken = false;
        portENTER_CRITICAL(&s_lock);
        if (s_state == RADIO_SCANNING
            || (s_state == RADIO_OFFERING && mac_eq(m->mac, s_partner.mac)
                && seat_for(m->mac) == 1))
        {
            peer_slot_t *p = find_peer_locked(m->mac);
            s_partner = p != NULL ? p->peer : unknown;
            s_settings = o->settings;
            s_partner_heard_us = now;
            s_state = RADIO_INCOMING;
            taken = true;
        }
        portEXIT_CRITICAL(&s_lock);

        if (taken)
            ESP_LOGI(TAG, "offer from " MACSTR, MAC2STR(m->mac));
        else
        {
            send_refuse(m->mac, RADIO_REFUSED_BUSY);
            forget_peer(m->mac);
        }
        break;
      }

      case MSG_ACCEPT:
      {
        if (m->len != sizeof(msg_accept_t)) break;
        const msg_accept_t *a = (const msg_accept_t *)m->data;
        bool connected = false;
        portENTER_CRITICAL(&s_lock);
        if (s_state == RADIO_OFFERING && mac_eq(m->mac, s_partner.mac)
            && compat_check(a->build_id, a->wad_id) == RADIO_REFUSED_NONE)
        {
            s_state = RADIO_CONNECTED;
            s_partner_heard_us = now;
            reset_link_stats();
            connected = true;
        }
        portEXIT_CRITICAL(&s_lock);
        if (connected)
            ESP_LOGI(TAG, "connected to %s, we are player %d",
                     s_partner.name, seat_for(s_partner.mac) + 1);
        break;
      }

      case MSG_REFUSE:
      {
        if (m->len != sizeof(msg_refuse_t)) break;
        const msg_refuse_t *r = (const msg_refuse_t *)m->data;
        portENTER_CRITICAL(&s_lock);
        if (s_state == RADIO_OFFERING && mac_eq(m->mac, s_partner.mac))
            back_to_scanning_locked((radio_refusal_t)r->reason);
        portEXIT_CRITICAL(&s_lock);
        forget_peer(m->mac);
        ESP_LOGW(TAG, "offer refused by " MACSTR ", reason %d",
                 MAC2STR(m->mac), r->reason);
        break;
      }

      case MSG_BYE:
        portENTER_CRITICAL(&s_lock);
        if (s_state != RADIO_SCANNING && mac_eq(m->mac, s_partner.mac))
            back_to_scanning_locked(s_state == RADIO_CONNECTED
                                    ? RADIO_REFUSED_LOST : RADIO_REFUSED_DECLINED);
        portEXIT_CRITICAL(&s_lock);
        forget_peer(m->mac);
        ESP_LOGI(TAG, "bye from " MACSTR, MAC2STR(m->mac));
        break;

      case MSG_PING:
        if (m->len != sizeof(msg_ping_t)) break;
        if (s_state == RADIO_CONNECTED && mac_eq(m->mac, s_partner.mac))
        {
            msg_ping_t pong;
            memcpy(&pong, m->data, sizeof pong);
            pong.h.type = MSG_PONG;
            send_raw(m->mac, &pong, sizeof pong);
            portENTER_CRITICAL(&s_lock);
            s_partner_heard_us = now;
            portEXIT_CRITICAL(&s_lock);
        }
        break;

      case MSG_PONG:
        if (m->len != sizeof(msg_ping_t)) break;
        if (s_state == RADIO_CONNECTED && mac_eq(m->mac, s_partner.mac))
            note_pong((const msg_ping_t *)m->data, now);
        break;
    }
}

static void log_link(void)
{
    radio_link_stats_t s = radio_link_stats();
    uint32_t dropped;
    portENTER_CRITICAL(&s_lock);
    dropped = s_rx_dropped;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "link: sent %lu answered %lu lost %lu (%.1f%%) "
                  "rtt last %lu avg %lu max %lu us, send fail %lu, rx drop %lu",
             (unsigned long)s.sent, (unsigned long)s.answered,
             (unsigned long)s.lost,
             s.sent ? 100.0 * s.lost / s.sent : 0.0,
             (unsigned long)s.rtt_last_us, (unsigned long)s.rtt_avg_us,
             (unsigned long)s.rtt_max_us, (unsigned long)s.sends_failed,
             (unsigned long)dropped);
}

static void radio_task(void *arg)
{
    (void)arg;
    int64_t next_beacon = 0, next_ping = 0, next_expire = 0, next_log = 0;

    for (;;)
    {
        int64_t now = esp_timer_get_time();

        // Copy the bits of state that decide what is due.
        portENTER_CRITICAL(&s_lock);
        radio_state_t st = s_state;
        bool beaconing = s_discoverable;
        int64_t offer_sent = s_offer_sent_us;
        int64_t heard = s_partner_heard_us;
        int64_t data_tx = s_last_data_tx_us;
        portEXIT_CRITICAL(&s_lock);

        if (beaconing && now >= next_beacon)
        {
            send_beacon();
            next_beacon = now + BEACON_PERIOD_US;
        }

        if (now >= next_expire)
        {
            expire_peers(now);
            next_expire = now + 500000;
        }

        if (st == RADIO_OFFERING && now - offer_sent > OFFER_TIMEOUT_US)
        {
            portENTER_CRITICAL(&s_lock);
            if (s_state == RADIO_OFFERING)
                back_to_scanning_locked(RADIO_REFUSED_TIMEOUT);
            portEXIT_CRITICAL(&s_lock);
            // Withdraw it, so their screen does not keep asking.
            send_simple(s_partner.mac, MSG_BYE);
            forget_peer(s_partner.mac);
            ESP_LOGW(TAG, "offer to %s timed out", s_partner.name);
        }

        if (st == RADIO_CONNECTED)
        {
            if (now >= next_ping)
            {
                send_ping(now);
                next_ping = now + (now - data_tx < GAME_QUIET_US
                                   ? PING_PERIOD_GAME_US : PING_PERIOD_US);
            }
            if (now >= next_log)
            {
                log_link();
                next_log = now + STATS_LOG_PERIOD_US;
            }
            if (now - heard > SESSION_TTL_US)
            {
                portENTER_CRITICAL(&s_lock);
                if (s_state == RADIO_CONNECTED)
                    back_to_scanning_locked(RADIO_REFUSED_LOST);
                portEXIT_CRITICAL(&s_lock);
                forget_peer(s_partner.mac);
                ESP_LOGW(TAG, "lost %s", s_partner.name);
            }
        }
        else
        {
            next_ping = 0;
            next_log = 0;
        }

        // Sleep until the next thing is due, or a message arrives.
        int64_t due = next_expire;
        if (beaconing && next_beacon < due) due = next_beacon;
        if (st == RADIO_CONNECTED && next_ping < due) due = next_ping;
        int64_t wait_us = due - esp_timer_get_time();
        TickType_t ticks = wait_us <= 0 ? 0 : pdMS_TO_TICKS((wait_us + 999) / 1000);
        if (ticks == 0 && wait_us > 0) ticks = 1;

        queued_msg_t m;
        if (xQueueReceive(s_queue, &m, ticks) == pdTRUE)
            handle_msg(&m, esp_timer_get_time());
    }
}

// ------------------------------------------------------------- public

bool radio_init(void)
{
    if (s_up) return true;

    size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t before_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    int64_t t0 = esp_timer_get_time();

    // No esp_netif and no default event loop: nothing here speaks IP, and
    // the only Wi-Fi events a station that never associates would post are
    // ones nobody listens for. The driver logs the missing loop once and
    // carries on; the loop's task and queue would cost the zone ~3.5 KB.
    esp_err_t err;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&cfg)) != ESP_OK
     || (err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK
     || (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK
     || (err = esp_wifi_start()) != ESP_OK
     || (err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK
     || (err = esp_wifi_set_max_tx_power(RADIO_TX_POWER_QDBM)) != ESP_OK
     || (err = esp_wifi_set_channel(RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE)) != ESP_OK
     || (err = esp_now_init()) != ESP_OK
     || (err = esp_now_register_recv_cb(on_recv)) != ESP_OK
     || (err = esp_now_register_send_cb(on_send)) != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi/esp-now: %s", esp_err_to_name(err));
        return false;
    }

    esp_now_peer_info_t bc = { 0 };
    memcpy(bc.peer_addr, BROADCAST, RADIO_MAC_LEN);
    bc.channel = RADIO_CHANNEL;
    bc.ifidx = WIFI_IF_STA;
    if ((err = esp_now_add_peer(&bc)) != ESP_OK)
    {
        ESP_LOGE(TAG, "broadcast peer: %s", esp_err_to_name(err));
        return false;
    }

    int8_t tx_power = 0;
    esp_wifi_get_max_tx_power(&tx_power);

    esp_wifi_get_mac(WIFI_IF_STA, s_mac);
    snprintf(s_name, sizeof s_name, "BADGE-%02X%02X", s_mac[4], s_mac[5]);

    s_queue = xQueueCreate(8, sizeof(queued_msg_t));
    if (s_queue == NULL
     || xTaskCreate(radio_task, "radio", 2560, NULL, 5, &s_task) != pdPASS)
    {
        ESP_LOGE(TAG, "no task");
        return false;
    }

    s_state = RADIO_SCANNING;
    s_up = true;

    size_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "%s up on channel %d at %.2f dBm in %u ms: DRAM %u -> %u "
                  "(%u taken), largest block %u -> %u",
             s_name, RADIO_CHANNEL, tx_power * 0.25,
             (unsigned)((esp_timer_get_time() - t0) / 1000),
             (unsigned)before, (unsigned)after, (unsigned)(before - after),
             (unsigned)before_largest,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
}

void radio_set_identity(uint32_t build_id, uint32_t wad_id)
{
    portENTER_CRITICAL(&s_lock);
    s_build_id = build_id;
    s_wad_id = wad_id;
    for (int i = 0; i < RADIO_MAX_PEERS; i++)
        if (s_peers[i].used)
            s_peers[i].peer.compatible =
                compat_check(s_peers[i].peer.build_id, s_peers[i].peer.wad_id)
                == RADIO_REFUSED_NONE;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "identity: build %08lx wad %08lx",
             (unsigned long)build_id, (unsigned long)wad_id);
}

const char *radio_name(void)
{
    return s_name;
}

void radio_set_discoverable(bool on)
{
    if (!s_up) return;
    portENTER_CRITICAL(&s_lock);
    s_discoverable = on;
    portEXIT_CRITICAL(&s_lock);
    // Wake the task so the first beacon goes out now, not at the next tick.
    if (on && s_task) xTaskAbortDelay(s_task);
}

radio_state_t radio_state(void)
{
    portENTER_CRITICAL(&s_lock);
    radio_state_t st = s_state;
    portEXIT_CRITICAL(&s_lock);
    return st;
}

int radio_peers(radio_peer_t *out, int max)
{
    int n = 0;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < RADIO_MAX_PEERS && n < max; i++)
    {
        if (!s_peers[i].used) continue;
        out[n] = s_peers[i].peer;
        out[n].age_ms = (uint32_t)((now - s_peers[i].last_seen_us) / 1000);
        n++;
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

bool radio_offer(const uint8_t mac[RADIO_MAC_LEN], const radio_settings_t *s)
{
    if (!s_up) return false;

    msg_offer_t o;
    fill_hdr(&o.h, MSG_OFFER);

    portENTER_CRITICAL(&s_lock);
    peer_slot_t *p = (s_state == RADIO_SCANNING) ? find_peer_locked(mac) : NULL;
    bool ok = p != NULL && p->peer.compatible;
    if (ok)
    {
        s_partner = p->peer;
        s_settings = *s;
        s_offer_sent_us = esp_timer_get_time();
        s_state = RADIO_OFFERING;
        s_refusal = RADIO_REFUSED_NONE;
    }
    else if (p != NULL)
    {
        s_refusal = compat_check(p->peer.build_id, p->peer.wad_id);
    }
    o.build_id = s_build_id;
    o.wad_id = s_wad_id;
    o.settings = *s;
    portEXIT_CRITICAL(&s_lock);

    if (!ok) return false;

    ESP_LOGI(TAG, "offering to %s", s_partner.name);
    if (!send_raw(mac, &o, sizeof o))
    {
        portENTER_CRITICAL(&s_lock);
        back_to_scanning_locked(RADIO_REFUSED_TIMEOUT);
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    return true;
}

bool radio_incoming(radio_peer_t *from, radio_settings_t *proposed)
{
    portENTER_CRITICAL(&s_lock);
    bool ok = s_state == RADIO_INCOMING;
    if (ok)
    {
        if (from) *from = s_partner;
        if (proposed) *proposed = s_settings;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

void radio_accept(void)
{
    msg_accept_t a;
    fill_hdr(&a.h, MSG_ACCEPT);

    portENTER_CRITICAL(&s_lock);
    bool ok = s_state == RADIO_INCOMING;
    if (ok)
    {
        s_state = RADIO_CONNECTED;
        s_partner_heard_us = esp_timer_get_time();
        reset_link_stats();
    }
    a.build_id = s_build_id;
    a.wad_id = s_wad_id;
    portEXIT_CRITICAL(&s_lock);

    if (!ok) return;
    send_raw(s_partner.mac, &a, sizeof a);
    if (s_task) xTaskAbortDelay(s_task);
    ESP_LOGI(TAG, "accepted %s, we are player %d",
             s_partner.name, seat_for(s_partner.mac) + 1);
}

void radio_decline(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ok = s_state == RADIO_INCOMING;
    if (ok) back_to_scanning_locked(RADIO_REFUSED_NONE);
    portEXIT_CRITICAL(&s_lock);
    if (!ok) return;
    send_refuse(s_partner.mac, RADIO_REFUSED_DECLINED);
    forget_peer(s_partner.mac);
}

void radio_disconnect(void)
{
    if (!s_up) return;
    portENTER_CRITICAL(&s_lock);
    radio_state_t was = s_state;
    if (was != RADIO_SCANNING) back_to_scanning_locked(RADIO_REFUSED_NONE);
    s_refusal = RADIO_REFUSED_NONE;
    portEXIT_CRITICAL(&s_lock);

    if (was == RADIO_SCANNING) return;
    if (was == RADIO_INCOMING) send_refuse(s_partner.mac, RADIO_REFUSED_DECLINED);
    else                       send_simple(s_partner.mac, MSG_BYE);
    if (was == RADIO_CONNECTED) log_link();
    forget_peer(s_partner.mac);
}

bool radio_session(radio_session_t *out)
{
    portENTER_CRITICAL(&s_lock);
    bool ok = s_state == RADIO_CONNECTED;
    if (ok && out)
    {
        out->peer = s_partner;
        out->settings = s_settings;
        out->player = seat_for(s_partner.mac);
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

radio_refusal_t radio_refusal(void)
{
    portENTER_CRITICAL(&s_lock);
    radio_refusal_t r = s_refusal;
    portEXIT_CRITICAL(&s_lock);
    return r;
}

void radio_clear_refusal(void)
{
    portENTER_CRITICAL(&s_lock);
    s_refusal = RADIO_REFUSED_NONE;
    portEXIT_CRITICAL(&s_lock);
}

bool radio_send(const void *data, size_t len)
{
    if (len > RADIO_MAX_FRAME) return false;

    uint8_t buf[MAX_MSG];
    uint8_t mac[RADIO_MAC_LEN];
    fill_hdr((hdr_t *)buf, MSG_DATA);
    memcpy(buf + sizeof(hdr_t), data, len);

    portENTER_CRITICAL(&s_lock);
    bool ok = s_state == RADIO_CONNECTED;
    memcpy(mac, s_partner.mac, RADIO_MAC_LEN);
    if (ok) s_last_data_tx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);

    return ok && send_raw(mac, buf, sizeof(hdr_t) + len);
}

int radio_recv(void *data, size_t max)
{
    int n = 0;
    portENTER_CRITICAL(&s_lock);
    if (s_rx_tail != s_rx_head)
    {
        n = s_rx[s_rx_tail].len;
        if ((size_t)n > max) n = (int)max;
        memcpy(data, s_rx[s_rx_tail].data, n);
        s_rx_tail = (s_rx_tail + 1) % RX_RING;
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

radio_link_stats_t radio_link_stats(void)
{
    portENTER_CRITICAL(&s_lock);
    radio_link_stats_t s = s_stats;
    portEXIT_CRITICAL(&s_lock);
    return s;
}
