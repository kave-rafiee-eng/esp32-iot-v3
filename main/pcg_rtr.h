#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

unsigned int CRC_16(uint8_t *buf, int len);

esp_err_t pcg_uart_init(void);

bool rtrReadRegister(uint16_t add, uint16_t *value, uint32_t timeout_ms);
