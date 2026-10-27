#include "pcg.h"

#include <string.h>

static unsigned int crc16_step(unsigned int crc, uint8_t byte)
{
    crc ^= (unsigned int)byte;
    for (int i = 8; i != 0; i--) {
        if ((crc & 0x0001) != 0) {
            crc >>= 1;
            crc ^= 0xA001;
        } else {
            crc >>= 1;
        }
    }
    return crc & 0xffffu;
}

uint16_t pcg_crc16(const uint8_t *buf, size_t len)
{
    unsigned int crc = 0xFFFF;

    for (size_t pos = 0; pos < len; pos++) {
        crc = crc16_step(crc, buf[pos]);
    }

    return (uint16_t)crc;
}

bool pcg_parse(const uint8_t *data, size_t len, pcg_packet_t *out)
{
    if (data == NULL || out == NULL || len < PCG_MIN_PACKET_SIZE) {
        return false;
    }

    size_t body_len = len - PCG_CRC_SIZE;
    uint16_t received = (uint16_t)data[len - 2] | ((uint16_t)data[len - 1] << 8);
    if (pcg_crc16(data, body_len) != received) {
        return false;
    }

    out->pid = data[PCG_OFF_PID];
    out->sender_type = data[PCG_OFF_SENDER_TYPE];
    out->sender_serial = (uint16_t)data[PCG_OFF_SENDER_SERIAL_LO]
                       | ((uint16_t)data[PCG_OFF_SENDER_SERIAL_HI] << 8);
    out->receiver_type = data[PCG_OFF_RECEIVER_TYPE];
    out->receiver_port = data[PCG_OFF_RECEIVER_PORT];
    out->payload = data + PCG_HEADER_SIZE;
    out->payload_len = body_len - PCG_HEADER_SIZE;
    return true;
}

size_t pcg_build(uint8_t *out, size_t out_cap,
                 uint8_t pid, uint8_t sender_type, uint16_t sender_serial,
                 uint8_t receiver_type, uint8_t receiver_port,
                 const uint8_t *payload, size_t payload_len)
{
    if (out == NULL || (payload == NULL && payload_len > 0)) {
        return 0;
    }

    size_t body_len = PCG_HEADER_SIZE + payload_len;
    size_t total = body_len + PCG_CRC_SIZE;
    if (out_cap < total) {
        return 0;
    }

    out[PCG_OFF_PID] = pid;
    out[PCG_OFF_SENDER_TYPE] = sender_type;
    out[PCG_OFF_SENDER_SERIAL_LO] = (uint8_t)(sender_serial & 0xff);
    out[PCG_OFF_SENDER_SERIAL_HI] = (uint8_t)((sender_serial >> 8) & 0xff);
    out[PCG_OFF_RECEIVER_TYPE] = receiver_type;
    out[PCG_OFF_RECEIVER_PORT] = receiver_port;
    if (payload_len > 0) {
        memcpy(out + PCG_HEADER_SIZE, payload, payload_len);
    }

    uint16_t crc = pcg_crc16(out, body_len);
    out[body_len] = (uint8_t)(crc & 0xff);
    out[body_len + 1] = (uint8_t)((crc >> 8) & 0xff);
    return total;
}

size_t pcg_split_frames(const uint8_t *data, size_t len, pcg_frame_cb_t cb, void *ctx,
                        size_t *consumed)
{
    size_t offset = 0;
    size_t count = 0;

    if (data == NULL || cb == NULL) {
        if (consumed != NULL) {
            *consumed = 0;
        }
        return 0;
    }

    while (offset + PCG_MIN_PACKET_SIZE <= len) {
        size_t max_end = offset + PCG_MAX_PACKET_SIZE;
        unsigned int crc = 0xFFFF;
        bool found = false;

        if (max_end > len) {
            max_end = len;
        }

        for (size_t i = 0; i < PCG_HEADER_SIZE; i++) {
            crc = crc16_step(crc, data[offset + i]);
        }

        for (size_t end = offset + PCG_MIN_PACKET_SIZE; end <= max_end; end++) {
            uint16_t received = (uint16_t)data[end - 2] | ((uint16_t)data[end - 1] << 8);
            if ((uint16_t)crc == received) {
                cb(data + offset, end - offset, ctx);
                offset = end;
                count++;
                found = true;
                break;
            }
            if (end < max_end) {
                crc = crc16_step(crc, data[end - 2]);
            }
        }

        if (!found) {
            offset++;
        }
    }

    if (consumed != NULL) {
        *consumed = offset;
    }
    return count;
}
