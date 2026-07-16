#pragma once

#include "esp_err.h"

esp_err_t gpio_output_init(void);
esp_err_t gpio_output_set(int level);
esp_err_t gpio_output_toggle(void);
