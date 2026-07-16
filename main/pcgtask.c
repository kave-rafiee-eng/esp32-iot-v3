#include "pcgtask.h"

#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mqtt_data.h"
#include "pcg_rtr.h"
#include "sdkconfig.h"

static const char *TAG = "pcgtask";
#define PCG_QUEUE_WAIT_MS 50

extern uint16_t deviceSerial;

static void pcgtask(void *arg)
{
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)arg;
    QueueHandle_t data_queue = mqtt_data_queue_get();

    if (client == NULL || data_queue == NULL) {
        ESP_LOGE(TAG, "invalid task context");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "started, mqtt client=%p", client);

    mqtt_data_msg_t msg;
    while (1) {
        if (xQueueReceive(data_queue, &msg, pdMS_TO_TICKS(PCG_QUEUE_WAIT_MS)) == pdTRUE) {
            ESP_LOGI(TAG, "TOPIC=%.*s", msg.topic_len, msg.topic);
            ESP_LOGI(TAG, "DATA len=%d", msg.data_len);
            esp_log_buffer_hex(TAG, msg.data, msg.data_len);
            rtr_uart_flush_rx();
            rtr_rs485_send((const uint8_t *)msg.data, msg.data_len);
        }

        uint8_t rx_buf[PCG_UART_RX_READ_SIZE];
        int len = uart_read_bytes(PCG_UART_NUM, rx_buf, sizeof(rx_buf), 0);

        if (len > 0) {
            char topic[64];
            snprintf(topic, sizeof(topic), "%d/sub", deviceSerial);
    
            ESP_LOGI(TAG, "get data from rs485 len : %d", len);
            esp_mqtt_client_publish(client, topic, (const char *)rx_buf, len, 1, 0);
        }

        vTaskDelay(1);

        // if( rtrReadRegister(10100, &deviceSerial, 0) == true ){
        //     ESP_LOGE(TAG,"serial : %d",deviceSerial);
        // }
       
    }
}

esp_err_t pcgtask_start(esp_mqtt_client_handle_t client)
{
    BaseType_t created = xTaskCreatePinnedToCore(pcgtask, "pcgtask", 4096, client, 5, NULL,1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create pcgtask");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
