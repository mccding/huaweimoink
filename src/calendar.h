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
 *   - 掉电重上电 = RTC 复位回 1970：calendar_clock_synced() 为假，绝不拿 1970 上屏。
 *     但「这次不参与定时」不等于「睡死」（R1.5.3③）：STA 已配时按退避窗口再醒一次去
 *     校时（1 h 起、每失败一次翻倍、12 h 封顶），网络恢复后设备自己把日期追平；
 *     没配 STA 才真返回 0（醒了也白醒，只能等页面推日期）。
 *   - 换日时刻 cal_tod_s：当天 00:00 起算的秒数（默认 60 = 00:01）。一天只渲染
 *     一次 —— 用「已渲染的天号」（epoch 天）判定，渲染过就等到明天。
 *   - 与热点 wake_s、轮播 car_int_s 并列，是 power_next_wake_s() 里的一个独立节奏
 *     （约定：0 = 不参与 / 1 = 已到期 / >1 = 距到期秒数）。
 *   - 日历开着 = 屏幕归日历管（R1.5.1 方案 B）：只要「今天还没上过屏」，开机、页面推来
 *     日期、换日时刻到点而设备正醒着，这三个入口都自动补刷当天（异步，不挂调用方）。
 *     当天首刷不再依赖用户点「立即显示今天」，那个按钮退成手动重刷的兜底。
 *     已上屏天号 cal_day 落 NVS → 深睡唤醒（定时 / 按键）时补刷只在真缺的那一天发生，
 *     不会每次按键都白刷一遍屏。但它只回答「今天刷过没有」，不回答「屏上此刻挂着什么」：
 *     掉电 / 复位 / OTA 重启之后旧图还留在屏上（双稳态）而设备无从得知，所以这三种开机
 *     一律强制回屏一次，不看 cal_day（R1.5.3① —— 实机踩到：同一天内断电重上电，屏上
 *     留着手推的待办，日历就此再也回不来）。
 *   - 版面语言 cal_lang：0 = 中文、1 = 英文；版面样式 cal_style：0 = 「暖纸」（情绪板方案 B1）、
 *     1 = 「红格」（情绪板方案 A1）（cal_face.h 的 CAL_LANG_* / CAL_STYLE_*）。改语言或换样式时若
 *     日历开着，配置接口会顺手按新配置重刷当天；关着就只存配置，不抢屏。
 *   - 升级保留、出厂清空：键名以 cal_ 开头，settings_reset_for_upgrade() 白名单
 *     不碰它；恢复出厂 nvs_erase_all() 一并清掉。
 *
 * R1.5.2 追加的两件事：
 *   - SNTP 自动校时（P3）：设备自己拿真 UTC，不再只靠人推日期。因为本模块的口径是
 *     「伪 UTC 墙上日期」，真 UTC 必须加回时区偏移才是用户屏幕上的那个日期，于是
 *     页面推日期时顺带把手机的时区偏移告诉固件（tz_min，分钟、东为正），落 NVS
 *     cal_tz；没推过就用默认 +480（UTC+8，本产品的目标市场）。
 *     两个校时时机，缺一不可：① 正常启动（插电 / 按键 / 热点醒来）后台起一次性任务
 *     —— 这管的是「白天带走、晚上装上」：掉电后 RTC 回 1970，不必等人开页面，设备
 *     自己把日期追平，当晚到点就能换日；② 定时自醒路径在校时前临时起 WiFi —— 这管的
 *     是「固定时间」：RTC 走的是内部 RC 慢时钟（板上没有 32.768 kHz 晶振），每天漂
 *     几分钟，它的 00:01 会慢慢前移，不校时早晚会在白天挂出「明天」。
 *     覆盖 lwip 的 weak sntp_sync_time() 是关键：默认实现直接 settimeofday(真 UTC)，
 *     那样屏上日期会整整差一个时区。
 *   - 与相册轮播互斥：两者抢的是同一块屏，同开时换日那天会被下一次换图盖掉。
 *     谁后开谁赢 —— 开日历就关轮播，开轮播就关日历，由服务端强制；开机时若发现
 *     历史遗留的同开状态，按「日历开着 = 屏幕归日历管」保留日历。
 */

/* 换日时刻：当天 00:00 起算的秒数。 */
#define CAL_TOD_MAX_S     86399
#define CAL_TOD_DEFAULT_S 60

/* 时区偏移（秒，东为正）。页面没推过之前的默认值按目标市场取 UTC+8。 */
#define CAL_TZ_DEFAULT_S  (8 * 3600)
#define CAL_TZ_MAX_S      (14 * 3600)

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
   只有成功才把「天号」记下来（R1.5.3②）—— 烧掉它等于这一整天从此不再补；失败留到
   下一次开机 / 补刷入口重试，同一个清醒窗口里由 s_try_day 挡住，不会秒级连刷。 */
int calendar_boot_tick(void);

/* 开机补刷：frame_init() 之后调用。参数 = 屏上此刻挂着什么是否未知：
   掉电 / 复位 / OTA 重启传 1（不看 cal_day，强制回屏一次），从深睡醒来传 0
   （屏内容原样保留，仍按天号跳过，免得每次按键唤醒白刷一遍）。
   幂等：一次开机最多试一次，失败也不会被每秒的 catchup 重刷。 */
void calendar_boot_catchup(int screen_unknown);

/* 醒着时的换日兜底：定时器路径之外（设备被页面喂着没睡、或睡前的那几秒）也要在
   换日时刻把当天刷上屏。给 idle_monitor_task 每秒调一次，幂等。 */
void calendar_awake_tick(void);

/* 日历当前开关（cal_on，不含「时钟有没有同步」的判断）。 */
int calendar_on(void);

/* 关掉日历并落 NVS —— 相册轮播被打开时的互斥副作用，只改配置不动屏。
   返回 1 = 本次真的从「开」变「关」（0 = 本来就关着）。 */
int calendar_set_off(void);

/* 是否需要自动校时：日历开着 + STA 已配（有互联网出口）+（时钟不可信 或 距上次校时
   已超过 12 小时）。掉电后 RTC 内存归零，这里必然为真，所以「装上电池」那次一定会校。 */
int calendar_sntp_needed(void);

/* 阻塞式校时（内部自带超时，最坏约 34 秒）：等 STA 拿 IP → 主服务器 → 备用服务器，
   任一成功则 settimeofday(真 UTC + tz) 并按方案 B 补刷当天。调用前 WiFi 必须已起
   （netif_ap_init），但**不需要**起 HTTP 服务。失败只是不校，不改时钟。 */
void calendar_sntp_run(void);

/* 正常启动路径用：需要校时则起一个一次性后台任务去做（不挂住启动），不需要时什么都不做。 */
void calendar_sntp_autostart(void);

/* 注册 /api/time（读写墙上日期）、/api/calendar/cfg（开关 + 换日时刻 + 语言 + 样式）、
   /api/calendar/show（立刻上屏当天）。 */
void calendar_register(httpd_handle_t server);

#endif /* MOINK_CALENDAR_H */
