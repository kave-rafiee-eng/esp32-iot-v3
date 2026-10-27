#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define FWU_MODE_READ  1
#define FWU_MODE_WRITE 2
#define FWU_MODE_ACK   3

#define FWU_ADDR_SIZE  16
#define FWU_DATA_SIZE  256
#define FWU_READ_SIZE  (1 + FWU_ADDR_SIZE)
#define FWU_WRITE_SIZE (1 + FWU_ADDR_SIZE + FWU_DATA_SIZE)

esp_err_t fwu_init(void);

/* READ replies as WRITE (address + 256 data). WRITE replies as ACK. */
bool fwu_build_reply(const uint8_t *req, size_t req_len,
                     uint8_t *out, size_t out_cap, size_t *out_len);
