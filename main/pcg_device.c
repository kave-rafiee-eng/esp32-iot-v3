#include "pcg_device.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mqtt_data.h"
#include "gpio_output.h"
#include "pcg_protocol.h"
#include "pcg_uart.h"

static const char *TAG = "pcg_device";

#define PCG_MQTT_RS485_DELAY_MS 20
#define PCG_SERIAL_LED_BLINK_MS 500

pcg_process_result_t pcg_device_handle(const pcg_device_frame_t *frame,
                                         const pcg_process_ctx_t *ctx) {
  if (frame == NULL || ctx == NULL) {
    return PCG_PROCESS_ERR_NULL;
  }

  ESP_LOGI(TAG, "device id=0x%04X request=%u", frame->device_id,
           frame->request);

  if (frame->request != PCG_DEVICE_REQUEST_SYNC || ctx->mqtt_queue == NULL) {
    return PCG_PROCESS_OK;
  }

  mqtt_data_msg_t msg;
  if (xQueueReceive(ctx->mqtt_queue, &msg, 0) != pdTRUE) {
    return PCG_PROCESS_OK;
  }

  vTaskDelay(pdMS_TO_TICKS(PCG_MQTT_RS485_DELAY_MS));
  pcg_uart_flush_rx();
  if (pcg_rs485_send((const uint8_t *)msg.data, (size_t)msg.data_len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "RS485 send failed after sync request");
    return PCG_PROCESS_ERR_NULL;
  }

  ESP_LOGI(TAG, "sent queued mqtt data to RS485, len=%d", msg.data_len);
  return PCG_PROCESS_OK;
}

static bool pcg_device_frame_matches(const uint8_t *frame) {
  return frame[PCG_OFF_SENDER_ID] == PCG_ADDR_ADVANCE &&
         frame[PCG_OFF_RECEIVER_ID] == PCG_ADDR_ESP32 &&
         frame[PCG_OFF_PORT] == PCG_PORT_DEVICE;
}

uint16_t pcg_read_serial_device(void) {
  uint8_t rx_buf[PCG_UART_RX_READ_SIZE];
  TickType_t last_blink = xTaskGetTickCount();

  while (1) {
    TickType_t now = xTaskGetTickCount();
    if (now - last_blink >= pdMS_TO_TICKS(PCG_SERIAL_LED_BLINK_MS)) {
      gpio_led_toggle(GPIO_LED_1);
      last_blink = now;
    }

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
    gpio_led_set(GPIO_LED_1, 1);
    return device_id;
  }
}
