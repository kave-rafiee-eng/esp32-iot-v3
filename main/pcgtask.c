#include "pcgtask.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "fwu.h"
#include "mqtt_data.h"
#include "pcg.h"
#include "pcg_rtr.h"

static const char *TAG = "pcgtask";

#define PCG_RX_ACC_SIZE 2048
#define DIP_PAYLOAD_SIZE 3
#define DIP_CMD_SYNCK_REQUEST 1

extern uint16_t deviceSerial;

static esp_mqtt_client_handle_t s_client;
static bool s_synck_busy;
static bool s_synck_armed;
static uint8_t s_release[MQTT_PAYLOAD_MAX_LEN];
static size_t s_release_len;
static bool s_release_valid;

typedef struct {
    uint8_t data[MQTT_PAYLOAD_MAX_LEN];
    uint16_t len;
} pending_slot_t;

static pending_slot_t s_pending[CONFIG_PCG_QUEUE_DEPTH];
static int s_pending_count;

static uint8_t s_rx_acc[PCG_RX_ACC_SIZE];
static size_t s_rx_len;
static size_t s_rx_scanned;
static TickType_t s_rx_last;

static const char *mqtt_suffix(uint8_t peer)
{
    switch (peer) {
    case PCG_PEER_CLIENT:
        return "client";
    case PCG_PEER_SERVER_UPDATE:
        return "server_update";
    case PCG_PEER_SERVER_RECORD:
        return "server_record";
    default:
        return NULL;
    }
}

static void mqtt_publish_frame(const char *suffix, const uint8_t *frame, size_t len)
{
    char topic[64];
    snprintf(topic, sizeof(topic), "%u/%s", (unsigned)deviceSerial, suffix);

    int msg_id = esp_mqtt_client_publish(s_client, topic, (const char *)frame, (int)len, 0, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "publish failed topic=%s len=%u", topic, (unsigned)len);
        return;
    }
    ESP_LOGI(TAG, "publish %s len=%u msg_id=%d", topic, (unsigned)len, msg_id);
}

static bool pending_push(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > MQTT_PAYLOAD_MAX_LEN) {
        ESP_LOGW(TAG, "pending frame dropped, len=%u", (unsigned)len);
        return false;
    }
    if (s_pending_count >= CONFIG_PCG_QUEUE_DEPTH) {
        ESP_LOGW(TAG, "pending queue full, dropping frame");
        return false;
    }

    pending_slot_t *slot = &s_pending[s_pending_count];
    memcpy(slot->data, data, len);
    slot->len = (uint16_t)len;
    s_pending_count++;
    return true;
}

static bool pending_pop(uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (s_pending_count <= 0) {
        return false;
    }

#if CONFIG_PCG_QUEUE_FIFO
    pending_slot_t *slot = &s_pending[0];
#else
    pending_slot_t *slot = &s_pending[s_pending_count - 1];
#endif

    if (slot->len > out_cap) {
        return false;
    }

    memcpy(out, slot->data, slot->len);
    *out_len = slot->len;

#if CONFIG_PCG_QUEUE_FIFO
    s_pending_count--;
    if (s_pending_count > 0) {
        memmove(&s_pending[0], &s_pending[1], (size_t)s_pending_count * sizeof(s_pending[0]));
    }
#else
    s_pending_count--;
#endif
    return true;
}

static void handle_dip(const uint8_t *payload, size_t len)
{
    if (len != DIP_PAYLOAD_SIZE) {
        ESP_LOGW(TAG, "DIP rejected, len=%u", (unsigned)len);
        return;
    }

    uint16_t serial = ((uint16_t)payload[0] << 8) | payload[1];
    uint8_t command = payload[2];
    if (command != DIP_CMD_SYNCK_REQUEST) {
        ESP_LOGW(TAG, "DIP command ignored: %u", command);
        return;
    }
    if (s_synck_busy) {
        return;
    }

    s_release_valid = pending_pop(s_release, sizeof(s_release), &s_release_len);
    ESP_LOGI(TAG, "SYNCK serial=%u queued=%d", (unsigned)serial, s_pending_count);
    if (!s_release_valid) {
        return;
    }

    /* Hold the line until this chunk is finished so a second SYNCK is ignored. */
    s_synck_busy = true;
    s_synck_armed = true;
}

