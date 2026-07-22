idf_component_register(SRCS "pcg_uart.c" "pcg_protocol.c" "pcg_device.c" "pcg_rtr.c" "app_main.c" "mqtt_data.c" "pcgtask.c" "gpio_output.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver mqtt esp_wifi esp_netif esp_event nvs_flash protocol_examples_common)
