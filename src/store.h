#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * 轮播帧存储（FB-017）：分区探测（第一部分，随 R1.2.0 并入）+ 槽位读写（第二部分，R1.4.0）。
 * 提前把 store 分区固化进 4MB 分区表，是因为分区表只能靠整包线刷改写（OTA 改不了）。
 *
 * 槽位布局（写入策略详见 docs/调研-内置Flash延寿方案.md）：
 *   - 槽位固定 128 KB（STORE_SLOT_SZ），共 STORE_SLOT_MAX 个；单槽可容纳最大帧
 *     800x600 2bpp = 120,000 B（768x552 = 105,984 B）。
 *   - 轮播图片「整槽轮转写入」：新图永远写空闲槽（round-robin），绝不原地覆写，
 *     把擦写均匀摊到全部槽位上；轮播游标（当前张 / 张数 / 模式）存 NVS
 *     （NVS 自带磨损均衡），store 分区内不放会被反复改写的元数据。
 *   - 槽内 16B 头：magic "MST1" + w/h/len（小端）+ 载荷 CRC16 + 保留。
 *     头无效（从未写过）的槽读作「空槽」。
 *
 * 并发：store 自身不加锁。写槽（erase + write，耗时长）只由持有帧锁的调用方发起
 * （收帧路径已持锁，见 frame.h），避免与传图 / 刷图并发争用 SPI flash；只读类
 * 调用（peek / verify / read_span，读进调用方自己的小缓冲）不碰共享状态，不受限。
 */

#define STORE_SLOT_SZ   0x20000u   /* 单槽 128 KB */
#define STORE_SLOT_MAX  8u         /* 0x110000 / 0x20000 */
#define STORE_FRAME_MAX 120000u    /* 一帧的最大字节数（800x600 2bpp） */

#define STORE_HDR_LEN   16

typedef struct {
    uint16_t w;
    uint16_t h;
    uint32_t len;
    uint16_t crc;      /* CRC16/CCITT-FALSE（poly 0x1021, init 0xFFFF），载荷字节 */
} store_meta_t;

/* 启动时探测 "store" 分区；缺失（旧分区表 OTA 上来的设备）时 slots = 0。 */
void store_init(void);

/* 可用槽位数；0 = 分区不存在，轮播功能不可用。 */
int store_slots(void);

/* 读槽头并做几何自洽校验（不读载荷、不验 CRC）。0 = 槽内有帧。 */
int store_slot_peek(uint8_t slot, store_meta_t *meta);

/* 读整槽：头 + 载荷 + CRC 全验。buf 容量须 >= STORE_FRAME_MAX。0 = 有效。 */
int store_slot_read(uint8_t slot, uint8_t *buf, store_meta_t *meta);

/* 整槽重写：擦槽 -> 写载荷 -> 写头 -> 读回校验。0 = 成功。
   先载荷后头的顺序保证断电中断只会留下「无头空槽」，不会出现
   「头像有效、载荷半截」的假有效槽。 */
int store_slot_write(uint8_t slot, const uint8_t *buf, uint16_t w, uint16_t h, uint32_t len);

/* 校验整槽载荷 CRC：分块读（不占整帧缓冲）。0 = 有效；meta 非空时回填元数据。
   轮播出图（HTTP 缩略图流）先 verify 再分片读，避免第二份 120 KB 缓冲。 */
int store_slot_verify(uint8_t slot, store_meta_t *meta);

/* 读槽载荷的一段（off 相对载荷起点；调用方先 verify / peek 自证范围合法）。 */
int store_slot_read_span(uint8_t slot, uint32_t off, uint8_t *buf, size_t n);

/* 擦除整个 store 分区（恢复出厂清帧用；不可逆）。 */
void store_wipe_all(void);
