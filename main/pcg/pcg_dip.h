#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Sink for UART bytes drained after a SYNCK RS485 write. */
typedef void (*pcg_dip_rx_sink_t)(const uint8_t *data, int len);

/* Queue an MQTT frame for later RS485 TX when Advance sends SYNCK. */
bool pcg_dip_queue_push(const uint8_t *data, size_t len);

/* Handle DIP port payload (SYNCK request arms one queued frame). */
void pcg_dip_handle(const uint8_t *payload, size_t len);

/* True when a SYNCK release is armed and waiting to run. */
bool pcg_dip_armed(void);

/* Send the armed frame on RS485, then drain leftover UART into rx_sink. */
void pcg_dip_release(pcg_dip_rx_sink_t rx_sink);
