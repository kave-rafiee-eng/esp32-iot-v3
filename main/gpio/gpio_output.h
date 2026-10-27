#pragma once

#include "esp_err.h"

#define GPIO_LED_1_PIN 15
#define GPIO_LED_2_PIN 2

typedef enum {
    GPIO_LED_1 = 0,
    GPIO_LED_2 = 1,
} gpio_led_t;

esp_err_t gpio_output_init(void);
esp_err_t gpio_output_set(int level);
esp_err_t gpio_output_toggle(void);

esp_err_t gpio_led_init(void);
esp_err_t gpio_led_set(gpio_led_t led, int level);
esp_err_t gpio_led_toggle(gpio_led_t led);
