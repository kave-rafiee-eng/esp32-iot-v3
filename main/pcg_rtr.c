#include "pcg_rtr.h"

#include "pcg.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define RTR_MODE_READ             1
#define RTR_MODE_WRITE            2

#define RTR_POLL_INTERVAL_MS      10
#define RTR_RESEND_INTERVAL_MS    1000
#define RTR_UART_RX_TOUT_SYMBOLS  3

static const char *TAG = "pcg_rtr";

typedef struct {
    uint16_t addr;
    uint8_t pid;
    uint16_t *value;
    bool found;
} rtr_wait_t;

static bool rtr_take_write_value(const uint8_t *payload, size_t len,
                                 uint16_t addr, uint16_t *value)
{
    if (payload == NULL || value == NULL || len < 2) {
        return false;
    }
    if (payload[0] != RTR_MODE_WRITE) {
        return false;
    }

    uint8_t count = payload[1];
    if (len != (size_t)(2 + count * 4)) {
        return false;
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t off = 2 + (size_t)i * 4;
        uint16_t got = ((uint16_t)payload[off] << 8) | payload[off + 1];
        if (got == addr) {
            *value = ((uint16_t)payload[off + 2] << 8) | payload[off + 3];
            return true;
        }
    }

    return false;
}

static void rtr_on_frame(const uint8_t *frame, size_t len, void *ctx)
{
    rtr_wait_t *wait = ctx;
    pcg_packet_t pkt;

    if (wait->found) {
        return;
    }
    if (!pcg_parse(frame, len, &pkt)) {
        return;
    }
    if (pkt.pid != wait->pid || pkt.receiver_port != PCG_PORT_RTR) {
        return;
    }
    if (pkt.sender_type != PCG_PEER_ADVANCE) {
        return;
    }
    if (!rtr_take_write_value(pkt.payload, pkt.payload_len, wait->addr, wait->value)) {
        return;
    }

    wait->found = true;
}

void rtr_uart_flush_rx(void)
{
    uint8_t discard[PCG_UART_RX_READ_SIZE];
    int flushed = 0;

    while (1) {
        int len = uart_read_bytes(PCG_UART_NUM, discard, sizeof(discard), 0);
        if (len <= 0) {
            break;
        }
        flushed += len;
    }

    if (flushed > 0) {
        ESP_LOGD(TAG, "flushed %d stale RX bytes", flushed);
    }
}

