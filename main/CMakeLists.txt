idf_component_register(SRCS "app_main.c" "mqtt_data.c" "pcgtask.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver mqtt esp_wifi esp_netif esp_event nvs_flash protocol_examples_common)
