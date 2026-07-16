#include "gpio_output.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "gpio_output";

esp_err_t gpio_output_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_GPIO_OUTPUT_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Start with output LOW */
    gpio_set_level(CONFIG_GPIO_OUTPUT_PIN, 0);

    ESP_LOGI(TAG, "GPIO%d configured as output", CONFIG_GPIO_OUTPUT_PIN);
    return ESP_OK;
}

esp_err_t gpio_output_set(int level)
{
    return gpio_set_level(CONFIG_GPIO_OUTPUT_PIN, level ? 1 : 0);
}

esp_err_t gpio_output_toggle(void)
{
    int level = gpio_get_level(CONFIG_GPIO_OUTPUT_PIN);
    return gpio_set_level(CONFIG_GPIO_OUTPUT_PIN, level ? 0 : 1);
}
