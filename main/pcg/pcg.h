#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCG_CRC_SIZE        2
#define PCG_HEADER_SIZE     6
#define PCG_MIN_PACKET_SIZE (PCG_HEADER_SIZE + PCG_CRC_SIZE)
#define PCG_MAX_PACKET_SIZE 1024

#define PCG_OFF_PID              0
#define PCG_OFF_SENDER_TYPE      1
#define PCG_OFF_SENDER_SERIAL_LO 2
#define PCG_OFF_SENDER_SERIAL_HI 3
#define PCG_OFF_RECEIVER_TYPE    4
#define PCG_OFF_RECEIVER_PORT    5

typedef enum {
    PCG_PEER_ADVANCE = 1,
    PCG_PEER_ESP32 = 2,
    PCG_PEER_CLIENT = 3,
    PCG_PEER_SERVER_UPDATE = 4,
    PCG_PEER_SERVER_RECORD = 5,
} pcg_peer_t;

typedef enum {
    PCG_PORT_DIP = 0,
    PCG_PORT_RTR = 1,
    PCG_PORT_FWU = 2,
    PCG_PORT_MODBUS = 3,
} pcg_port_t;

typedef struct {
    uint8_t pid;
    uint8_t sender_type;
    uint16_t sender_serial;
    uint8_t receiver_type;
    uint8_t receiver_port;
    const uint8_t *payload;
    size_t payload_len;
} pcg_packet_t;

typedef void (*pcg_frame_cb_t)(const uint8_t *frame, size_t len, void *ctx);

uint16_t pcg_crc16(const uint8_t *buf, size_t len);

bool pcg_parse(const uint8_t *data, size_t len, pcg_packet_t *out);

size_t pcg_build(uint8_t *out, size_t out_cap,
                 uint8_t pid, uint8_t sender_type, uint16_t sender_serial,
                 uint8_t receiver_type, uint8_t receiver_port,
                 const uint8_t *payload, size_t payload_len);

/* Shortest CRC-valid frame at each offset. *consumed is the first byte not scanned. */
size_t pcg_split_frames(const uint8_t *data, size_t len, pcg_frame_cb_t cb, void *ctx,
                        size_t *consumed);
