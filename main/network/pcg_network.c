#include "pcg_network.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gpio_output.h"
#include "sdkconfig.h"

static const char *TAG = "pcg_network";

/* Blink phases are 100 ms, so the loop must not run slower than that. */
#define MONITOR_TICK_MAX_MS 100

/* Both links up: on most of the time, with a short off blink. */
#define LED2_BOTH_ON_MS 400
#define LED2_BOTH_OFF_MS 100

/* Wi-Fi only: even blink. */
#define LED2_WIFI_ON_MS 500
#define LED2_WIFI_OFF_MS 500

/* Serial not read yet. */
#define LED1_WAIT_ON_MS 500
#define LED1_WAIT_OFF_MS 500

typedef enum {
    LED_PAT_OFF = 0,
    LED_PAT_ON,
    LED_PAT_BOTH,
    LED_PAT_WIFI,
    LED_PAT_WAIT,
} led_pat_t;

static volatile bool s_wifi_connected = false;
static volatile bool s_mqtt_connected = false;
static volatile bool s_serial_ready = false;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;

static void mqtt_kick_reconnect(void)
{
    if (s_mqtt_client == NULL || s_mqtt_connected) {
        return;
    }

    esp_err_t err = esp_mqtt_client_reconnect(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT reconnect: %s", esp_err_to_name(err));
    }
}

static void on_wifi_disconnect(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    s_wifi_connected = false;
    ESP_LOGI(TAG, "Wi-Fi disconnected, reconnecting...");

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
}

static void on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id,
                      void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    s_wifi_connected = true;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    mqtt_kick_reconnect();
}

esp_err_t pcg_network_init(void)
{
    esp_err_t err;

    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                     &on_wifi_disconnect, NULL);
    if (err != ESP_OK) {
        return err;
    }

    return esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL);
}

void pcg_network_get_status(pcg_network_status_t *status)
{
    if (status == NULL) {
        return;
    }
    status->wifi_connected = s_wifi_connected;
    status->mqtt_connected = s_mqtt_connected;
}

void pcg_network_mqtt_set_connected(bool connected)
{
    s_mqtt_connected = connected;
}

void pcg_network_set_mqtt_client(esp_mqtt_client_handle_t client)
{
    s_mqtt_client = client;
}

void pcg_network_serial_set_ready(bool ready)
{
    s_serial_ready = ready;
}

static int led_pattern_level(led_pat_t pat, TickType_t now, TickType_t origin)
{
    uint32_t on_ms;
    uint32_t off_ms;

    switch (pat) {
    case LED_PAT_ON:
        return 1;
    case LED_PAT_OFF:
        return 0;
    case LED_PAT_BOTH:
        on_ms = LED2_BOTH_ON_MS;
        off_ms = LED2_BOTH_OFF_MS;
        break;
    case LED_PAT_WIFI:
        on_ms = LED2_WIFI_ON_MS;
        off_ms = LED2_WIFI_OFF_MS;
        break;
    case LED_PAT_WAIT:
        on_ms = LED1_WAIT_ON_MS;
        off_ms = LED1_WAIT_OFF_MS;
        break;
    default:
        return 0;
    }

    uint32_t period = on_ms + off_ms;
    uint32_t elapsed = (uint32_t)pdTICKS_TO_MS(now - origin);
    return (elapsed % period) < on_ms ? 1 : 0;
}

static void led_update(gpio_led_t led, led_pat_t pat, led_pat_t *prev, TickType_t *origin)
{
    TickType_t now = xTaskGetTickCount();

    if (pat != *prev) {
        *prev = pat;
        *origin = now;
    }
    gpio_led_set(led, led_pattern_level(pat, now, *origin));
}

static void network_monitor_task(void *arg)
{
    (void)arg;

    pcg_network_status_t prev = {0};
    const TickType_t reset_interval_ticks = pdMS_TO_TICKS(60 * 60 * 1000);
    TickType_t task_start = xTaskGetTickCount();
    led_pat_t led2_prev = LED_PAT_OFF;
    led_pat_t led1_prev = LED_PAT_OFF;
    TickType_t led2_origin = task_start;
    TickType_t led1_origin = task_start;
    uint32_t interval_ms = CONFIG_PCG_NETWORK_MONITOR_INTERVAL_MS;

    if (interval_ms > MONITOR_TICK_MAX_MS) {
        interval_ms = MONITOR_TICK_MAX_MS;
    }

    ESP_LOGI(TAG, "network monitor started");

    while (1) {
        pcg_network_status_t status;
        pcg_network_get_status(&status);

        if (status.wifi_connected != prev.wifi_connected ||
            status.mqtt_connected != prev.mqtt_connected) {
            ESP_LOGI(TAG, "status changed: wifi=%s mqtt=%s",
                     status.wifi_connected ? "connected" : "disconnected",
                     status.mqtt_connected ? "connected" : "disconnected");
            prev = status;
        }

        led_pat_t led2 = LED_PAT_OFF;
        if (status.wifi_connected && status.mqtt_connected) {
            led2 = LED_PAT_BOTH;
        } else if (status.wifi_connected) {
            led2 = LED_PAT_WIFI;
        }
        led_update(GPIO_LED_2, led2, &led2_prev, &led2_origin);

        led_pat_t led1 = s_serial_ready ? LED_PAT_ON : LED_PAT_WAIT;
        led_update(GPIO_LED_1, led1, &led1_prev, &led1_origin);

        if ((xTaskGetTickCount() - task_start) >= reset_interval_ticks) {
            ESP_LOGI(TAG, "hourly reset timer elapsed, restarting ESP");
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

esp_err_t pcg_network_monitor_start(void)
{
    BaseType_t created =
        xTaskCreate(network_monitor_task, "pcg_network", 3072, NULL, 3, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create network monitor task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
