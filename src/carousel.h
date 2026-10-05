#ifndef MOINK_CAROUSEL_H
#define MOINK_CAROUSEL_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

/*
 * 相册轮播（FB-017 第二部分，R1.4.0）
 *
 * 职责一句话：管理「哪些帧参与轮播 / 顺序 / 当前张」这些元数据（NVS，命名空间
 * moink，键 car_*），在定时唤醒时推进一张 —— 显示后立即回睡，全程不起 WiFi；
 * 帧本体存在 store 分区（store.c），本模块只维护「显示序 -> 物理槽」的映射。
 *
 * 关键约定：
 *   - 推进时机 = 定时唤醒且「换图节奏」到期（carousel_next_in_s 返回 1），一次
 *     到期恰好换一张；按钮唤醒不推进（仍走正常管理启动，起 WiFi 开热点）。
 *   - 换图节奏 car_int_s（R1.5.0）与「自动唤醒开热点」wake_s 完全解耦：轮播可以
 *     每小时自己换一张而全程不起 WiFi，热点按自己的间隔醒来。
 *   - cur 语义 = 最近一次显示的「显示序下标」；下一张 = cur 的一后继：顺序模式
 *     (cur+1)%n，随机模式 esp_random()%n 且重掷直到 != cur。
 *   - 删除只摘列表不擦槽：物理槽随轮转分配（car_wseq）自然复用，磨损摊匀。
 *   - 升级保留 / 出厂清空：settings_reset_for_upgrade 不动 car_*（含 car_int_s）；
 *     恢复出厂走 carousel_factory_wipe() 擦掉全部帧槽。
 *
 * 并发：状态只被 httpd 任务（端点）与开机单线程路径改写，二者不在同一时刻发生，
 * 不加锁。
 */

/* 读 NVS 状态并做自洽压缩（越界 / 重复列表项丢弃）。在 store_init 之后调用。 */
void carousel_init(void);

/* 开关已开且至少有一张可播。 */
int carousel_enabled(void);

/* 换图节奏上下限（秒），也是 /api/carousel/cfg 的 int_s 校验范围；0 = 不参与定时。 */
#define CAR_INT_MIN_S  60
#define CAR_INT_MAX_S  86400

/* 设换图间隔（秒）并立即落盘；非 0 且越界返回 ESP_ERR_INVALID_ARG，状态不变。
   改动后节奏从此刻重新起算。当前值由 /api/carousel/list 的 int_s 字段回显。 */
esp_err_t carousel_set_int(uint32_t v);

/* 距下次该换一张（power.h 的节奏约定）：0 = 轮播未开或未设间隔，不参与定时；
   1 = 已到期（该调用 carousel_boot_tick）；>1 = 还要等的秒数。 */
uint32_t carousel_next_in_s(void);

/* 定时唤醒且节奏到期时推进一张：0 = 已显示（调用方随即深睡），-1 = 不可用 / 全部读失败。
   无论成败都记消费时刻，失败时按整间隔退避，不会连续空醒。 */
int carousel_boot_tick(void);

/* 恢复出厂：擦掉全部帧槽并清内存状态（NVS 键由 settings_factory_reset 清）。 */
void carousel_factory_wipe(void);

/* 注册 /api/carousel/{list,frame,add,del,advance,cfg} 六个路由。 */
void carousel_register(httpd_handle_t server);

#endif /* MOINK_CAROUSEL_H */
