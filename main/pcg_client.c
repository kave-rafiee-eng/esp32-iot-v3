#include "pcg_client.h"

#include <stdio.h>

#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "pcg_client";

pcg_process_result_t pcg_client_handle(pcg_port_t port, const uint8_t *data,
                                       size_t len,
                                       const pcg_process_ctx_t *ctx) {
  if (ctx == NULL || ctx->mqtt_client == NULL) {
    return PCG_PROCESS_ERR_NULL;
  }

  switch (port) {
  case PCG_PORT_DEVICE:
  case PCG_PORT_RTR:
    break;
  default:
    ESP_LOGW(TAG, "unknown port %u for client", port);
    return PCG_PROCESS_ERR_PORT;
  }

  char topic[64];
  snprintf(topic, sizeof(topic), "%u/sub", ctx->device_serial);
  esp_mqtt_client_publish(ctx->mqtt_client, topic, (const char *)data, (int)len,
                          1, 0);
  ESP_LOGI(TAG, "published RS485 data to MQTT topic=%s port=%u len=%u", topic,
           port, (unsigned)len);
  return PCG_PROCESS_OK;
}
