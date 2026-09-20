// Badge-to-badge radio over ESP-NOW.
//
// Knows nothing about Doom. It finds other badges, agrees a session with one
// of them, and then moves small fixed-size frames between the two. The engine
// side (badge_boot.c) drives the state machine from its connect screen and
// the lockstep code will move ticcmds through radio_send / radio_recv.
//
// All calls are safe from the main task. The ESP-NOW callbacks and the
// protocol task run elsewhere; the shared state is behind a spinlock.

#ifndef BADGE_RADIO_H
#define BADGE_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIO_MAC_LEN     6
#define RADIO_NAME_LEN    12        // "BADGE-CFCC" plus NUL, padded
#define RADIO_MAX_PEERS   4
#define RADIO_MAX_FRAME   32        // game payload per radio_send

typedef enum
{
    RADIO_OFF,          // radio_init failed or was never called
    RADIO_SCANNING,     // listening for beacons, sending ours if discoverable
    RADIO_OFFERING,     // we sent an offer and are waiting for the answer
    RADIO_INCOMING,     // a peer offered us a game; accept or decline
    RADIO_CONNECTED,    // session agreed with one peer
} radio_state_t;

// Why the last offer ended without a session. Cleared by radio_clear_refusal.
typedef enum
{
    RADIO_REFUSED_NONE,
    RADIO_REFUSED_DECLINED,     // the other player said no
    RADIO_REFUSED_BUSY,         // they were already offering or connected
    RADIO_REFUSED_FIRMWARE,     // their build is not ours
    RADIO_REFUSED_WAD,          // their WAD is not ours
    RADIO_REFUSED_TIMEOUT,      // no answer
    RADIO_REFUSED_LOST,         // the session dropped
} radio_refusal_t;

typedef struct
{
    uint8_t  mac[RADIO_MAC_LEN];
    char     name[RADIO_NAME_LEN];
    uint32_t build_id;
    uint32_t wad_id;
    bool     compatible;        // build_id and wad_id both match ours
    uint32_t age_ms;            // since the last beacon
} radio_peer_t;

// What the two badges must agree on before a game can start. The offerer's
// values win; the accepter adopts them.
typedef struct
{
    uint8_t skill;
    uint8_t episode;
    uint8_t map;
    uint8_t deathmatch;
    uint8_t nomonsters;
} radio_settings_t;

typedef struct
{
    radio_peer_t     peer;
    radio_settings_t settings;
    uint8_t          player;    // 0 or 1: our seat, by MAC order
} radio_session_t;

// Round-trip statistics for the 20-byte ping frames that run at TICRATE
// while connected. Latency is the full round trip in microseconds.
typedef struct
{
    uint32_t sent;
    uint32_t answered;
    uint32_t lost;              // unanswered after RADIO_PING_TIMEOUT_MS
    uint32_t rtt_last_us;
    uint32_t rtt_avg_us;        // moving average over the last 32 answers
    uint32_t rtt_max_us;
    uint32_t sends_failed;      // esp_now_send refused or reported failure
} radio_link_stats_t;

// Bring the Wi-Fi driver and ESP-NOW up. Call before anything sizes a large
// heap, because the driver takes its DRAM here and never gives it back. Logs
// the DRAM it cost. Returns false if the radio could not start; every other
// call is then a harmless no-op.
bool radio_init(void);

// Who we are. Beacons carry both ids and an offer from a badge with either
// different is refused rather than paired.
void radio_set_identity(uint32_t build_id, uint32_t wad_id);
const char *radio_name(void);

// Send beacons so other badges can list us. Off by default; the connect
// screen turns it on and the menu turns it off.
void radio_set_discoverable(bool on);

radio_state_t radio_state(void);

// Peers heard recently, compatible or not. Returns the count written.
int radio_peers(radio_peer_t *out, int max);

// Offer a game to a peer. Only from RADIO_SCANNING; false if refused
// locally (unknown peer, incompatible). The answer arrives as a state change
// to RADIO_CONNECTED, or back to RADIO_SCANNING with a refusal set.
bool radio_offer(const uint8_t mac[RADIO_MAC_LEN], const radio_settings_t *s);

// From RADIO_INCOMING: who is asking and what they propose.
bool radio_incoming(radio_peer_t *from, radio_settings_t *proposed);
void radio_accept(void);
void radio_decline(void);

// Drop whatever is in progress -- an offer, an incoming request or a
// session -- and go back to scanning. Tells the peer.
void radio_disconnect(void);

// From RADIO_CONNECTED.
bool radio_session(radio_session_t *out);

radio_refusal_t radio_refusal(void);
void radio_clear_refusal(void);

// Game frames, only while connected. radio_recv returns the payload length,
// or 0 when nothing is waiting.
bool radio_send(const void *data, size_t len);
int  radio_recv(void *data, size_t max);

radio_link_stats_t radio_link_stats(void);

#endif
