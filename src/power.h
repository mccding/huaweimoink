#ifndef MOINK_POWER_H
#define MOINK_POWER_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 电源管理：电池 ADC 粗测、深睡 / 唤醒源、空闲休眠判定、定时唤醒节奏。
 *
 * 休眠策略：
 *   - sleep_s = 空闲无活动后深睡的超时（0 = 不休眠）。
 *   - 定时唤醒后进入「等人的窗口」：默认 AUTO_WAKE_WINDOW_S 秒内若无活动则回睡，
 *     有上传活动则按正常 sleep_s 续命。
 *
 * 唤醒节奏（R1.5.0）：wake_s（开热点等人传图）、轮播换图间隔、日历改日期各自
 * 独立计时，入睡时取「最近的一个到期时刻」武装 RTC 定时器；醒来只做到期的那件事，
 * 没到期的（尤其是热点）不做 —— 定时换图因此可以完全不起 WiFi。
 *
 * 各节奏查询函数统一约定返回值：
 *   0 = 该节奏未开启（不参与定时）
 *   1 = 已到期，该干活了
 *   >1 = 距到期还有这么多秒
 *
 * 「不休眠」不再等于「永不入睡」：只要有任何节奏开启，仍会在短窗口后入睡，
 * 否则定时器永远不会被武装（R1.4.0 的死角：开轮播 + 不休眠 = 轮播不动）。
 */

/* 配置唤醒键输入 + 电池 ADC。 */
void power_init(void);

/* 重置空闲计时（任何上传 / 请求都调用）。 */
void power_activity(void);

/* 距最后一次请求 / 上传过了多少秒（只读，不改任何休眠判定）。
   给「换完图提前回睡」用：传输还在一片一片地 call power_activity()，静默够久
   才说明这条连接真的停了，此刻入睡才不会把 OTA 写入或传图拦腰截断。 */
uint32_t power_idle_s(void);

/* 本次是否为定时器唤醒（决定是否套用短窗口）。 */
void power_set_auto_wake(bool v);
bool power_auto_wake(void);

/* 当前唤醒原因位图（esp_sleep_get_wakeup_causes()）。 */
uint32_t power_wakeup_causes(void);

/*
 * 单调秒计数：含深睡时长，掉电归零。
 * C3 的 esp_timer 由 XTAL 供时钟，深睡期间停走且唤醒后从 0 重新计数，不能直接用来
 * 跨睡眠计时；这里在入睡前把本次清醒时长折进 RTC 内存累加器，醒来若确为定时唤醒再
 * 补上「上次武装的秒数」，两者相加即近似真实经过时间（误差 = 关机/启动开销）。
 * 所有节奏的消费时刻都用它打点。
 */
uint32_t power_mono_s(void);

/* 距下次「开热点等人」定时器唤醒的秒数（约定见文件头；0 = wake_s 关闭）。 */
uint32_t power_hotspot_in_s(void);

/* 本次开机走了正常管理启动（热点已开）：热点节奏从此刻重新起算。 */
void power_mark_hotspot_wake(void);

/* 各节奏里最近的到期时刻（= 入睡时该武装的秒数）；0 = 一个节奏都没开。 */
uint32_t power_next_wake_s(void);

/* 空闲是否已超时，应进入深睡。 */
bool power_should_sleep(void);

/* 设唤醒源并进入深睡（不返回）。 */
void power_enter_deep_sleep(void);

/* 电池电压 mV 粗测（分压 470k:100k；满量程需按板实测标定）。 */
int power_battery_mv(void);

#endif /* MOINK_POWER_H */
