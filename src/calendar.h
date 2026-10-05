#ifndef MOINK_CALENDAR_H
#define MOINK_CALENDAR_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

/*
 * 日历自动改日期（R1.5.0，功能1）
 *
 * 职责一句话：把「今天几号」变成设备自己的定时任务 —— 页面把手机上的**墙上日期**
 * 推给它（POST /api/time），它靠 RTC 走时（settimeofday 跨深睡有效），每天到用户
 * 指定的换日时刻自己醒来、把当天日历上屏、接着睡，全程不起 WiFi。
 *
 * 关键约定：
 *   - 设备只认「墙上日期」，不认时区：页面按本地日历读数打包
 *     `t = Date.UTC(年,月,日,时,分,秒)/1000`（即「把本地时间当作 UTC 的秒数」），
 *     固件一律用 gmtime_r() 还原 —— 还原出来的就是用户手机屏幕上那个日期。
 *     于是固件侧零时区、零 civil 算法，也不需要 NVS 里存日期基准。
 *   - 掉电重上电 = RTC 复位回 1970：calendar_clock_synced() 为假，此时日历节奏
 *     不参与定时（页面提示「先同步日期」），绝不拿 1970 去上屏。
 *   - 换日时刻 cal_tod_s：当天 00:00 起算的秒数（默认 60 = 00:01）。一天只渲染
 *     一次 —— 用「已渲染的天号」（epoch 天）判定，渲染过就等到明天。
 *   - 与热点 wake_s、轮播 car_int_s 并列，是 power_next_wake_s() 里的一个独立节奏
 *     （约定：0 = 不参与 / 1 = 已到期 / >1 = 距到期秒数）。
 *   - 开启日历**不会**立刻抢屏：时钟一可用就把「今天」记成已处理，当天首刷由页面
 *     「立即显示今天」（POST /api/calendar/show）显式触发，之后才交给换日时刻。
 *   - 版面语言 cal_lang：0 = 中文、1 = 英文；版面样式 cal_style：0 = 「暖纸」（情绪板方案 B1）、
 *     1 = 「红格」（情绪板方案 A1）（cal_face.h 的 CAL_LANG_* / CAL_STYLE_*）。改语言或换样式时若
 *     日历开着，配置接口会顺手按新配置重刷当天；关着就只存配置，不抢屏。
 *   - 升级保留、出厂清空：键名以 cal_ 开头，settings_reset_for_upgrade() 白名单
 *     不碰它；恢复出厂 nvs_erase_all() 一并清掉。
 */

/* 换日时刻：当天 00:00 起算的秒数。 */
#define CAL_TOD_MAX_S     86399
#define CAL_TOD_DEFAULT_S 60

/* 合理日期窗口（页面按本地墙上时间打包的「伪 UTC 秒」）：
   2020-01-01 .. 2099-12-31。窗口外一律视为没同步过 / 参数非法。 */
#define CAL_MIN_EPOCH 1577836800
#define CAL_MAX_EPOCH 4102444799

/* 读 NVS 配置（cal_on / cal_tod_s / cal_lang / cal_style）。在 settings_init / power_init 之后调用。 */
void calendar_init(void);

/* RTC 里是可信日期（在 CAL_MIN_EPOCH..CAL_MAX_EPOCH 窗口内）。 */
int calendar_clock_synced(void);

/* 距下次该改日期的秒数（power.h 的节奏约定）：
   0 = 未开启或日期未同步，不参与定时；
   1 = 已到期且今天还没渲染过（该调用 calendar_boot_tick）；
   >1 = 还要等的秒数（今天已渲染过时直接给到明天）。 */
uint32_t calendar_next_in_s(void);

/* 当前墙上日期（gmtime_r 还原，字段就是用户看到的日历读数）。
   返回 0 = 时钟不可信（未同步），字段不写。 */
int calendar_today(int *y, int *mo, int *d, int *hh, int *mm, int *ss, int *wd);

/* 定时唤醒且换日节奏到期时渲染当天日历上屏：0 = 已显示，-1 = 不可用 / 绘制失败。
   成败都把「天号」记为已处理，失败按一整天退避，不会秒级连醒连刷。 */
int calendar_boot_tick(void);

/* 注册 /api/time（读写墙上日期）、/api/calendar/cfg（开关 + 换日时刻 + 语言 + 样式）、
   /api/calendar/show（立刻上屏当天）。 */
void calendar_register(httpd_handle_t server);

#endif /* MOINK_CALENDAR_H */
