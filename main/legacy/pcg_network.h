#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
  bool wifi_connected;
  bool mqtt_connected;
} pcg_network_status_t;

esp_err_t pcg_network_wifi_connect(void);
void pcg_network_get_status(pcg_network_status_t *status);
void pcg_network_mqtt_set_connected(bool connected);
esp_err_t pcg_network_monitor_start(void);
