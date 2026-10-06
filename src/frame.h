#ifndef MOINK_FRAME_H
#define MOINK_FRAME_H

#include "esp_http_server.h"
#include <stddef.h>
#include <stdint.h>

/*
 * 帧格式 v1/v2（api = 2）：16 字节头（大端）+ 2bpp 载荷。字段与偏移不变，
 * 只有 hdr[2] 与载荷几何在 v2 上扩展：
 *
 *   偏移 0-1   魔数 0xA5 0x5A
 *   偏移 2     格式版本：1 = 768x552 载荷；2 = A1 原生 800x600 载荷
 *   偏移 3     面板类型（0=A0 / 1=A1，信息性字段）
 *   偏移 4-5   宽（BE）
 *   偏移 6-7   高（BE）
 *   偏移 8-11  载荷长度（BE）
 *   偏移 12-13 CRC16/CCITT-FALSE（poly 0x1021, init 0xFFFF，载荷字节）
 *   偏移 14-15 保留（0）
 *
 * 载荷为 2bpp 位图（MSB 优先、行自下而上 = 180° 契约，与 epd_drv.c 一致），
 * 长度为 宽/4*高：768x552 → 105984；800x600 → 120000。
 * 固件按 hdr 里的宽高判定布局。800x600 载荷只在 a1_mode 2（对照档）被接受，
 * 且缓冲必须真的有 120000 字节（见 epd_frame_geom_ok 与 frame_init）。
 */

#define FRAME_HDR_LEN    16
#define FRAME_MAGIC0     0xA5
#define FRAME_MAGIC1     0x5A
#define FRAME_FMT_VERSION 1          /* 基础格式（768x552 载荷） */
#define FRAME_FMT_VERSION_NATIVE 2   /* A1 原生 800x600 载荷 */

/*
 * 分配帧缓冲（按当前面板画像，R1.5.4 起不再是编译期最大帧）并创建同步量。
 * 分配失败时不重启、不 abort：缓冲留空，所有显示入口自行拒绝，设备照常启动
 * —— 启动期 abort 会被 bootloader 判成坏镜像并静默回滚 OTA（R1.5.3 踩过）。
 */
void frame_init(void);

/* 缓冲实际容量（字节）；frame_init 失败时为 0。写缓冲的路径都以此为上界。 */
size_t frame_buf_len(void);

/* 阻塞等待一个显示事件（上传完成 / 槽位排队）。 */
void frame_wait(void);

/* R1.0.8：残影清理（持帧锁，与传图/显示互斥）。成功返回 0。 */
int frame_clear_cycles(int cycles);

/* POST /api/frame 处理器：收头 -> 校验 -> 流式收载荷并算 CRC -> 触发显示。 */
esp_err_t frame_upload_handler(httpd_req_t *req);

/*
 * ---------- R1.4.0（FB-017 第二部分）：帧缓冲受控复用（轮播） ----------
 *
 * 上传 / 轮播加入 / 槽位显示共用同一块帧缓冲与同一把互斥锁（store 写入的
 * 并发纪律 = 必持帧锁）：
 *
 *   frame_recv_locked()     收帧（头 + 载荷 + CRC）入缓冲。成功返回 0 且锁
 *                           仍被持有，调用方随后必须 frame_unlock()（其间可
 *                           用 frame_buf() 把帧写进 store 槽位）；失败返回
 *                           负值，锁已释放且错误响应已发出。
 *   frame_show_slot()       整读槽位 -> 全屏刷新 -> 面板深睡（15~25 秒）。
 *   frame_queue_slot()      槽号交显示任务异步刷新（HTTP 应答不被刷新阻塞；
 *                           连续排队后到覆盖先到）。
 *   frame_display_pending() 显示任务入口：优先处理排队槽位，否则刷新缓冲。
 */
int frame_recv_locked(httpd_req_t *req, uint16_t *w, uint16_t *h, uint32_t *len);
void frame_unlock(void);
uint8_t *frame_buf(void);        /* 缓冲未就绪时返回 NULL，调用方必须判空 */
int frame_show_slot(uint8_t slot);
void frame_queue_slot(uint8_t slot);
int frame_display_pending(void);

/*
 * ---------- R1.5.0（功能1）：固件自绘画面（日历） ----------
 *
 * 与传图 / 轮播共用同一块帧缓冲和同一把锁：持锁 -> 调 draw 画进缓冲 -> 刷新。
 * 锁不外泄（draw 只拿到缓冲指针），所以日历和传图不可能互相踩。
 *
 *   frame_render_display()  同步：调用任务里画完就刷（15~25 秒），开机定时路径用。
 *   frame_render_queue()    异步：画完交给显示任务刷新，HTTP 应答不被刷新挂住。
 *
 * 两者都先校验几何（epd_frame_geom_ok），成功返回 0。
 */
typedef int (*frame_draw_fn)(uint8_t *fb, void *user);

int frame_render_display(uint16_t w, uint16_t h, frame_draw_fn draw, void *user);
int frame_render_queue(uint16_t w, uint16_t h, frame_draw_fn draw, void *user);

#endif /* MOINK_FRAME_H */
