/*
 * 墨印 · MoInk — store 原始帧存储（FB-017：R1.2.0 分区底座 + R1.4.0 槽位读写）
 *
 * 职责一句话：把 "store" 分区当作 STORE_SLOT_MAX 个定长槽位（各 128 KB），
 * 提供「探测 / 探读元数据 / 整读校验 / 整槽轮转写」四种原语；不与 NVS 和
 * 轮播业务耦合（游标等元数据都归 carousel.c 管）。
 *
 * 为什么不用文件系统（LittleFS / FATFS）：轮播帧是「定长大对象、写一次读多次」，
 * 整槽轮转写入的写放大 = 恰好 1（每张图消耗一次整槽擦除），没有任何文件系统
 * 元数据开销；LittleFS 的写时复制与元数据提交反而会带来额外擦写。
 *
 * 槽内布局（STORE_HDR_LEN = 16 字节头 + 载荷）：
 *   偏移 0-3  魔数 "MST1"
 *   偏移 4-5  宽 u16 LE        偏移 6-7  高 u16 LE
 *   偏移 8-11 载荷长度 u32 LE  偏移 12-13 CRC16（LE）  偏移 14-15 保留（0）
 *
 * ★ 上电安全顺序（见 store_slot_write 注释）：先擦整槽 → 写载荷 → 最后写头。
 *   任何时点掉电，槽要么无头（视为空），要么头 + 载荷与 CRC 自洽；不存在
 *   「头有效、载荷半截」的假有效槽。头写在槽首，掉电后残留内容全部落在头之外
 *   或被头覆盖 —— 头本身也是最后才出现的。
 */
#include "store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "store";

static const esp_partition_t *s_part;
static uint32_t s_slots;

/* CRC-16/CCITT-FALSE：poly 0x1021，init 0xFFFF，无反射、无终异或。
   （与 frame.c 的同名实现逐位一致；两处均 static，避免新增公共头依赖。） */
static uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* 载荷几何自洽校验（与 frame.c 的宽高规则同一口径；面板无关）。 */
static int meta_ok(uint16_t w, uint16_t h, uint32_t len)
{
    if ((w & 3) || h == 0) return 0;
    if (len == 0 || len > STORE_FRAME_MAX) return 0;
    if (len != (uint32_t)(w / 4) * (uint32_t)h) return 0;
    return 1;
}

static uint32_t slot_off(uint8_t slot)
{
    return (uint32_t)slot * STORE_SLOT_SZ;
}

void store_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY, "store");
    if (!s_part) {
        s_slots = 0;
        ESP_LOGW(TAG, "store partition not found (flashed over an older "
                      "partition table via OTA?) - carousel storage unavailable");
        return;
    }

    s_slots = s_part->size / STORE_SLOT_SZ;
    if (s_slots > STORE_SLOT_MAX) s_slots = STORE_SLOT_MAX;
    ESP_LOGI(TAG, "store: off=0x%06lx size=%lu KB, %lu slot(s) x %u KB "
                  "(frame <= %u B)",
             (unsigned long)s_part->address, (unsigned long)(s_part->size / 1024),
             (unsigned long)s_slots, (unsigned)(STORE_SLOT_SZ / 1024),
             (unsigned)STORE_FRAME_MAX);
}

int store_slots(void)
{
    return (int)s_slots;
}

int store_slot_peek(uint8_t slot, store_meta_t *meta)
{
    if (!s_part || slot >= s_slots) return -1;

    uint8_t hdr[STORE_HDR_LEN];
    if (esp_partition_read(s_part, slot_off(slot), hdr, sizeof(hdr)) != ESP_OK)
        return -1;
    if (memcmp(hdr, "MST1", 4) != 0) return -1;   /* 空槽 / 残槽 */

    uint16_t w   = (uint16_t)(hdr[4] | (hdr[5] << 8));
    uint16_t h   = (uint16_t)(hdr[6] | (hdr[7] << 8));
    uint32_t len = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
                   ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    uint16_t crc = (uint16_t)(hdr[12] | (hdr[13] << 8));
    if (!meta_ok(w, h, len)) return -1;

    if (meta) {
        meta->w = w;
        meta->h = h;
        meta->len = len;
        meta->crc = crc;
    }
    return 0;
}

