#include "pcg_rtr.h"

#include <stdlib.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

typedef enum {
    def_PCG_ADD_ADVANCE = 1,
    def_PCG_ADD_ESP32   = 2,
    def_PCG_ADD_CLIENT  = 3,
} def_PCG_ADD;

typedef enum {
    def_PCG_PORT_RTR = 1,
} def_PCG_PORT;

typedef enum {
    def_PCG_RTR_MODE_READ  = 1,
    def_PCG_RTR_MODE_WRITE = 2,
} def_PCG_RTR_MODE;

typedef enum {
    RTR_PARSE_OK = 0,
    RTR_PARSE_ERR_NULL_VALUE,
    RTR_PARSE_ERR_WRONG_LEN,
    RTR_PARSE_ERR_CRC,
    RTR_PARSE_ERR_SENDER_ID,
    RTR_PARSE_ERR_RECEIVER_ID,
    RTR_PARSE_ERR_PORT,
    RTR_PARSE_ERR_PID,
    RTR_PARSE_ERR_MODE,
    RTR_PARSE_ERR_REG_COUNT,
    RTR_PARSE_ERR_ADDRESS,
} rtr_parse_result_t;



#define RTR_POLL_INTERVAL_MS      10
#define RTR_RESEND_INTERVAL_MS    1000
#define RTR_UART_RX_TOUT_SYMBOLS  3

#define CRC_SIZE                  2
#define PCG_HEADER_SIZE           3   /* sender_id, receiver_id, port */
#define RTR_HEADER_SIZE           3   /* pid, mode, register_count */
#define RTR_REGISTER_READ_SIZE    2   /* address only */
#define RTR_REGISTER_WRITE_SIZE   4   /* address + value */

#define RTR_READ_REQUEST_SIZE     (PCG_HEADER_SIZE + RTR_HEADER_SIZE + RTR_REGISTER_READ_SIZE + CRC_SIZE)
#define RTR_READ_RESPONSE_SIZE    (PCG_HEADER_SIZE + RTR_HEADER_SIZE + RTR_REGISTER_WRITE_SIZE + CRC_SIZE)

#define RTR_OFF_SENDER_ID         0
#define RTR_OFF_RECEIVER_ID       1
#define RTR_OFF_PORT              2
#define RTR_OFF_PID               3
#define RTR_OFF_MODE              4
#define RTR_OFF_REG_COUNT         5
#define RTR_OFF_ADDR_HI           6
#define RTR_OFF_ADDR_LO           7
#define RTR_OFF_VALUE_HI          8
#define RTR_OFF_VALUE_LO          9

static const char *TAG = "pcg_rtr";

static void rtr_append_crc(uint8_t *buf, size_t payload_len)
{
    uint16_t crc = CRC_16(buf, payload_len);
    buf[payload_len]     = crc & 0xff;
    buf[payload_len + 1] = (crc >> 8) & 0xff;
}

static size_t rtr_build_read_request(uint8_t *buf, uint16_t addr, uint8_t pid)
{
    buf[RTR_OFF_SENDER_ID]   = def_PCG_ADD_ESP32;
    buf[RTR_OFF_RECEIVER_ID] = def_PCG_ADD_ADVANCE;
    buf[RTR_OFF_PORT]        = def_PCG_PORT_RTR;
    buf[RTR_OFF_PID]         = pid;
    buf[RTR_OFF_MODE]        = def_PCG_RTR_MODE_READ;
    buf[RTR_OFF_REG_COUNT]   = 1;
    buf[RTR_OFF_ADDR_HI]     = (addr >> 8) & 0xff;
    buf[RTR_OFF_ADDR_LO]     = addr & 0xff;

    rtr_append_crc(buf, PCG_HEADER_SIZE + RTR_HEADER_SIZE + RTR_REGISTER_READ_SIZE);
    return RTR_READ_REQUEST_SIZE;
}

