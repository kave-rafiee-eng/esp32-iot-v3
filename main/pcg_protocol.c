#include "pcg_protocol.h"

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mqtt_data.h"
#include "pcg_uart.h"

static const char *TAG = "pcg_protocol";

uint16_t pcg_crc16(const uint8_t *buf, size_t len) {
  uint16_t crc = 0xFFFF;

  for (size_t pos = 0; pos < len; pos++) {
    crc ^= buf[pos];

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

bool pcg_check_crc(const uint8_t *data, size_t data_len, const char *log_tag) {
  if (data_len < PCG_CRC_SIZE) {
    return false;
  }

  uint16_t rx_crc = data[data_len - 2] | (data[data_len - 1] << 8);
  uint16_t calc_crc = pcg_crc16(data, data_len - PCG_CRC_SIZE);

  if (calc_crc != rx_crc) {
    if (log_tag != NULL) {
      ESP_LOGW(log_tag, "CRC mismatch, rx=0x%04X calc=0x%04X", rx_crc,
               calc_crc);
      ESP_LOG_BUFFER_HEX_LEVEL(log_tag, data, data_len, ESP_LOG_WARN);
    }
    return false;
  }

  return true;
}

pcg_process_result_t pcg_process(const uint8_t *data, size_t len,
                                 esp_mqtt_client_handle_t mqtt_client,
                                 uint16_t device_serial,
                                 QueueHandle_t mqtt_queue) {
  if (data == NULL) {
    return PCG_PROCESS_ERR_NULL;
  }

  if (len < PCG_MIN_FRAME_SIZE) {
    ESP_LOGW(TAG, "frame too short, len=%u", (unsigned)len);
    return PCG_PROCESS_ERR_TOO_SHORT;
  }

  if (!pcg_check_crc(data, len, TAG)) {
    return PCG_PROCESS_ERR_CRC;
  }

  uint8_t sender_id = data[PCG_OFF_SENDER_ID];
  if (sender_id != PCG_ADDR_ADVANCE) {
    ESP_LOGW(TAG, "sender ID mismatch, got %u expected %u", sender_id,
             PCG_ADDR_ADVANCE);
    return PCG_PROCESS_ERR_SENDER_ID;
  }

  uint8_t receiver_id = data[PCG_OFF_RECEIVER_ID];
  if (receiver_id == PCG_ADDR_ESP32) {
    uint8_t port = data[PCG_OFF_PORT];

    if (port != PCG_PORT_DEVICE) {
      ESP_LOGW(TAG, "port mismatch for ESP32, got %u", port);
      return PCG_PROCESS_ERR_PORT;
    }

    if (len < PCG_DEVICE_MIN_FRAME_SIZE) {
      ESP_LOGW(TAG, "device frame too short, len=%u", (unsigned)len);
      return PCG_PROCESS_ERR_TOO_SHORT;
    }

    uint8_t request = data[PCG_OFF_DEVICE_REQUEST];
    if (request == PCG_DEVICE_REQUEST_SYNC && mqtt_queue != NULL) {
      mqtt_data_msg_t msg;
      if (xQueueReceive(mqtt_queue, &msg, 0) == pdTRUE) {
        vTaskDelay(pdMS_TO_TICKS(PCG_MQTT_RS485_DELAY_MS));
        pcg_uart_flush_rx();
        if (pcg_rs485_send((const uint8_t *)msg.data, (size_t)msg.data_len) !=
            ESP_OK) {
          ESP_LOGE(TAG, "RS485 send failed after sync request");
        } else {
          ESP_LOGI(TAG, "sent queued mqtt data to RS485, len=%d", msg.data_len);
        }
      }
    }

    return PCG_PROCESS_OK;
  }

  if (receiver_id == PCG_ADDR_CLIENT) {
    if (mqtt_client == NULL) {
      return PCG_PROCESS_ERR_NULL;
    }

    char topic[64];
    snprintf(topic, sizeof(topic), "%u/sub", device_serial);
    esp_mqtt_client_publish(mqtt_client, topic, (const char *)data, (int)len, 1,
                            0);
    ESP_LOGI(TAG, "published RS485 data to MQTT topic=%s len=%u", topic,
             (unsigned)len);
    return PCG_PROCESS_OK;
  }

  ESP_LOGW(TAG, "receiver ID mismatch, got %u", receiver_id);
  return PCG_PROCESS_ERR_RECEIVER_ID;
}
