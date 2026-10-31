#include "fwu.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "fwu";

#define FWU_SECTOR_SIZE 4096
#define FWU_PARTITION_LABEL "fwu"

static const esp_partition_t *s_part;
static uint8_t s_sector[FWU_SECTOR_SIZE] __attribute__((aligned(4)));

esp_err_t fwu_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY,
                                      FWU_PARTITION_LABEL);
    if (s_part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found", FWU_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "partition '%s' size=%u address=0x%08lx",
             FWU_PARTITION_LABEL, (unsigned)s_part->size, (unsigned long)s_part->address);
    return ESP_OK;
}

static bool fwu_map(const uint8_t *addr, uint32_t *offset, bool need_sector)
{
    /* Address is a block index (LE), matching userpanel blockAddress(). */
    uint32_t block = (uint32_t)addr[0]
                   | ((uint32_t)addr[1] << 8)
                   | ((uint32_t)addr[2] << 16)
                   | ((uint32_t)addr[3] << 24);

    if (block > (UINT32_MAX / FWU_DATA_SIZE)) {
        ESP_LOGW(TAG, "block %lu overflow", (unsigned long)block);
        return false;
    }

    uint32_t off = block * FWU_DATA_SIZE;

    if (s_part == NULL || (uint64_t)off + FWU_DATA_SIZE > s_part->size) {
        ESP_LOGW(TAG, "block %lu offset 0x%08lx outside partition",
                 (unsigned long)block, (unsigned long)off);
        return false;
    }

    if (need_sector) {
        uint32_t sector = off & ~(uint32_t)(FWU_SECTOR_SIZE - 1);
        if ((uint64_t)sector + FWU_SECTOR_SIZE > s_part->size) {
            ESP_LOGW(TAG, "sector 0x%08lx exceeds partition", (unsigned long)sector);
            return false;
        }
    }

    *offset = off;
    return true;
}

static bool block_is_erased(const uint8_t *block)
{
    for (size_t i = 0; i < FWU_DATA_SIZE; i++) {
        if (block[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static bool fwu_write_block(uint32_t offset, const uint8_t *data)
{
    uint32_t sector = offset & ~(uint32_t)(FWU_SECTOR_SIZE - 1);
    uint32_t inner = offset - sector;

    esp_err_t err = esp_partition_read(s_part, sector, s_sector, FWU_SECTOR_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sector read failed: %s", esp_err_to_name(err));
        return false;
    }

    memcpy(s_sector + inner, data, FWU_DATA_SIZE);

    err = esp_partition_erase_range(s_part, sector, FWU_SECTOR_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sector erase failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_partition_write(s_part, sector, s_sector, FWU_SECTOR_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sector write failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

bool fwu_build_reply(const uint8_t *req, size_t req_len,
                     uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (req == NULL || out == NULL || out_len == NULL) {
        return false;
    }

    if (s_part == NULL) {
        ESP_LOGE(TAG, "partition unavailable");
        return false;
    }

    if (req_len < FWU_READ_SIZE) {
        ESP_LOGW(TAG, "payload too short: %u", (unsigned)req_len);
        return false;
    }

    uint8_t mode = req[0];
    const uint8_t *addr = req + 1;
    uint32_t offset = 0;

    if (mode == FWU_MODE_READ) {
        if (req_len != FWU_READ_SIZE) {
            ESP_LOGW(TAG, "READ length mismatch: %u", (unsigned)req_len);
            return false;
        }
        if (!fwu_map(addr, &offset, false)) {
            return false;
        }
        if (out_cap < FWU_WRITE_SIZE) {
            return false;
        }

        uint8_t block[FWU_DATA_SIZE] __attribute__((aligned(4)));
        esp_err_t err = esp_partition_read(s_part, offset, block, FWU_DATA_SIZE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(err));
            return false;
        }

        /* Erased flash is 0xFF. A bootloader must see image size 0, not 0xFFFFFFFF. */
        if (block_is_erased(block)) {
            memset(block, 0, FWU_DATA_SIZE);
            ESP_LOGI(TAG, "READ offset=0x%08lx erased, reply zeros", (unsigned long)offset);
        } else {
            ESP_LOGI(TAG, "READ offset=0x%08lx", (unsigned long)offset);
        }

        out[0] = FWU_MODE_WRITE;
        memcpy(out + 1, addr, FWU_ADDR_SIZE);
        memcpy(out + 1 + FWU_ADDR_SIZE, block, FWU_DATA_SIZE);
        *out_len = FWU_WRITE_SIZE;
        return true;
    }

    if (mode == FWU_MODE_WRITE) {
        if (req_len != FWU_WRITE_SIZE) {
            ESP_LOGW(TAG, "WRITE length mismatch: %u", (unsigned)req_len);
            return false;
        }
        if (!fwu_map(addr, &offset, true)) {
            return false;
        }
        if (out_cap < FWU_READ_SIZE) {
            return false;
        }
        if (!fwu_write_block(offset, req + 1 + FWU_ADDR_SIZE)) {
            return false;
        }

        out[0] = FWU_MODE_ACK;
        memcpy(out + 1, addr, FWU_ADDR_SIZE);
        *out_len = FWU_READ_SIZE;
        ESP_LOGI(TAG, "WRITE offset=0x%08lx", (unsigned long)offset);
        return true;
    }

    ESP_LOGW(TAG, "mode ignored: %u", mode);
    return false;
}
