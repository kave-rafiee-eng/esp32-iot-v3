#include "pcgtask.h"

#include <stdint.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mqtt_data.h"
#include "pcg_protocol.h"
#include "pcg_uart.h"
#include "sdkconfig.h"

static const char *TAG = "pcgtask";

extern uint16_t deviceSerial;

static void pcgtask(void *arg) {
  esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)arg;
  QueueHandle_t data_queue = mqtt_data_queue_get();

  if (client == NULL || data_queue == NULL) {
    ESP_LOGE(TAG, "invalid task context");
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "started, mqtt client=%p", client);

  while (1) {
    uint8_t rx_buf[PCG_UART_RX_READ_SIZE];
    int len = uart_read_bytes(PCG_UART_NUM, rx_buf, sizeof(rx_buf), 0);

    if (len > 0) {
      ESP_LOGI(TAG, "RS485 data received, len=%d", len);
      pcg_process(rx_buf, (size_t)len, client, deviceSerial, data_queue);
    }

    vTaskDelay(1);
  }
}

esp_err_t pcgtask_start(esp_mqtt_client_handle_t client) {
  BaseType_t created =
      xTaskCreatePinnedToCore(pcgtask, "pcgtask", 4096, client, 5, NULL, 1);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "failed to create pcgtask");
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