int store_slot_read(uint8_t slot, uint8_t *buf, size_t cap, store_meta_t *meta)
{
    store_meta_t m;
    if (store_slot_peek(slot, &m) != 0) return -1;

    if (buf) {
        /* 缓冲是按画像分配的，可能装不下这一槽 —— 越界写会把堆直接写坏。 */
        if (m.len > cap) {
            ESP_LOGE(TAG, "slot %u holds %lu bytes, buffer only %u", slot,
                     (unsigned long)m.len, (unsigned)cap);
            return -1;
        }
        uint32_t off = slot_off(slot) + STORE_HDR_LEN;
        if (esp_partition_read(s_part, off, buf, m.len) != ESP_OK) return -1;
        if (crc16_update(0xFFFF, buf, m.len) != m.crc) return -1;
    }
    if (meta) *meta = m;
    return 0;
}

int store_slot_write(uint8_t slot, const uint8_t *buf, uint16_t w, uint16_t h,
                     uint32_t len)
{
    if (!s_part || slot >= s_slots) return -1;
    if (!buf || !meta_ok(w, h, len)) return -1;
    /* 单槽必须装得下头 + 载荷（STORE_FRAME_MAX 已保证，防御性再验一次）。 */
    if ((uint32_t)STORE_HDR_LEN + len > STORE_SLOT_SZ) return -1;

    uint32_t off = slot_off(slot);

    /* 1) 整槽擦除：旧头随之消失 —— 此后任何时点掉电，本槽都读作「空」。 */
    if (esp_partition_erase_range(s_part, off, STORE_SLOT_SZ) != ESP_OK) {
        ESP_LOGE(TAG, "slot %u erase failed", slot);
        return -1;
    }

    /* 2) 写载荷 + 读回校验（分块 4 KB，栈上小缓冲，不占第二份整帧内存）。 */
    uint8_t chunk[256];
    uint16_t crc = 0xFFFF;
    uint32_t pos = 0;
    while (pos < len) {
        uint32_t n = len - pos;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (esp_partition_write(s_part, off + STORE_HDR_LEN + pos, buf + pos, n)
                != ESP_OK) {
            ESP_LOGE(TAG, "slot %u payload write failed @%lu", slot,
                     (unsigned long)pos);
            return -1;
        }
        crc = crc16_update(crc, buf + pos, n);
        pos += n;
    }

    uint32_t back = 0;
    uint16_t vcrc = 0xFFFF;
    while (back < len) {
        uint32_t n = len - back;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (esp_partition_read(s_part, off + STORE_HDR_LEN + back, chunk, n)
                != ESP_OK) {
            ESP_LOGE(TAG, "slot %u readback failed", slot);
            return -1;
        }
        vcrc = crc16_update(vcrc, chunk, n);
        back += n;
    }
    if (vcrc != crc) {
        ESP_LOGE(TAG, "slot %u readback CRC mismatch", slot);
        return -1;
    }

    /* 3) 头最后写：载荷已验证，此刻才让本槽「可见」。 */
    uint8_t hdr[STORE_HDR_LEN] = { 'M', 'S', 'T', '1',
        (uint8_t)(w & 0xFF), (uint8_t)(w >> 8),
        (uint8_t)(h & 0xFF), (uint8_t)(h >> 8),
        (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF),
        (uint8_t)((len >> 16) & 0xFF), (uint8_t)((len >> 24) & 0xFF),
        (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8), 0, 0 };
    if (esp_partition_write(s_part, off, hdr, sizeof(hdr)) != ESP_OK) {
        ESP_LOGE(TAG, "slot %u header write failed", slot);
        return -1;
    }

    ESP_LOGI(TAG, "slot %u written: %ux%u %lu B crc=0x%04x", slot, w, h,
             (unsigned long)len, crc);
    return 0;
}

int store_slot_verify(uint8_t slot, store_meta_t *meta)
{
    store_meta_t m;
    if (store_slot_peek(slot, &m) != 0) return -1;

    uint8_t chunk[256];
    uint32_t off = slot_off(slot) + STORE_HDR_LEN;
    uint16_t crc = 0xFFFF;
    uint32_t pos = 0;
    while (pos < m.len) {
        uint32_t n = m.len - pos;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (esp_partition_read(s_part, off + pos, chunk, n) != ESP_OK) return -1;
        crc = crc16_update(crc, chunk, n);
        pos += n;
    }
    if (crc != m.crc) return -1;
    if (meta) *meta = m;
    return 0;
}

int store_slot_read_span(uint8_t slot, uint32_t off, uint8_t *buf, size_t n)
{
    if (!s_part || slot >= s_slots) return -1;
    return esp_partition_read(s_part, slot_off(slot) + STORE_HDR_LEN + off,
                              buf, n) == ESP_OK ? 0 : -1;
}

void store_wipe_all(void)
{
    if (!s_part || s_slots == 0) return;
    esp_err_t e = esp_partition_erase_range(s_part, 0, s_part->size);
    ESP_LOGI(TAG, "store wiped: %s", e == ESP_OK ? "ok" : "failed");
}
