#include "pcgtask.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "fwu.h"
#include "fwu_handle.h"
#include "mqtt_data.h"
#include "pcg.h"
#include "pcg_dip.h"
#include "pcg_rtr.h"

/* Log tag for this gateway task. */
static const char *TAG = "pcgtask";

/* Coalesce buffer size for UART RX bytes before frame split. */
#define PCG_RX_ACC_SIZE 2048

/* Device serial used in MQTT topics; defined elsewhere. */
extern uint16_t deviceSerial;

/* Shared MQTT client handle for publishes from this task. */
static esp_mqtt_client_handle_t s_client;

/* Accumulator for coalescing UART RX bytes into a parseable chunk. */
static uint8_t s_rx_acc[PCG_RX_ACC_SIZE];
/* Current number of bytes stored in s_rx_acc. */
static size_t s_rx_len;
/* Bytes in s_rx_acc already processed by the last rx_flush. */
static size_t s_rx_scanned;
/* Tick of the last byte appended to s_rx_acc (idle-flush timer). */
static TickType_t s_rx_last;

/* Map a PCG peer type to its MQTT topic suffix, or NULL if unknown. */
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

/* Publish a raw PCG frame to "{deviceSerial}/{suffix}". */
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

/* FWU MQTT reply callback: peer type -> topic suffix -> publish. */
static void fwu_mqtt_publish(uint8_t peer, const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    const char *suffix = mqtt_suffix(peer);
    if (suffix == NULL) {
        ESP_LOGW(TAG, "FWU cannot reply to sender %u", peer);
        return;
    }
    mqtt_publish_frame(suffix, frame, len);
}

/* Dispatch a frame addressed to the ESP32 by receiver port (DIP / FWU). */
static void handle_esp32(const pcg_packet_t *pkt)
{
    if (pkt->receiver_port == PCG_PORT_DIP) {
        pcg_dip_handle(pkt->payload, pkt->payload_len);
        return;
    }
    if (pkt->receiver_port == PCG_PORT_FWU) {
        fwu_handle_packet(pkt, fwu_mqtt_publish, NULL);
        return;
    }
    ESP_LOGW(TAG, "ESP32 unhandled port %u", pkt->receiver_port);
}

/* Callback for each complete PCG frame split from the UART coalesce buffer. */
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

/* Route an inbound MQTT payload: queue for Advance, or handle locally. */
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
        pcg_dip_queue_push(data, len);
        return;
    }
    if (pkt.receiver_type == PCG_PEER_ESP32) {
        ESP_LOGI(TAG, "mqtt -> ESP32 port=%u", pkt.receiver_port);
        handle_esp32(&pkt);
        return;
    }

    ESP_LOGW(TAG, "mqtt destination ignored: %u", pkt.receiver_type);
}

/* Split coalesced UART bytes into PCG frames and keep any incomplete tail. */
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

/* Append UART bytes to s_rx_acc; flush early if the buffer is full. */
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

/* Main gateway loop: MQTT queue, UART coalesce/flush, and SYNCK release. */
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

        if (pcg_dip_armed()) {
            pcg_dip_release(rx_append);
        }
    }
}

/* Init FWU storage and start the gateway task pinned to core 1. */
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
