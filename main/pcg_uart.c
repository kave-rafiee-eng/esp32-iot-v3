#include "pcg_uart.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

static const char *TAG = "pcg_uart";

void pcg_uart_flush_rx(void) {
  uint8_t discard[PCG_UART_RX_READ_SIZE];
  int flushed = 0;

  while (1) {
    int len = uart_read_bytes(PCG_UART_NUM, discard, sizeof(discard), 0);
    if (len <= 0) {
      break;
    }
    flushed += len;
  }

  if (flushed > 0) {
    ESP_LOGD(TAG, "flushed %d stale RX bytes", flushed);
  }
}

esp_err_t pcg_rs485_send(const uint8_t *data, size_t len) {
  if (data == NULL || len == 0) {
    ESP_LOGE(TAG, "RS485 send: invalid buffer");
    return ESP_ERR_INVALID_ARG;
  }

  int written = uart_write_bytes(PCG_UART_NUM, data, len);
  if (written < 0 || (size_t)written != len) {
    ESP_LOGE(TAG, "RS485 send failed, written=%d, expected=%u", written,
             (unsigned)len);
    return ESP_FAIL;
  }

  esp_err_t err = uart_wait_tx_done(PCG_UART_NUM, portMAX_DELAY);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "RS485 wait TX done failed: %s", esp_err_to_name(err));
    return err;
  }

  return ESP_OK;
}

esp_err_t pcg_uart_init(void) {
  uart_config_t uart_config = {
      .baud_rate = PCG_UART_BAUD_RATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  esp_err_t err = uart_driver_install(PCG_UART_NUM, PCG_UART_RX_BUF_SIZE,
                                      PCG_UART_TX_BUF_SIZE, 0, NULL, 0);
  if (err != ESP_OK) {
    return err;
  }

  err = uart_param_config(PCG_UART_NUM, &uart_config);
  if (err != ESP_OK) {
    uart_driver_delete(PCG_UART_NUM);
    return err;
  }

  err =
      uart_set_pin(PCG_UART_NUM, CONFIG_PCG_UART_TX_PIN, CONFIG_PCG_UART_RX_PIN,
                   CONFIG_GPIO_OUTPUT_PIN, UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    uart_driver_delete(PCG_UART_NUM);
    return err;
  }

  err = uart_set_mode(PCG_UART_NUM, UART_MODE_RS485_HALF_DUPLEX);
  if (err != ESP_OK) {
    uart_driver_delete(PCG_UART_NUM);
    return err;
  }

  err = uart_set_rx_timeout(PCG_UART_NUM, PCG_UART_RX_TOUT_SYMBOLS);
  if (err != ESP_OK) {
    uart_driver_delete(PCG_UART_NUM);
    return err;
  }

  ESP_LOGI(TAG,
           "UART%d init OK (RS485 half-duplex), baud=%d, TX=GPIO%d, RX=GPIO%d, "
           "DE/RE(RTS)=GPIO%d",
           PCG_UART_NUM, PCG_UART_BAUD_RATE, CONFIG_PCG_UART_TX_PIN,
           CONFIG_PCG_UART_RX_PIN, CONFIG_GPIO_OUTPUT_PIN);
  return ESP_OK;
}