static void handle_fwu(const pcg_packet_t *pkt)
{
    uint8_t payload[FWU_WRITE_SIZE];
    size_t payload_len = 0;

    if (!fwu_build_reply(pkt->payload, pkt->payload_len, payload, sizeof(payload), &payload_len)) {
        ESP_LOGW(TAG, "FWU rejected, pid=%u", pkt->pid);
        return;
    }

    uint8_t frame[PCG_HEADER_SIZE + FWU_WRITE_SIZE + PCG_CRC_SIZE];
    size_t frame_len = pcg_build(frame, sizeof(frame),
                                 pkt->pid, PCG_PEER_ESP32, 0,
                                 pkt->sender_type, PCG_PORT_FWU,
                                 payload, payload_len);
    if (frame_len == 0) {
        ESP_LOGE(TAG, "FWU reply build failed");
        return;
    }

    if (pkt->sender_type == PCG_PEER_ADVANCE) {
        ESP_LOGI(TAG, "FWU reply -> serial, len=%u", (unsigned)frame_len);
        rtr_rs485_send(frame, frame_len);
        return;
    }

    const char *suffix = mqtt_suffix(pkt->sender_type);
    if (suffix == NULL) {
        ESP_LOGW(TAG, "FWU cannot reply to sender %u", pkt->sender_type);
        return;
    }
    mqtt_publish_frame(suffix, frame, frame_len);
}

static void handle_esp32(const pcg_packet_t *pkt)
{
    if (pkt->receiver_port == PCG_PORT_DIP) {
        handle_dip(pkt->payload, pkt->payload_len);
        return;
    }
    if (pkt->receiver_port == PCG_PORT_FWU) {
        handle_fwu(pkt);
        return;
    }
    ESP_LOGW(TAG, "ESP32 unhandled port %u", pkt->receiver_port);
}

static void on_serial_frame(const uint8_t *frame, size_t len, void *ctx)
{
    pcg_packet_t pkt;
    (void)ctx;

    if (!pcg_parse(frame, len, &pkt)) {
        ESP_LOGW(TAG, "serial parse failed, len=%u", (unsigned)len);
        return;
    }
    if (pkt.sender_type != PCG_PEER_ADVANCE) {
        ESP_LOGW(TAG, "serial sender rejected: %u", pkt.sender_type);
        return;
    }
    if (pkt.receiver_type == PCG_PEER_ESP32) {
        ESP_LOGI(TAG, "serial -> ESP32 port=%u len=%u", pkt.receiver_port, (unsigned)len);
        handle_esp32(&pkt);
        return;
    }

    const char *suffix = mqtt_suffix(pkt.receiver_type);
    if (suffix != NULL) {
        mqtt_publish_frame(suffix, frame, len);
        return;
    }

    ESP_LOGW(TAG, "serial destination ignored: %u", pkt.receiver_type);
}

static void handle_from_mqtt(const uint8_t *data, size_t len)
{
    pcg_packet_t pkt;

    if (!pcg_parse(data, len, &pkt)) {
        ESP_LOGW(TAG, "mqtt parse failed, len=%u", (unsigned)len);
        return;
    }
    if (mqtt_suffix(pkt.sender_type) == NULL) {
        ESP_LOGW(TAG, "mqtt sender rejected: %u", pkt.sender_type);
        return;
    }
    if (pkt.receiver_type == PCG_PEER_ADVANCE) {
        if (pending_push(data, len)) {
            ESP_LOGI(TAG, "mqtt -> pending queue (%d)", s_pending_count);
        }
        return;
    }
    if (pkt.receiver_type == PCG_PEER_ESP32) {
        ESP_LOGI(TAG, "mqtt -> ESP32 port=%u", pkt.receiver_port);
        handle_esp32(&pkt);
        return;
    }

    ESP_LOGW(TAG, "mqtt destination ignored: %u", pkt.receiver_type);
}

static void rx_append(const uint8_t *data, int len);

