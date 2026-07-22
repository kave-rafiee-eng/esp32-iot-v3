#pragma once

#include <stdint.h>

uint16_t pcg_read_serial_device(void);

/** @deprecated use pcg_read_serial_device */
#define readSerialDevice pcg_read_serial_device
