#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pcg_protocol.h"

#define RTR_POLL_INTERVAL_MS 10
#define RTR_RESEND_INTERVAL_MS 1000

bool rtrReadRegister(uint16_t add, uint16_t *value, uint32_t timeout_ms);
