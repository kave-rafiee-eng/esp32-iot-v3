#include "pcg_network.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "pcg_network";

static volatile bool s_wifi_connected = false;
static volatile bool s_mqtt_connected = false;

static SemaphoreHandle_t s_got_ip_sem = NULL;

static void on_wifi_disconnect(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  (void)arg;
  (void)event_base;
  (void)event_id;
  (void)event_data;

  s_wifi_connected = false;
  ESP_LOGI(TAG, "Wi-Fi disconnected, reconnecting...");
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_ERR_WIFI_NOT_STARTED) {
    ESP_ERROR_CHECK(err);
  }
}

static void on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id,
                      void *event_data) {
  (void)arg;
  (void)event_base;
  (void)event_id;

  ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
  s_wifi_connected = true;
  ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

  if (s_got_ip_sem != NULL) {
    xSemaphoreGive(s_got_ip_sem);
  }
}

esp_err_t pcg_network_wifi_connect(void) {
  s_got_ip_sem = xSemaphoreCreateBinary();
  if (s_got_ip_sem == NULL) {
    return ESP_ERR_NO_MEM;
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
  if (sta_netif == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                           &on_wifi_disconnect, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                           &on_got_ip, NULL));

  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = CONFIG_EXAMPLE_WIFI_SSID,
              .password = CONFIG_EXAMPLE_WIFI_PASSWORD,
          },
  };

  ESP_LOGI(TAG, "Connecting to %s...", wifi_config.sta.ssid);
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_connect());

  ESP_LOGI(TAG, "Waiting for IP");
  xSemaphoreTake(s_got_ip_sem, portMAX_DELAY);
  vSemaphoreDelete(s_got_ip_sem);
  s_got_ip_sem = NULL;

  return ESP_OK;
}

void pcg_network_get_status(pcg_network_status_t *status) {
  if (status == NULL) {
    return;
  }
  status->wifi_connected = s_wifi_connected;
  status->mqtt_connected = s_mqtt_connected;
}

void pcg_network_mqtt_set_connected(bool connected) {
  s_mqtt_connected = connected;
}

static void network_monitor_task(void *arg) {
  (void)arg;

  pcg_network_status_t prev = {0};

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

    vTaskDelay(pdMS_TO_TICKS(CONFIG_PCG_NETWORK_MONITOR_INTERVAL_MS));
  }
}

esp_err_t pcg_network_monitor_start(void) {
  BaseType_t created =
      xTaskCreate(network_monitor_task, "pcg_network", 3072, NULL, 3, NULL);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "failed to create network monitor task");
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
