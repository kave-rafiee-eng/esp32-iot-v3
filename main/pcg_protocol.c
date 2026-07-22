#include "pcg_protocol.h"

#include "esp_log.h"

#include "pcg_client.h"
#include "pcg_device.h"

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
                                 const pcg_process_ctx_t *ctx) {
  if (data == NULL || ctx == NULL) {
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
  pcg_port_t port = (pcg_port_t)data[PCG_OFF_PORT];

  switch (receiver_id) {
  case PCG_ADDR_ESP32:
    switch (port) {
    case PCG_PORT_DEVICE: {
      if (len < PCG_DEVICE_MIN_FRAME_SIZE) {
        ESP_LOGW(TAG, "device frame too short, len=%u", (unsigned)len);
        return PCG_PROCESS_ERR_TOO_SHORT;
      }

      pcg_device_frame_t frame = {
          .device_id = (uint16_t)((data[PCG_OFF_DEVICE_ID_HI] << 8) |
                                  data[PCG_OFF_DEVICE_ID_LO]),
          .request = data[PCG_OFF_DEVICE_REQUEST],
      };
      return pcg_device_handle(&frame, ctx);
    }
    case PCG_PORT_RTR:
      ESP_LOGW(TAG, "RTR port handler not implemented");
      return PCG_PROCESS_IGNORED;
    default:
      ESP_LOGW(TAG, "unknown port %u for ESP32", port);
      return PCG_PROCESS_ERR_PORT;
    }

  case PCG_ADDR_CLIENT:
    return pcg_client_handle(port, data, len, ctx);

  default:
    ESP_LOGW(TAG, "receiver ID mismatch, got %u", receiver_id);
    return PCG_PROCESS_ERR_RECEIVER_ID;
  }
}
