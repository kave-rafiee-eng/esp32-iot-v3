#include "pcg_device.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pcg_protocol.h"
#include "pcg_uart.h"

static const char *TAG = "pcg_device";

static bool pcg_device_frame_matches(const uint8_t *frame) {
  return frame[PCG_OFF_SENDER_ID] == PCG_ADDR_ADVANCE &&
         frame[PCG_OFF_RECEIVER_ID] == PCG_ADDR_ESP32 &&
         frame[PCG_OFF_PORT] == PCG_PORT_DEVICE;
}

uint16_t pcg_read_serial_device(void) {
  uint8_t rx_buf[PCG_UART_RX_READ_SIZE];

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(PCG_DEVICE_POLL_MS));

    int len = uart_read_bytes(PCG_UART_NUM, rx_buf, sizeof(rx_buf), 0);
    if (len <= 0) {
      continue;
    }

    if ((size_t)len < PCG_DEVICE_MIN_FRAME_SIZE) {
      ESP_LOGW(TAG, "frame too short, len=%d", len);
      continue;
    }

    if (!pcg_check_crc(rx_buf, (size_t)len, TAG)) {
      continue;
    }

    if (!pcg_device_frame_matches(rx_buf)) {
      continue;
    }

    uint16_t device_id =
        (rx_buf[PCG_OFF_DEVICE_ID_HI] << 8) | rx_buf[PCG_OFF_DEVICE_ID_LO];
    uint8_t device_request = rx_buf[PCG_OFF_DEVICE_REQUEST];

    ESP_LOGI(TAG, "device id=0x%04X request=%u", device_id, device_request);
    return device_id;
  }
}
