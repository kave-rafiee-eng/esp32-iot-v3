#include "gpio_output.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "gpio_output";

static const gpio_num_t s_led_pins[] = {
    GPIO_LED_1_PIN,
    GPIO_LED_2_PIN,
};

static int s_led_levels[2] = {0, 0};

esp_err_t gpio_output_init(void) {
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

  gpio_set_level(CONFIG_GPIO_OUTPUT_PIN, 0);

  ESP_LOGI(TAG, "GPIO%d configured as output", CONFIG_GPIO_OUTPUT_PIN);
  return ESP_OK;
}

esp_err_t gpio_output_set(int level) {
  return gpio_set_level(CONFIG_GPIO_OUTPUT_PIN, level ? 1 : 0);
}

esp_err_t gpio_output_toggle(void) {
  int level = gpio_get_level(CONFIG_GPIO_OUTPUT_PIN);
  return gpio_set_level(CONFIG_GPIO_OUTPUT_PIN, level ? 0 : 1);
}

esp_err_t gpio_led_init(void) {
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << GPIO_LED_1_PIN) | (1ULL << GPIO_LED_2_PIN),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  esp_err_t err = gpio_config(&io_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LED gpio_config failed: %s", esp_err_to_name(err));
    return err;
  }

  gpio_set_level(GPIO_LED_1_PIN, 0);
  gpio_set_level(GPIO_LED_2_PIN, 0);
  s_led_levels[GPIO_LED_1] = 0;
  s_led_levels[GPIO_LED_2] = 0;

  ESP_LOGI(TAG, "LED GPIO%d and GPIO%d configured as output", GPIO_LED_1_PIN,
           GPIO_LED_2_PIN);
  return ESP_OK;
}

esp_err_t gpio_led_set(gpio_led_t led, int level) {
  if (led > GPIO_LED_2) {
    return ESP_ERR_INVALID_ARG;
  }

  s_led_levels[led] = level ? 1 : 0;
  return gpio_set_level(s_led_pins[led], s_led_levels[led]);
}

esp_err_t gpio_led_toggle(gpio_led_t led) {
  if (led > GPIO_LED_2) {
    return ESP_ERR_INVALID_ARG;
  }

  s_led_levels[led] = s_led_levels[led] ? 0 : 1;
  return gpio_set_level(s_led_pins[led], s_led_levels[led]);
}