static rtr_parse_result_t rtr_parse_read_response(const uint8_t *data, size_t len,
                                                  uint16_t expected_addr, uint8_t expected_pid,
                                                  uint16_t *value)
{
    if (value == NULL) {
        ESP_LOGE(TAG, "parse: value pointer is NULL");
        return RTR_PARSE_ERR_NULL_VALUE;
    }

    if (len != RTR_READ_RESPONSE_SIZE) {
        ESP_LOGW(TAG, "parse: wrong length, got %u, expected %u", (unsigned)len, RTR_READ_RESPONSE_SIZE);
        return RTR_PARSE_ERR_WRONG_LEN;
    }

    uint16_t rx_crc = data[len - 2] | (data[len - 1] << 8);
    uint16_t calc_crc = CRC_16((uint8_t *)data, len - 2);
    if (calc_crc != rx_crc) {
        ESP_LOGW(TAG, "parse: CRC mismatch, rx=0x%04X calc=0x%04X", rx_crc, calc_crc);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_WARN);
        return RTR_PARSE_ERR_CRC;
    }

    uint8_t sender_id   = data[RTR_OFF_SENDER_ID];
    uint8_t receiver_id = data[RTR_OFF_RECEIVER_ID];
    uint8_t port        = data[RTR_OFF_PORT];
    uint8_t pid         = data[RTR_OFF_PID];
    uint8_t mode        = data[RTR_OFF_MODE];
    uint8_t reg_count   = data[RTR_OFF_REG_COUNT];

    if (sender_id != def_PCG_ADD_ADVANCE) {
        ESP_LOGW(TAG, "parse: sender ID mismatch, got %u, expected %u",
                 sender_id, def_PCG_ADD_ADVANCE);
        return RTR_PARSE_ERR_SENDER_ID;
    }

    if (receiver_id != def_PCG_ADD_ESP32) {
        ESP_LOGW(TAG, "parse: receiver ID mismatch, got %u, expected %u",
                 receiver_id, def_PCG_ADD_ESP32);
        return RTR_PARSE_ERR_RECEIVER_ID;
    }

    if (port != def_PCG_PORT_RTR) {
        ESP_LOGW(TAG, "parse: port mismatch, got %u, expected %u", port, def_PCG_PORT_RTR);
        return RTR_PARSE_ERR_PORT;
    }

    if (pid != expected_pid) {
        ESP_LOGW(TAG, "parse: PID mismatch, got 0x%02X, expected 0x%02X", pid, expected_pid);
        return RTR_PARSE_ERR_PID;
    }

    if (mode != def_PCG_RTR_MODE_WRITE) {
        ESP_LOGW(TAG, "parse: mode mismatch, got %u, expected %u", mode, def_PCG_RTR_MODE_WRITE);
        return RTR_PARSE_ERR_MODE;
    }

    if (reg_count != 1) {
        ESP_LOGW(TAG, "parse: register count mismatch, got %u, expected 1", reg_count);
        return RTR_PARSE_ERR_REG_COUNT;
    }

    uint16_t rx_addr = (data[RTR_OFF_ADDR_HI] << 8) | data[RTR_OFF_ADDR_LO];
    if (rx_addr != expected_addr) {
        ESP_LOGW(TAG, "parse: address mismatch, got 0x%04X, expected 0x%04X", rx_addr, expected_addr);
        return RTR_PARSE_ERR_ADDRESS;
    }

    *value = (data[RTR_OFF_VALUE_HI] << 8) | data[RTR_OFF_VALUE_LO];
    return RTR_PARSE_OK;
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

    uint8_t tx_buf[RTR_READ_REQUEST_SIZE];
    uint8_t rx_buf[PCG_UART_RX_READ_SIZE];
    uint8_t pid = rand() & 0xff;
    size_t tx_len = rtr_build_read_request(tx_buf, add, pid);
    bool wait_forever = (timeout_ms == 0);

    ESP_LOGI(TAG, "read reg 0x%04X started, pid=0x%02X, timeout=%s",
             add, pid, wait_forever ? "infinite" : "limited");

    rtr_uart_flush_rx();
    if (rtr_rs485_send(tx_buf, tx_len) != ESP_OK) {
        ESP_LOGE(TAG, "read reg 0x%04X: RS485 send failed", add);
        return false;
    }

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

        int len = uart_read_bytes(PCG_UART_NUM, rx_buf, sizeof(rx_buf), 0);
        if (len <= 0) {
            continue;
        }

        ESP_LOGD(TAG, "read reg 0x%04X: received %d bytes", add, len);

        rtr_parse_result_t result = rtr_parse_read_response(rx_buf, (size_t)len, add, pid, value);
        if (result == RTR_PARSE_OK) {
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

unsigned int CRC_16(uint8_t *buf, int len)
{
    unsigned int crc = 0xFFFF;

    for (int pos = 0; pos < len; pos++) {
        crc ^= (unsigned int)buf[pos];

        for (int i = 8; i != 0; i--) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}
