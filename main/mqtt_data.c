#include "mqtt_data.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "mqtt_data";

static QueueHandle_t s_mqtt_data_queue = NULL;

QueueHandle_t mqtt_data_queue_get(void)
{
    return s_mqtt_data_queue;
}

esp_err_t mqtt_data_queue_init(void)
{
    if (s_mqtt_data_queue != NULL) {
        return ESP_OK;
    }

    s_mqtt_data_queue = xQueueCreate(MQTT_DATA_QUEUE_LEN, sizeof(mqtt_data_msg_t));
    if (s_mqtt_data_queue == NULL) {
        ESP_LOGE(TAG, "failed to create mqtt data queue");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool mqtt_data_enqueue(esp_mqtt_event_handle_t event)
{
    if (s_mqtt_data_queue == NULL || event == NULL) {
        return false;
    }

    mqtt_data_msg_t msg = {0};

    if (event->topic_len <= 0 || event->topic == NULL) {
        return false;
    }

    msg.topic_len = event->topic_len;
    if (msg.topic_len >= MQTT_TOPIC_MAX_LEN) {
        msg.topic_len = MQTT_TOPIC_MAX_LEN - 1;
    }
    memcpy(msg.topic, event->topic, msg.topic_len);
    msg.topic[msg.topic_len] = '\0';

    if (event->data_len > 0 && event->data != NULL) {
        msg.data_len = event->data_len;
        if (msg.data_len >= MQTT_PAYLOAD_MAX_LEN) {
            msg.data_len = MQTT_PAYLOAD_MAX_LEN - 1;
        }
        memcpy(msg.data, event->data, msg.data_len);
        msg.data[msg.data_len] = '\0';
    }

    if (xQueueSend(s_mqtt_data_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "mqtt data queue full, dropping message");
        return false;
    }

    return true;
}
