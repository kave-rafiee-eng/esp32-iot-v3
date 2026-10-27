#pragma once

#include <stdint.h>

#include "pcg_protocol.h"

uint16_t pcg_read_serial_device(void);

pcg_process_result_t pcg_device_handle(const pcg_device_frame_t *frame,
                                         const pcg_process_ctx_t *ctx);

/** @deprecated use pcg_read_serial_device */
#define readSerialDevice pcg_read_serial_device
