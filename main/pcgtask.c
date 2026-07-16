#include "pcgtask.h"

#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gpio_output.h"
#include "mqtt_data.h"
#include "pcg_rtr.h"
#include "sdkconfig.h"

static const char *TAG = "pcgtask";
#define PCG_QUEUE_WAIT_MS 50



static void pcg_uart_write_message(const mqtt_data_msg_t *msg)
{
    // if (msg == NULL || msg->data_len <= 0) {
    //     return;
    // }

    // uart_write_bytes(PCG_UART_NUM, msg->data, msg->data_len);
    // uart_write_bytes(PCG_UART_NUM, "\r\n", 2);
}

/* Sample: write GPIO output HIGH / LOW / toggle. */
static void pcg_gpio_write_sample(void)
{
    gpio_output_set(1);   /* GPIO4 = HIGH (3.3V) */
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_output_set(0);   /* GPIO4 = LOW  (0V)   */
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_output_toggle(); /* flip current level */
}

/* Sample: read whatever arrived on UART2 (Serial 2) and log it. */
static void pcg_uart_read_sample(void)
{
    // uint8_t data[PCG_UART_RX_READ_SIZE];
    // int len = uart_read_bytes(PCG_UART_NUM, data, sizeof(data) - 1, 0);
    // if (len <= 0) {
    //     return;
    // }

    // data[len] = '\0';
    // ESP_LOGI(TAG, "UART2 RX (%d bytes): %.*s", len, len, (char *)data);
    // ESP_LOG_BUFFER_HEXDUMP(TAG, data, len, ESP_LOG_DEBUG);
}

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
            ESP_LOGI(TAG, "DATA=%.*s", msg.data_len, msg.data);
            pcg_uart_write_message(&msg);
            gpio_output_toggle();
        }

        pcg_uart_read_sample();
        // vTaskDelay(100);
        // const uint8_t data[20] = "heloooooooo";
        // uart_write_bytes(PCG_UART_NUM, data, sizeof(data));
    }
}

esp_err_t pcgtask_start(esp_mqtt_client_handle_t client)
{
    esp_err_t err = gpio_output_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = pcg_uart_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t created = xTaskCreate(pcgtask, "pcgtask", 4096, client, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create pcgtask");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
