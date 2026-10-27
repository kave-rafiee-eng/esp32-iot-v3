#pragma once

#include "pcg.h"

/* Publish a FWU reply frame to the MQTT peer that sent the request. */
typedef void (*fwu_mqtt_publish_fn)(uint8_t peer, const uint8_t *frame, size_t len, void *ctx);

/* Handle FWU port: build reply and send on RS485 or MQTT. */
void fwu_handle_packet(const pcg_packet_t *pkt, fwu_mqtt_publish_fn mqtt_publish, void *ctx);
