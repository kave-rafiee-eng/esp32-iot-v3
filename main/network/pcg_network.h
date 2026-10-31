#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "mqtt_client.h"

typedef struct {
    bool wifi_connected;
    bool mqtt_connected;
} pcg_network_status_t;

/* Register Wi-Fi/IP handlers. Call after the default event loop exists
 * and before example_connect(), so the first GOT_IP is observed. */
esp_err_t pcg_network_init(void);

void pcg_network_get_status(pcg_network_status_t *status);
void pcg_network_mqtt_set_connected(bool connected);
void pcg_network_set_mqtt_client(esp_mqtt_client_handle_t client);

/* LED1 blinks until this is set, then stays on. */
void pcg_network_serial_set_ready(bool ready);

/* LED2: mostly on with a short blink when Wi-Fi and MQTT are up,
 * even blink when only Wi-Fi is up, off otherwise.
 * LED1: blinks until the device serial is read, then stays on.
 * Restarts the ESP after one hour. */
esp_err_t pcg_network_monitor_start(void);