static void release_armed_synck(void)
{
    uint8_t tmp[PCG_UART_RX_READ_SIZE];
    int n;

    if (!s_synck_armed) {
        return;
    }
    s_synck_armed = false;

    if (s_release_valid) {
        if (CONFIG_PCG_SYNCK_DELAY_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_PCG_SYNCK_DELAY_MS));
        }
        if (rtr_rs485_send(s_release, s_release_len) != ESP_OK) {
            ESP_LOGE(TAG, "SYNCK serial write failed");
        }
        s_release_valid = false;
    }

    while ((n = uart_read_bytes(PCG_UART_NUM, tmp, sizeof(tmp), 0)) > 0) {
        rx_append(tmp, n);
    }
    s_synck_busy = false;
}

static void rx_flush(void)
{
    size_t len;
    size_t consumed = 0;
    size_t frames;

    if (s_rx_len == 0) {
        return;
    }

    len = s_rx_len;
    ESP_LOGD(TAG, "serial chunk %u bytes", (unsigned)len);
    frames = pcg_split_frames(s_rx_acc, len, on_serial_frame, NULL, &consumed);
    if (frames == 0 && len >= PCG_MIN_PACKET_SIZE) {
        ESP_LOGW(TAG, "serial parse failed, %u bytes", (unsigned)len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_rx_acc, len > 64 ? 64 : len, ESP_LOG_WARN);
    } else if (frames > 1) {
        ESP_LOGI(TAG, "serial split %u frames", (unsigned)frames);
    }

    if (consumed < len) {
        memmove(s_rx_acc, s_rx_acc + consumed, len - consumed);
        s_rx_len = len - consumed;
    } else {
        s_rx_len = 0;
    }
    s_rx_scanned = s_rx_len;
}

static void rx_append(const uint8_t *data, int len)
{
    size_t left = (size_t)len;

    if (len <= 0 || data == NULL) {
        return;
    }

    while (left > 0) {
        size_t space;
        size_t take;

        if (s_rx_len == sizeof(s_rx_acc)) {
            ESP_LOGW(TAG, "RX coalesce full, flushing");
            rx_flush();
        }

        space = sizeof(s_rx_acc) - s_rx_len;
        if (space == 0) {
            ESP_LOGW(TAG, "RX coalesce overflow, dropping buffer");
            s_rx_len = 0;
            s_rx_scanned = 0;
            space = sizeof(s_rx_acc);
        }
        take = left < space ? left : space;
        memcpy(s_rx_acc + s_rx_len, data, take);
        s_rx_len += take;
        data += take;
        left -= take;
        s_rx_last = xTaskGetTickCount();
    }
}

static void pcgtask(void *arg)
{
    QueueHandle_t data_queue = mqtt_data_queue_get();
    s_client = (esp_mqtt_client_handle_t)arg;

    if (s_client == NULL || data_queue == NULL) {
        ESP_LOGE(TAG, "invalid task context");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "started, device serial=%u", (unsigned)deviceSerial);

    while (1) {
        mqtt_data_msg_t msg;
        TickType_t wait = pdMS_TO_TICKS(50);

        if (s_rx_len > s_rx_scanned) {
            TickType_t elapsed = xTaskGetTickCount() - s_rx_last;
            TickType_t need = pdMS_TO_TICKS(CONFIG_PCG_RX_FLUSH_MS);
            wait = (elapsed >= need) ? 0 : (need - elapsed);
        }

        if (xQueueReceive(data_queue, &msg, wait) == pdTRUE) {
            handle_from_mqtt(msg.data, (size_t)msg.data_len);
        }

        uint8_t rx_tmp[PCG_UART_RX_READ_SIZE];
        int rx_len;
        while ((rx_len = uart_read_bytes(PCG_UART_NUM, rx_tmp, sizeof(rx_tmp), 0)) > 0) {
            rx_append(rx_tmp, rx_len);
        }

        if (s_rx_len > s_rx_scanned &&
            (xTaskGetTickCount() - s_rx_last) >= pdMS_TO_TICKS(CONFIG_PCG_RX_FLUSH_MS)) {
            rx_flush();
        }

        if (s_synck_armed) {
            release_armed_synck();
        }
    }
}

esp_err_t pcgtask_start(esp_mqtt_client_handle_t client)
{
    if (fwu_init() != ESP_OK) {
        ESP_LOGW(TAG, "FWU storage disabled");
    }

    BaseType_t created = xTaskCreatePinnedToCore(pcgtask, "pcgtask", 8192, client, 5, NULL, 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create pcgtask");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
