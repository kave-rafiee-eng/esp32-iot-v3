#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mqtt_client.h"

typedef enum {
  PCG_ADDR_ADVANCE = 1,
  PCG_ADDR_ESP32 = 2,
  PCG_ADDR_CLIENT = 3,
} pcg_addr_t;

typedef enum {
  PCG_PORT_DEVICE = 0,
  PCG_PORT_RTR = 1,
} pcg_port_t;

typedef enum {
  PCG_RTR_MODE_READ = 1,
  PCG_RTR_MODE_WRITE = 2,
} pcg_rtr_mode_t;

typedef enum {
  RTR_PARSE_OK = 0,
  RTR_PARSE_ERR_NULL_VALUE,
  RTR_PARSE_ERR_WRONG_LEN,
  RTR_PARSE_ERR_CRC,
  RTR_PARSE_ERR_SENDER_ID,
  RTR_PARSE_ERR_RECEIVER_ID,
  RTR_PARSE_ERR_PORT,
  RTR_PARSE_ERR_PID,
  RTR_PARSE_ERR_MODE,
  RTR_PARSE_ERR_REG_COUNT,
  RTR_PARSE_ERR_ADDRESS,
} rtr_parse_result_t;

#define PCG_CRC_SIZE 2
#define PCG_HEADER_SIZE 3 /* sender_id, receiver_id, port */
#define PCG_DEVICE_PAYLOAD_SIZE 3

#define RTR_HEADER_SIZE 3         /* pid, mode, register_count */
#define RTR_REGISTER_READ_SIZE 2  /* address only */
#define RTR_REGISTER_WRITE_SIZE 4 /* address + value */

#define RTR_READ_REQUEST_SIZE                                                  \
  (PCG_HEADER_SIZE + RTR_HEADER_SIZE + RTR_REGISTER_READ_SIZE + PCG_CRC_SIZE)
#define RTR_READ_RESPONSE_SIZE                                                 \
  (PCG_HEADER_SIZE + RTR_HEADER_SIZE + RTR_REGISTER_WRITE_SIZE + PCG_CRC_SIZE)

#define PCG_OFF_SENDER_ID 0
#define PCG_OFF_RECEIVER_ID 1
#define PCG_OFF_PORT 2
#define PCG_OFF_DEVICE_ID_HI 3
#define PCG_OFF_DEVICE_ID_LO 4
#define PCG_OFF_DEVICE_REQUEST 5

#define RTR_OFF_PID 3
#define RTR_OFF_MODE 4
#define RTR_OFF_REG_COUNT 5
#define RTR_OFF_ADDR_HI 6
#define RTR_OFF_ADDR_LO 7
#define RTR_OFF_VALUE_HI 8
#define RTR_OFF_VALUE_LO 9

#define PCG_DEVICE_MIN_FRAME_SIZE                                              \
  (PCG_HEADER_SIZE + PCG_DEVICE_PAYLOAD_SIZE + PCG_CRC_SIZE)

#define PCG_MIN_FRAME_SIZE (PCG_HEADER_SIZE + PCG_CRC_SIZE)

#define PCG_DEVICE_REQUEST_SYNC 1
#define PCG_MQTT_RS485_DELAY_MS 20

#define PCG_DEVICE_POLL_MS 10

typedef enum {
  PCG_PROCESS_OK = 0,
  PCG_PROCESS_ERR_NULL,
  PCG_PROCESS_ERR_TOO_SHORT,
  PCG_PROCESS_ERR_CRC,
  PCG_PROCESS_ERR_SENDER_ID,
  PCG_PROCESS_ERR_RECEIVER_ID,
  PCG_PROCESS_ERR_PORT,
  PCG_PROCESS_IGNORED,
} pcg_process_result_t;

uint16_t pcg_crc16(const uint8_t *buf, size_t len);
bool pcg_check_crc(const uint8_t *data, size_t data_len, const char *log_tag);

pcg_process_result_t pcg_process(const uint8_t *data, size_t len,
                                 esp_mqtt_client_handle_t mqtt_client,
                                 uint16_t device_serial,
                                 QueueHandle_t mqtt_queue);
