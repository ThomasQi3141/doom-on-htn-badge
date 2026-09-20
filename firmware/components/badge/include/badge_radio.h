// ESP-NOW transport for two-badge co-op.
//
// Deliberately thin: this layer knows about framing and nothing about Doom.
// It brings the radio up in the smallest configuration that can carry a
// broadcast frame -- no netif, no lwIP, no station connect -- and hands
// received payloads to the game through a queue.
//
// Everything above this file lives in the doom component's badge_net.c.

#ifndef BADGE_RADIO_H
#define BADGE_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ESP-NOW's own ceiling. Nothing we send comes close: the largest frame is a
// TICSET at four tics deep for two players, which is 78 bytes.
#define BADGE_RADIO_MAX_PAYLOAD 250

#define BADGE_RADIO_MAGIC 0x4d44u  // 'DM'

// Wire message types. The numbering is part of the protocol; append only.
typedef enum {
    BADGE_MSG_ANNOUNCE = 1,  // host -> everyone, "I am waiting for player 2"
    BADGE_MSG_JOIN     = 2,  // client -> host, "I want in"
    BADGE_MSG_START    = 3,  // host -> client, settings + WAD id
    BADGE_MSG_TICCMD   = 4,  // client -> host, its input for up to 4 tics
    BADGE_MSG_TICSET   = 5,  // host -> client, both players' input, 4 tics deep
} badge_msg_type_t;

// Prefixes every payload. Packed because the two badges must agree on the
// layout byte for byte, and the fields are deliberately unaligned-safe.
typedef struct __attribute__((packed)) {
    uint16_t magic;  // BADGE_RADIO_MAGIC; anything else is someone else's frame
    uint8_t  type;   // badge_msg_type_t
    uint8_t  seq;    // wraps; for logging and duplicate spotting only
} badge_radio_hdr_t;

// One received frame, copied out of the WiFi task's stack. `len` is the length
// of the whole frame, header included, so the payload is `len - sizeof(hdr)`.
typedef struct {
    uint8_t mac[6];
    uint8_t len;
    uint8_t data[BADGE_RADIO_MAX_PAYLOAD];
} badge_radio_packet_t;

// Brings up WiFi in station mode on a fixed channel and registers the
// broadcast peer. Safe to call twice; the second call is a no-op.
esp_err_t badge_radio_init(void);

// True once badge_radio_init() has succeeded.
bool badge_radio_ready(void);

// Stops the radio and hands its dynamic buffers back, without deinitialising.
// Used when the player picks single player after the radio has come up.
esp_err_t badge_radio_stop(void);

// This badge's station MAC, for identifying peers during pairing.
const uint8_t *badge_radio_mac(void);

// Broadcasts one frame. `hdr->seq` is filled in by this call. `payload` may be
// NULL when `payload_len` is zero.
esp_err_t badge_radio_send(badge_msg_type_t type, const void *payload,
                           size_t payload_len);

// Sends to one specific peer rather than the broadcast address. Used for the
// pairing handshake, where the recipient is known and a retry is worth the
// latency. Tic traffic deliberately does not use this -- see badge_net.c.
esp_err_t badge_radio_send_to(const uint8_t mac[6], badge_msg_type_t type,
                              const void *payload, size_t payload_len);

// Adds a unicast peer. Idempotent.
esp_err_t badge_radio_add_peer(const uint8_t mac[6]);

// Pops the next received frame, or returns false if none arrived within
// `timeout_ms`. Pass 0 to poll. Frames whose magic does not match, or that are
// shorter than the header, are dropped inside the receive callback and never
// reach here.
bool badge_radio_recv(badge_radio_packet_t *out, uint32_t timeout_ms);

// Drops every queued frame. Used when pairing restarts.
void badge_radio_flush(void);

// Frames dropped because the queue was full -- a stall somewhere above this
// layer. Purely diagnostic.
uint32_t badge_radio_dropped(void);

// Unicast sends the driver reported as failed. Purely diagnostic; broadcast
// sends are never acknowledged and so never counted here.
uint32_t badge_radio_send_failures(void);

#ifdef __cplusplus
}
#endif

#endif  // BADGE_RADIO_H
