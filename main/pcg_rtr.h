#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PCG_UART_NUM              UART_NUM_2
#define PCG_UART_BAUD_RATE        38400
#define PCG_UART_TX_BUF_SIZE      0
#define PCG_UART_RX_BUF_SIZE      2048
#define PCG_UART_RX_READ_SIZE     256

#define PCG_RTR_DEVICE_SERIAL     10100

esp_err_t pcg_uart_init(void);
void rtr_uart_flush_rx(void);

bool rtrReadRegister(uint16_t add, uint16_t *value, uint32_t timeout_ms);
esp_err_t rtr_rs485_send(const uint8_t *data, size_t len);
