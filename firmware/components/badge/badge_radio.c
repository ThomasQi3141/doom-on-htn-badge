#include "badge_radio.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "radio";

// Channel 1 by agreement, not by discovery. Two badges with no AP between them
// have nothing to negotiate on, and scanning would cost time and code for a
// choice that only has to match.
#define RADIO_CHANNEL 1

// Six frames is two tics of slack at 35 Hz in each direction. The game drains
// this every tic; if it ever fills, something above has stalled, and the drop
// counter is the symptom to look for.
#define RADIO_QUEUE_DEPTH 6

static const uint8_t k_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static bool             s_ready;
static bool             s_started;
static QueueHandle_t    s_rx;
static uint8_t          s_mac[6];
static uint8_t          s_seq;
static volatile uint32_t s_dropped;
static volatile uint32_t s_send_failures;

// Runs on the WiFi task (priority 23), above everything Doom uses. It must not
// block and must not take long: validate, copy, post, return.
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    badge_radio_packet_t pkt;

    if (len < (int)sizeof(badge_radio_hdr_t) || len > BADGE_RADIO_MAX_PAYLOAD)
        return;

    // Someone else's ESP-NOW traffic, or noise that happened to decode.
    badge_radio_hdr_t hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != BADGE_RADIO_MAGIC)
        return;

    memcpy(pkt.mac, info->src_addr, 6);
    pkt.len = (uint8_t)len;
    memcpy(pkt.data, data, (size_t)len);

    // Never block the WiFi task waiting for the game to catch up.
    if (xQueueSend(s_rx, &pkt, 0) != pdTRUE)
        s_dropped++;
}

static void on_send(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    // Broadcast frames are never acknowledged, so this only ever reports on the
    // unicast handshake. Counting it separates "the peer never heard us" from
    // "the peer heard us and did not like it".
    if (status != ESP_NOW_SEND_SUCCESS)
        s_send_failures++;
}

esp_err_t badge_radio_init(void)
{
    esp_err_t err;

    if (s_ready)
        return ESP_OK;

    s_rx = xQueueCreate(RADIO_QUEUE_DEPTH, sizeof(badge_radio_packet_t));
    if (s_rx == NULL)
        return ESP_ERR_NO_MEM;

    // The WiFi PHY keeps its calibration data in NVS. Nothing else here uses
    // it -- CONFIG_ESP_WIFI_NVS_ENABLED is off, so no credentials are stored.
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
        goto fail_queue;

    // ESP-NOW itself does not need an event loop, but esp_wifi_init posts to
    // the default one. Already-created is fine: main may have made it.
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        goto fail_queue;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK)
        goto fail_queue;

    // No netif and no lwIP: nothing here ever gets an IP address. STA mode is
    // simply the mode ESP-NOW needs a channel in.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail_wifi;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto fail_wifi;
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail_wifi;
    s_started = true;

    // Without an AP there are no beacons to wake for, so modem sleep would
    // simply drop inbound frames between our own transmissions.
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) goto fail_wifi;

    err = esp_wifi_set_channel(RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) goto fail_wifi;

    err = esp_now_init();
    if (err != ESP_OK) goto fail_wifi;

    err = esp_now_register_recv_cb(on_recv);
    if (err != ESP_OK) goto fail_now;
    err = esp_now_register_send_cb(on_send);
    if (err != ESP_OK) goto fail_now;

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, k_broadcast, 6);
    peer.channel = RADIO_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK) goto fail_now;

    err = esp_wifi_get_mac(WIFI_IF_STA, s_mac);
    if (err != ESP_OK) goto fail_now;

    s_ready = true;
    ESP_LOGI(TAG, "up on channel %d, mac %02x:%02x:%02x:%02x:%02x:%02x",
             RADIO_CHANNEL, s_mac[0], s_mac[1], s_mac[2],
             s_mac[3], s_mac[4], s_mac[5]);
    return ESP_OK;

    // Unwind in reverse, so a failed init leaves nothing half-running holding
    // memory the zone heap is about to be sized against.
fail_now:
    esp_now_deinit();
fail_wifi:
    if (s_started) { esp_wifi_stop(); s_started = false; }
    esp_wifi_deinit();
fail_queue:
    vQueueDelete(s_rx);
    s_rx = NULL;
    ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
    return err;
}

bool badge_radio_ready(void)
{
    return s_ready;
}

esp_err_t badge_radio_stop(void)
{
    if (!s_started)
        return ESP_OK;

    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK)
    {
        s_started = false;
        s_ready = false;
        ESP_LOGI(TAG, "stopped");
    }
    return err;
}

const uint8_t *badge_radio_mac(void)
{
    return s_mac;
}

esp_err_t badge_radio_add_peer(const uint8_t mac[6])
{
    if (!s_ready)
        return ESP_ERR_INVALID_STATE;
    if (esp_now_is_peer_exist(mac))
        return ESP_OK;

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = RADIO_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer);
}

static esp_err_t send_to(const uint8_t mac[6], badge_msg_type_t type,
                         const void *payload, size_t payload_len)
{
    uint8_t frame[BADGE_RADIO_MAX_PAYLOAD];
    badge_radio_hdr_t hdr;

    if (!s_ready)
        return ESP_ERR_INVALID_STATE;
    if (payload_len + sizeof(hdr) > sizeof(frame))
        return ESP_ERR_INVALID_SIZE;

    hdr.magic = BADGE_RADIO_MAGIC;
    hdr.type  = (uint8_t)type;
    hdr.seq   = s_seq++;

    memcpy(frame, &hdr, sizeof(hdr));
    if (payload_len > 0)
        memcpy(frame + sizeof(hdr), payload, payload_len);

    return esp_now_send(mac, frame, sizeof(hdr) + payload_len);
}

esp_err_t badge_radio_send(badge_msg_type_t type, const void *payload,
                           size_t payload_len)
{
    return send_to(k_broadcast, type, payload, payload_len);
}

esp_err_t badge_radio_send_to(const uint8_t mac[6], badge_msg_type_t type,
                              const void *payload, size_t payload_len)
{
    return send_to(mac, type, payload, payload_len);
}

bool badge_radio_recv(badge_radio_packet_t *out, uint32_t timeout_ms)
{
    if (!s_ready)
        return false;
    return xQueueReceive(s_rx, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void badge_radio_flush(void)
{
    if (s_rx != NULL)
        xQueueReset(s_rx);
}

uint32_t badge_radio_dropped(void)
{
    return s_dropped;
}

uint32_t badge_radio_send_failures(void)
{
    return s_send_failures;
}
