#include "pcg_dip.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "mqtt_data.h"
#include "pcg_rtr.h"

static const char *TAG = "pcg_dip";

/* Expected DIP payload length: serial (2) + command (1). */
#define DIP_PAYLOAD_SIZE 3
/* DIP command that requests release of one pending MQTT->RS485 frame. */
#define DIP_CMD_SYNCK_REQUEST 1

/* True while a SYNCK release is in progress (blocks a second SYNCK). */
static bool s_synck_busy;
/* True when a pending frame is ready to send on RS485 after SYNCK. */
static bool s_synck_armed;
/* Frame popped from the pending queue for the current SYNCK release. */
static uint8_t s_release[MQTT_PAYLOAD_MAX_LEN];
/* Length of s_release. */
static size_t s_release_len;
/* True when s_release holds a valid frame to send. */
static bool s_release_valid;

/* One queued MQTT frame waiting for Advance SYNCK before RS485 TX. */
typedef struct {
    uint8_t data[MQTT_PAYLOAD_MAX_LEN];
    uint16_t len;
} pending_slot_t;

/* Queue of MQTT frames destined for Advance (held until SYNCK). */
static pending_slot_t s_pending[CONFIG_PCG_QUEUE_DEPTH];
/* Number of valid entries in s_pending. */
static int s_pending_count;

/* Dequeue one pending frame into out (FIFO or LIFO per config). */
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

bool pcg_dip_queue_push(const uint8_t *data, size_t len)
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
    ESP_LOGI(TAG, "mqtt -> pending queue (%d)", s_pending_count);
    return true;
}

void pcg_dip_handle(const uint8_t *payload, size_t len)
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

bool pcg_dip_armed(void)
{
    return s_synck_armed;
}

void pcg_dip_release(pcg_dip_rx_sink_t rx_sink)
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

    if (rx_sink != NULL) {
        while ((n = uart_read_bytes(PCG_UART_NUM, tmp, sizeof(tmp), 0)) > 0) {
            rx_sink(tmp, n);
        }
    }
    s_synck_busy = false;
}
