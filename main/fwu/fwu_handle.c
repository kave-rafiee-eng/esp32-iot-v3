#include "fwu_handle.h"

#include "esp_log.h"

#include "fwu.h"
#include "pcg_rtr.h"

static const char *TAG = "fwu_handle";

void fwu_handle_packet(const pcg_packet_t *pkt, fwu_mqtt_publish_fn mqtt_publish, void *ctx)
{
    uint8_t payload[FWU_WRITE_SIZE];
    size_t payload_len = 0;

    if (pkt == NULL) {
        return;
    }

    if (!fwu_build_reply(pkt->payload, pkt->payload_len, payload, sizeof(payload), &payload_len)) {
        ESP_LOGW(TAG, "FWU rejected, pid=%u", pkt->pid);
        return;
    }

    uint8_t frame[PCG_HEADER_SIZE + FWU_WRITE_SIZE + PCG_CRC_SIZE];
    size_t frame_len = pcg_build(frame, sizeof(frame),
                                 pkt->pid, PCG_PEER_ESP32, 0,
                                 pkt->sender_type, PCG_PORT_FWU,
                                 payload, payload_len);
    if (frame_len == 0) {
        ESP_LOGE(TAG, "FWU reply build failed");
        return;
    }

    if (pkt->sender_type == PCG_PEER_ADVANCE) {
        ESP_LOGI(TAG, "FWU reply -> serial, len=%u", (unsigned)frame_len);
        rtr_rs485_send(frame, frame_len);
        return;
    }

    if (mqtt_publish == NULL) {
        ESP_LOGW(TAG, "FWU cannot reply to sender %u (no MQTT publish)", pkt->sender_type);
        return;
    }

    mqtt_publish(pkt->sender_type, frame, frame_len, ctx);
}
