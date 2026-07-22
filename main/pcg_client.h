#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pcg_protocol.h"

pcg_process_result_t pcg_client_handle(pcg_port_t port, const uint8_t *data,
                                       size_t len,
                                       const pcg_process_ctx_t *ctx);