esp_err_t rtr_rs485_send(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        ESP_LOGE(TAG, "RS485 send: invalid buffer");
        return ESP_ERR_INVALID_ARG;
    }

    int written = uart_write_bytes(PCG_UART_NUM, data, len);
    if (written < 0 || (size_t)written != len) {
        ESP_LOGE(TAG, "RS485 send failed, written=%d, expected=%u", written, (unsigned)len);
        return ESP_FAIL;
    }

    esp_err_t err = uart_wait_tx_done(PCG_UART_NUM, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RS485 wait TX done failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

bool rtrReadRegister(uint16_t add, uint16_t *value, uint32_t timeout_ms)
{
    if (value == NULL) {
        ESP_LOGE(TAG, "read reg 0x%04X: value pointer is NULL", add);
        return false;
    }

    uint8_t payload[4] = {
        RTR_MODE_READ,
        1,
        (uint8_t)((add >> 8) & 0xff),
        (uint8_t)(add & 0xff),
    };
    uint8_t pid = (uint8_t)(esp_random() & 0xff);
    uint8_t tx_buf[PCG_MIN_PACKET_SIZE + sizeof(payload)];
    size_t tx_len = pcg_build(tx_buf, sizeof(tx_buf), pid,
                              PCG_PEER_ESP32, 0,
                              PCG_PEER_ADVANCE, PCG_PORT_RTR,
                              payload, sizeof(payload));
    if (tx_len == 0) {
        ESP_LOGE(TAG, "read reg 0x%04X: failed to build request", add);
        return false;
    }

    bool wait_forever = (timeout_ms == 0);
    ESP_LOGI(TAG, "read reg 0x%04X started, pid=0x%02X, timeout=%s",
             add, pid, wait_forever ? "infinite" : "limited");

    rtr_uart_flush_rx();
    if (rtr_rs485_send(tx_buf, tx_len) != ESP_OK) {
        ESP_LOGE(TAG, "read reg 0x%04X: RS485 send failed", add);
        return false;
    }

    uint8_t acc[128];
    size_t acc_len = 0;
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t deadline = wait_forever ? 0 : start_tick + pdMS_TO_TICKS(timeout_ms);
    TickType_t next_resend = start_tick + pdMS_TO_TICKS(RTR_RESEND_INTERVAL_MS);
    uint32_t resend_count = 0;

    while (wait_forever || xTaskGetTickCount() < deadline) {
        if (xTaskGetTickCount() >= next_resend) {
            resend_count++;
            ESP_LOGW(TAG, "read reg 0x%04X: no valid response, resending (#%lu, pid=0x%02X)",
                     add, (unsigned long)resend_count, pid);
            if (rtr_rs485_send(tx_buf, tx_len) != ESP_OK) {
                ESP_LOGE(TAG, "read reg 0x%04X: RS485 resend failed", add);
            }
            next_resend = xTaskGetTickCount() + pdMS_TO_TICKS(RTR_RESEND_INTERVAL_MS);
        }

        vTaskDelay(pdMS_TO_TICKS(RTR_POLL_INTERVAL_MS));

        uint8_t rx_tmp[64];
        int len = uart_read_bytes(PCG_UART_NUM, rx_tmp, sizeof(rx_tmp), 0);
        if (len <= 0) {
            continue;
        }

        if (acc_len + (size_t)len > sizeof(acc)) {
            ESP_LOGW(TAG, "read reg 0x%04X: RX accumulator overflow, resetting", add);
            acc_len = 0;
        }
        memcpy(acc + acc_len, rx_tmp, (size_t)len);
        acc_len += (size_t)len;

        rtr_wait_t wait = {
            .addr = add,
            .pid = pid,
            .value = value,
            .found = false,
        };
        pcg_split_frames(acc, acc_len, rtr_on_frame, &wait, NULL);
        if (wait.found) {
            ESP_LOGI(TAG, "read reg 0x%04X success, value=0x%04X (%u)", add, *value, *value);
            return true;
        }
    }

    ESP_LOGW(TAG, "read reg 0x%04X timed out after %lu ms (%lu resends)",
             add, (unsigned long)timeout_ms, (unsigned long)resend_count);
    return false;
}

esp_err_t pcg_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = PCG_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(PCG_UART_NUM, PCG_UART_RX_BUF_SIZE, PCG_UART_TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    err = uart_param_config(PCG_UART_NUM, &uart_config);
    if (err != ESP_OK) {
        uart_driver_delete(PCG_UART_NUM);
        return err;
    }

    err = uart_set_pin(PCG_UART_NUM, CONFIG_PCG_UART_TX_PIN, CONFIG_PCG_UART_RX_PIN,
                       CONFIG_GPIO_OUTPUT_PIN, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        uart_driver_delete(PCG_UART_NUM);
        return err;
    }

    err = uart_set_mode(PCG_UART_NUM, UART_MODE_RS485_HALF_DUPLEX);
    if (err != ESP_OK) {
        uart_driver_delete(PCG_UART_NUM);
        return err;
    }

    err = uart_set_rx_timeout(PCG_UART_NUM, RTR_UART_RX_TOUT_SYMBOLS);
    if (err != ESP_OK) {
        uart_driver_delete(PCG_UART_NUM);
        return err;
    }

    ESP_LOGI(TAG, "UART%d init OK (RS485 half-duplex), baud=%d, TX=GPIO%d, RX=GPIO%d, DE/RE(RTS)=GPIO%d",
             PCG_UART_NUM, PCG_UART_BAUD_RATE, CONFIG_PCG_UART_TX_PIN, CONFIG_PCG_UART_RX_PIN,
             CONFIG_GPIO_OUTPUT_PIN);
    return ESP_OK;
}
