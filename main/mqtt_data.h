#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mqtt_client.h"

#define MQTT_DATA_QUEUE_LEN   10
#define MQTT_TOPIC_MAX_LEN    128
#define MQTT_PAYLOAD_MAX_LEN  512

typedef struct {
    char topic[MQTT_TOPIC_MAX_LEN];
    int topic_len;
    char data[MQTT_PAYLOAD_MAX_LEN];
    int data_len;
} mqtt_data_msg_t;

QueueHandle_t mqtt_data_queue_get(void);

esp_err_t mqtt_data_queue_init(void);

bool mqtt_data_enqueue(esp_mqtt_event_handle_t event);
