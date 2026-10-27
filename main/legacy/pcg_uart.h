#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

#define PCG_UART_NUM UART_NUM_2
#define PCG_UART_BAUD_RATE 38400
#define PCG_UART_TX_BUF_SIZE 0
#define PCG_UART_RX_BUF_SIZE 256
#define PCG_UART_RX_READ_SIZE 128
#define PCG_UART_RX_TOUT_SYMBOLS 3

esp_err_t pcg_uart_init(void);
void pcg_uart_flush_rx(void);
esp_err_t pcg_rs485_send(const uint8_t *data, size_t len);

/** @deprecated use pcg_uart_flush_rx */
#define rtr_uart_flush_rx pcg_uart_flush_rx

/** @deprecated use pcg_rs485_send */
#define rtr_rs485_send pcg_rs485_send
