#include "pcg_rtr.h"

#include <stdlib.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pcg_uart.h"

static const char *TAG = "pcg_rtr";

static void rtr_append_crc(uint8_t *buf, size_t payload_len) {
  uint16_t crc = pcg_crc16(buf, payload_len);
  buf[payload_len] = crc & 0xff;
  buf[payload_len + 1] = (crc >> 8) & 0xff;
}

static size_t rtr_build_read_request(uint8_t *buf, uint16_t addr, uint8_t pid) {
  buf[PCG_OFF_SENDER_ID] = PCG_ADDR_ESP32;
  buf[PCG_OFF_RECEIVER_ID] = PCG_ADDR_ADVANCE;
  buf[PCG_OFF_PORT] = PCG_PORT_RTR;
  buf[RTR_OFF_PID] = pid;
  buf[RTR_OFF_MODE] = PCG_RTR_MODE_READ;
  buf[RTR_OFF_REG_COUNT] = 1;
  buf[RTR_OFF_ADDR_HI] = (addr >> 8) & 0xff;
  buf[RTR_OFF_ADDR_LO] = addr & 0xff;

  rtr_append_crc(buf,
                 PCG_HEADER_SIZE + RTR_HEADER_SIZE + RTR_REGISTER_READ_SIZE);
  return RTR_READ_REQUEST_SIZE;
}

static rtr_parse_result_t
rtr_parse_read_response(const uint8_t *data, size_t len, uint16_t expected_addr,
                        uint8_t expected_pid, uint16_t *value) {
  if (value == NULL) {
    ESP_LOGE(TAG, "parse: value pointer is NULL");
    return RTR_PARSE_ERR_NULL_VALUE;
  }

  if (len != RTR_READ_RESPONSE_SIZE) {
    ESP_LOGW(TAG, "parse: wrong length, got %u, expected %u", (unsigned)len,
             RTR_READ_RESPONSE_SIZE);
    return RTR_PARSE_ERR_WRONG_LEN;
  }

  if (!pcg_check_crc(data, len, TAG)) {
    return RTR_PARSE_ERR_CRC;
  }

  uint8_t sender_id = data[PCG_OFF_SENDER_ID];
  uint8_t receiver_id = data[PCG_OFF_RECEIVER_ID];
  uint8_t port = data[PCG_OFF_PORT];
  uint8_t pid = data[RTR_OFF_PID];
  uint8_t mode = data[RTR_OFF_MODE];
  uint8_t reg_count = data[RTR_OFF_REG_COUNT];

  if (sender_id != PCG_ADDR_ADVANCE) {
    ESP_LOGW(TAG, "parse: sender ID mismatch, got %u, expected %u", sender_id,
             PCG_ADDR_ADVANCE);
    return RTR_PARSE_ERR_SENDER_ID;
  }

  if (receiver_id != PCG_ADDR_ESP32) {
    ESP_LOGW(TAG, "parse: receiver ID mismatch, got %u, expected %u",
             receiver_id, PCG_ADDR_ESP32);
    return RTR_PARSE_ERR_RECEIVER_ID;
  }

  if (port != PCG_PORT_RTR) {
    ESP_LOGW(TAG, "parse: port mismatch, got %u, expected %u", port,
             PCG_PORT_RTR);
    return RTR_PARSE_ERR_PORT;
  }

  if (pid != expected_pid) {
    ESP_LOGW(TAG, "parse: PID mismatch, got 0x%02X, expected 0x%02X", pid,
             expected_pid);
    return RTR_PARSE_ERR_PID;
  }

  if (mode != PCG_RTR_MODE_WRITE) {
    ESP_LOGW(TAG, "parse: mode mismatch, got %u, expected %u", mode,
             PCG_RTR_MODE_WRITE);
    return RTR_PARSE_ERR_MODE;
  }

  if (reg_count != 1) {
    ESP_LOGW(TAG, "parse: register count mismatch, got %u, expected 1",
             reg_count);
    return RTR_PARSE_ERR_REG_COUNT;
  }

  uint16_t rx_addr = (data[RTR_OFF_ADDR_HI] << 8) | data[RTR_OFF_ADDR_LO];
  if (rx_addr != expected_addr) {
    ESP_LOGW(TAG, "parse: address mismatch, got 0x%04X, expected 0x%04X",
             rx_addr, expected_addr);
    return RTR_PARSE_ERR_ADDRESS;
  }

  *value = (data[RTR_OFF_VALUE_HI] << 8) | data[RTR_OFF_VALUE_LO];
  return RTR_PARSE_OK;
}

bool rtrReadRegister(uint16_t add, uint16_t *value, uint32_t timeout_ms) {
  if (value == NULL) {
    ESP_LOGE(TAG, "read reg 0x%04X: value pointer is NULL", add);
    return false;
  }

  uint8_t tx_buf[RTR_READ_REQUEST_SIZE];
  uint8_t rx_buf[PCG_UART_RX_READ_SIZE];
  uint8_t pid = rand() & 0xff;
  size_t tx_len = rtr_build_read_request(tx_buf, add, pid);
  bool wait_forever = (timeout_ms == 0);

  ESP_LOGI(TAG, "read reg 0x%04X started, pid=0x%02X, timeout=%s", add, pid,
           wait_forever ? "infinite" : "limited");

  pcg_uart_flush_rx();
  if (pcg_rs485_send(tx_buf, tx_len) != ESP_OK) {
    ESP_LOGE(TAG, "read reg 0x%04X: RS485 send failed", add);
    return false;
  }

  TickType_t start_tick = xTaskGetTickCount();
  TickType_t deadline =
      wait_forever ? 0 : start_tick + pdMS_TO_TICKS(timeout_ms);
  TickType_t next_resend = start_tick + pdMS_TO_TICKS(RTR_RESEND_INTERVAL_MS);
  uint32_t resend_count = 0;

  while (wait_forever || xTaskGetTickCount() < deadline) {
    if (xTaskGetTickCount() >= next_resend) {
      resend_count++;
      ESP_LOGW(
          TAG,
          "read reg 0x%04X: no valid response, resending (#%lu, pid=0x%02X)",
          add, (unsigned long)resend_count, pid);
      if (pcg_rs485_send(tx_buf, tx_len) != ESP_OK) {
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

    rtr_parse_result_t result =
        rtr_parse_read_response(rx_buf, (size_t)len, add, pid, value);
    if (result == RTR_PARSE_OK) {
      ESP_LOGI(TAG, "read reg 0x%04X success, value=0x%04X (%u)", add, *value,
               *value);
      return true;
    }
  }

  ESP_LOGW(TAG, "read reg 0x%04X timed out after %lu ms (%lu resends)", add,
           (unsigned long)timeout_ms, (unsigned long)resend_count);
  return false;
}
