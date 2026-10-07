/*
 * 墨印 · MoInk — 日历自动改日期（R1.5.0，功能1）
 *
 * 见 calendar.h 的契约说明。实现要点：
 *   - 不自己算日历：页面把「手机屏幕上的日期读数」当 UTC 秒数推过来，固件
 *     settimeofday() 之后一律用 gmtime_r() 还原 —— libc 负责 civil 换算，
 *     固件里没有一个字节的时区 / 闰年 / 日序算法。
 *   - 走时靠 RTC（CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y）：深睡期间 RTC 继续计数，
 *     醒来 gettimeofday() 就是接着走的墙上时间；掉电重上电回 1970，由窗口校验挡住。
 *   - 一天只刷一次：RTC 内存里记「已处理的天号」（epoch 天），处理过就等明天。
 *     失败也算处理过 —— 否则夜间会 30 秒一次反复空醒刷屏（比漏刷一天严重得多）。
 *   - 帧绘制借 frame.c 的持锁绘制入口（frame_render_display / frame_render_queue），
 *     与传图 / 轮播共用同一块帧缓冲和同一把锁，不额外占 106 KB。
 */
#include "calendar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cal_face.h"
#include "carousel.h"
#include "frame.h"
#include "netif_ap.h"
#include "power.h"
#include "settings.h"

static const char *TAG = "calendar";

#define NVS_NS "moink"
#define SECONDS_OF_DAY 86400

static struct {
    uint8_t  on;          /* 开关：到换日时刻自己醒来改日期 */
    uint32_t tod_s;       /* 换日时刻，自当天 00:00 起算的秒数 */
    uint8_t  lang;        /* 版面语言：CAL_LANG_ZH / CAL_LANG_EN */
    uint8_t  style;       /* 版面样式：0 .. CAL_STYLE_N-1（暖纸/红格/翻页牌/打卡点阵） */
    int32_t  tz_s;        /* 时区偏移秒（东为正）：SNTP 的真 UTC 加它才等于页面口径的墙上日期 */
} s_cal;

/* 已成功上屏的天号（epoch 天）。落 NVS（cal_day）：跨复位 / 掉电仍有效。它只回答
   「今天刷过没有」，不回答「屏上此刻挂着什么」—— 后者在掉电 / 复位 / OTA 重启之后是
   未知的（墨水屏双稳态，不上电也保留旧图），所以开机那一次不许拿它否决回屏（R1.5.3①）。 */
static int32_t s_shown_day = -1;

/* 本次开机尝试过上屏的天号，只在 RAM 里。挡的是 idle_monitor_task 每秒一次的 catchup：
   画失败不再烧天号（R1.5.3②）之后，没有这道闸就会对着一块坏屏每秒重刷。 */
static int32_t s_try_day = -1;

/* 本次开机「知不知道屏上挂着什么」。掉电 / 复位 / OTA 重启后是不知道的（双稳态留旧图），
   而上屏成功一次之后就知道了 —— 后面每一次补刷（校时成功、页面推日期、存配置）都要沿用
   这个答案，不能各自判断。开机时往往还没校上时（RTC 回 1970，无从判断今天），真正把日期
   追平的是校时之后的那一次补刷，所以这个判定必须活到那一步。 */
static bool s_screen_unknown;

/* 没校上时不许「不带定时器就睡死」：退避阶梯 1 h 起、每失败一次翻倍、12 h 封顶。
   计数器走 RTC 内存（跨深睡有效），刚掉电重上电时它是随机值，用之前一律夹到上界。 */
#define NTP_RETRY_BASE_S    3600
#define NTP_RETRY_MAX_S     43200
#define NTP_RETRY_MAX_SHIFT 4               /* 1h<<4 = 16 h，早已过封顶，阶梯到此 */
RTC_DATA_ATTR static uint8_t s_ntp_fail;

static nvs_handle_t s_nvs;
static bool s_nvs_ok;

static int today_num(int32_t *out);
static int calendar_show(int async);

/* ---------- NVS ---------- */

static void persist_cfg(void)
{
    if (!s_nvs_ok) return;
    nvs_set_u8(s_nvs, "cal_on", s_cal.on);
    nvs_set_u32(s_nvs, "cal_tod_s", s_cal.tod_s);
    nvs_set_u8(s_nvs, "cal_lang", s_cal.lang);
    nvs_set_u8(s_nvs, "cal_style", s_cal.style);
    nvs_set_i32(s_nvs, "cal_tz", s_cal.tz_s);
    nvs_commit(s_nvs);
}

static void persist_day(void)
{
    if (!s_nvs_ok) return;
    nvs_set_i32(s_nvs, "cal_day", s_shown_day);
    nvs_commit(s_nvs);
}

void calendar_init(void)
{
    s_cal.on = 0;
    s_cal.tod_s = CAL_TOD_DEFAULT_S;
    s_cal.lang = CAL_LANG_ZH;
    s_cal.style = CAL_STYLE_WARM;
    s_cal.tz_s = CAL_TZ_DEFAULT_S;

    if (nvs_open(NVS_NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed; calendar disabled");
        return;
    }
    s_nvs_ok = true;

    uint8_t u8;
    if (nvs_get_u8(s_nvs, "cal_on", &u8) == ESP_OK) s_cal.on = u8 ? 1 : 0;
    if (nvs_get_u8(s_nvs, "cal_lang", &u8) == ESP_OK)
        s_cal.lang = (u8 == CAL_LANG_EN) ? CAL_LANG_EN : CAL_LANG_ZH;
    if (nvs_get_u8(s_nvs, "cal_style", &u8) == ESP_OK)
        s_cal.style = (u8 < CAL_STYLE_N) ? u8 : CAL_STYLE_WARM;
    uint32_t v;
    if (nvs_get_u32(s_nvs, "cal_tod_s", &v) == ESP_OK && v <= CAL_TOD_MAX_S)
        s_cal.tod_s = v;
    int32_t dv;
    if (nvs_get_i32(s_nvs, "cal_day", &dv) == ESP_OK) s_shown_day = dv;
    int32_t tz;
    if (nvs_get_i32(s_nvs, "cal_tz", &tz) == ESP_OK &&
        tz >= -CAL_TZ_MAX_S && tz <= CAL_TZ_MAX_S)
        s_cal.tz_s = tz;

    /* 互斥的开机兜底：R1.5.1 及更早允许两个同时开（谁后显示谁盖掉对方），升到本版
       第一次开机时按「日历开着 = 屏幕归日历管」保留日历、关掉轮播。carousel_init()
       在本函数之前跑完，所以这里读到的是已加载的轮播状态。 */
    if (s_cal.on && carousel_switch_on()) {
        carousel_set_off();
        ESP_LOGW(TAG, "calendar and carousel were both on; carousel turned off");
    }

    int y, mo, d, hh, mm, ss, wd;
    if (calendar_today(&y, &mo, &d, &hh, &mm, &ss, &wd)) {
        /* 这里只加载配置：屏和帧缓冲还没就绪。今天没上过屏的话，由 main.c 在
           frame_init() 之后调 calendar_boot_catchup() 补刷（方案 B）。 */
        ESP_LOGI(TAG, "on=%u tod=%lus lang=%u style=%u  wall=%04d-%02d-%02d %02d:%02d:%02d wd=%d next_in=%lus",
                 s_cal.on, (unsigned long)s_cal.tod_s, (unsigned)s_cal.lang,
                 (unsigned)s_cal.style,
                 y, mo, d, hh, mm, ss, wd,
                 (unsigned long)calendar_next_in_s());
    } else {
        ESP_LOGW(TAG, "on=%u tod=%lus lang=%u style=%u  wall clock NOT synced (等页面推日期，日历暂不参与定时)",
                 s_cal.on, (unsigned long)s_cal.tod_s, (unsigned)s_cal.lang,
                 (unsigned)s_cal.style);
    }
}

/* ---------- 墙上时间 ---------- */

/* 取 RTC 走时的「伪 UTC 秒」（页面口径：本地日历读数当 UTC 打包）。 */
static int wall_epoch(int64_t *out)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    *out = (int64_t)tv.tv_sec;
    return 1;
}

int calendar_clock_synced(void)
{
    int64_t now;
    if (!wall_epoch(&now)) return 0;
    return (now >= CAL_MIN_EPOCH && now <= CAL_MAX_EPOCH) ? 1 : 0;
}

int calendar_today(int *y, int *mo, int *d, int *hh, int *mm, int *ss, int *wd)
{
    int64_t now;
    if (!wall_epoch(&now) || now < CAL_MIN_EPOCH || now > CAL_MAX_EPOCH) return 0;
    time_t t = (time_t)now;
    struct tm g;
    if (gmtime_r(&t, &g) == NULL) return 0;
    if (y)  *y  = g.tm_year + 1900;
    if (mo) *mo = g.tm_mon + 1;
    if (d)  *d  = g.tm_mday;
    if (hh) *hh = g.tm_hour;
    if (mm) *mm = g.tm_min;
    if (ss) *ss = g.tm_sec;
    if (wd) *wd = g.tm_wday;                 /* 0 = 周日，与页面日历表一致 */
    return 1;
}

uint32_t calendar_next_in_s(void)
{
    if (!s_cal.on) return 0;
    int64_t now;
    if (!wall_epoch(&now) || now < CAL_MIN_EPOCH || now > CAL_MAX_EPOCH) {
        /* 时钟不可信时绝不拿 1970 上屏 —— 但也不能因此就「不参与定时」而睡死（R1.5.3③）：
           有 STA 出口就按退避窗口再醒一次去校时，1 h 起、每失败一次翻倍、12 h 封顶。
           没有出口才真返回 0（醒了也白醒）。 */
        const moink_settings_t *st = settings_get();
        if (!st->sta_enable || !st->sta_ssid[0]) return 0;
        uint8_t n = s_ntp_fail > NTP_RETRY_MAX_SHIFT ? NTP_RETRY_MAX_SHIFT : s_ntp_fail;
        uint32_t win = NTP_RETRY_BASE_S << n;
        return win > NTP_RETRY_MAX_S ? NTP_RETRY_MAX_S : win;
    }

    int32_t day = (int32_t)(now / SECONDS_OF_DAY);
    int32_t sod = (int32_t)(now % SECONDS_OF_DAY);
    if (day == s_shown_day)                  /* 今天已经刷过了 */
        return (uint32_t)(SECONDS_OF_DAY - sod + (int32_t)s_cal.tod_s);
    if (sod < (int32_t)s_cal.tod_s)
        return (uint32_t)((int32_t)s_cal.tod_s - sod);
    return 1;                                /* 已过换日时刻且今天还没刷 */
}

/* 今天的 epoch 天号；时钟不可信返回 0。 */
static int today_num(int32_t *out)
{
    int64_t now;
    if (!wall_epoch(&now) || now < CAL_MIN_EPOCH || now > CAL_MAX_EPOCH) return 0;
    *out = (int32_t)(now / SECONDS_OF_DAY);
    return 1;
}

/* frame_task 还没创建时（开机早期），补刷必须**当场刷完才返回**：异步入口只是把画好的
   帧交给显示任务并记上「今天已上屏」，可那时任务还不存在 —— 那一帧永远上不了屏，而
   NVS 里的天号已经烧掉，这一天再也不会重试。R1.5.7② 的 frame_pending_display() 护栏
   正是为它设的，代价是「换日那一拍」被拦着不许早睡，落回正常启动、开热点、把整段空闲
   窗口烧完（L5 实测 196 秒）。R1.5.8 于是把这一拍改成同步：刷完即睡。
   闸门由 main.c 掌管：定时自醒分支起 SNTP 之前拉起来（校完时那次补刷走的是同一条路），
   frame_task 创建前落下去；之后的 HTTP / 后台任务照旧异步，不把应答挂 15~25 秒。 */
static int s_display_sync;
void calendar_display_sync(int on) { s_display_sync = on ? 1 : 0; }

/* 方案 B：日历开着 + 时钟可用 + 今天该刷 → 补刷当天。
   三个入口共用：开机（帧缓冲就绪后）、页面推来日期、换日时刻到点而设备正醒着。
   force = 「屏上此刻挂着什么未知」（掉电 / 复位 / OTA 重启），此时不许拿
   s_shown_day 当理由不回屏（R1.5.3①）；s_try_day 保证一次开机最多试一次，
   失败也不被同一个清醒窗口里的 catchup 重试刷爆。 */
static void catchup_today(int force)
{
    if (!s_cal.on) return;
    int32_t today;
    if (!today_num(&today)) return;
    if (s_try_day == today) return;
    if (!force && s_shown_day == today) return;
    ESP_LOGI(TAG, "day %ld %s on panel, catching up", (long)today,
             force ? "content unknown" : "missing");
    (void)calendar_show(!s_display_sync);
}

void calendar_boot_catchup(int screen_unknown)
{
    s_screen_unknown = screen_unknown != 0;
    catchup_today(s_screen_unknown);
}
void calendar_awake_tick(void)                 { catchup_today(0); }

int calendar_on(void) { return s_cal.on; }

/* 互斥副作用：相册轮播被打开 → 日历让出这块屏。只改配置，不动屏、不清帧，
   用户随时可以再打开日历（那时轮到轮播被关）。 */
int calendar_set_off(void)
{
    if (!s_cal.on) return 0;
    s_cal.on = 0;
    persist_cfg();
    ESP_LOGW(TAG, "calendar turned off (相册轮播开启，同一块屏只能有一个主人)");
    return 1;
}

/* ---------- SNTP 自动校时（R1.5.2，P3） ----------
 *
 * 为什么必须自己算：lwip 的默认 sntp_sync_time() 是 weak 的，它把**真 UTC** 直接
 * settimeofday 进系统 —— 本项目的口径却是「把手机屏幕上的日期当 UTC 打包」（见文件头），
 * 两者差一个时区。所以这里覆盖它，把页面告知的偏移加回去，屏上日期才和手机一致。
 */

#define NTP_SERVER_PRIMARY  "ntp.aliyun.com"
#define NTP_SERVER_FALLBACK "cn.pool.ntp.org"
#define NTP_IP_WAIT_MS      10000   /* 等 STA 拿到 IP */
#define NTP_REPLY_MS        12000   /* 单台服务器的应答预算（含随机启动延时 + DNS） */
#define NTP_MIN_AGE_S       43200   /* 距上次校时至少 12 小时才再校（12h < 每天一次自醒） */

/* 上次校时成功的单调时刻：RTC 内存，跨深睡有效、掉电归 0 —— 归 0 正是我们要的：
   掉电后 RTC 内存没了、时钟也是 1970，那次开机一定会被判定需要校时。 */
RTC_DATA_ATTR static uint32_t s_sntp_at_s;
static volatile bool s_sntp_done;

void sntp_sync_time(struct timeval *tv)
{
    int64_t wall = (int64_t)tv->tv_sec + s_cal.tz_s;
    if (wall < CAL_MIN_EPOCH || wall > CAL_MAX_EPOCH) {
        ESP_LOGW(TAG, "sntp reply out of date window (%lld), ignored", (long long)wall);
        return;
    }
    struct timeval w = { .tv_sec = (time_t)wall, .tv_usec = tv->tv_usec };
    if (settimeofday(&w, NULL) != 0) {
        ESP_LOGW(TAG, "sntp settimeofday failed");
        return;
    }
    /* weak 默认实现里这两步是它自己做的，覆盖后得自己补上。 */
    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    s_sntp_done = true;
    ESP_LOGI(TAG, "sntp: utc %lld + tz %lds -> wall %lld",
             (long long)tv->tv_sec, (long long)s_cal.tz_s, (long long)wall);
}

int calendar_sntp_needed(void)
{
    if (!s_cal.on) return 0;                       /* 日历没开，校了也没人用 */
    const moink_settings_t *st = settings_get();
    if (!st->sta_enable || !st->sta_ssid[0]) return 0;   /* 没有互联网出口 */
    if (!calendar_clock_synced()) return 1;               /* 掉电后 1970：非校不可 */
    return (power_mono_s() - s_sntp_at_s >= NTP_MIN_AGE_S) ? 1 : 0;
}

static int sntp_try(const char *server)
{
    s_sntp_done = false;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setservername(0, server);
    esp_sntp_init();
    for (int waited = 0; waited < NTP_REPLY_MS && !s_sntp_done; waited += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_sntp_stop();
    if (!s_sntp_done) ESP_LOGW(TAG, "sntp: %s silent within %d ms", server, NTP_REPLY_MS);
    return s_sntp_done ? 1 : 0;
}

void calendar_sntp_run(void)
{
    for (int waited = 0; waited < NTP_IP_WAIT_MS && !netif_sta_up(); waited += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!netif_sta_up()) {
        ESP_LOGW(TAG, "sntp: STA still has no IP after %d ms, clock untouched", NTP_IP_WAIT_MS);
        s_ntp_fail++;
        return;
    }
    if (!sntp_try(NTP_SERVER_PRIMARY) && !sntp_try(NTP_SERVER_FALLBACK)) {
        s_ntp_fail++;
        return;
    }

    s_ntp_fail = 0;             /* 校上了，退避阶梯从头再来 */
    s_sntp_at_s = power_mono_s();
    int y, mo, d, hh, mm, ss, wd;
    if (calendar_today(&y, &mo, &d, &hh, &mm, &ss, &wd)) {
        ESP_LOGI(TAG, "sntp: wall = %04d-%02d-%02d %02d:%02d (next_in=%lus)",
                 y, mo, d, hh, mm, (unsigned long)calendar_next_in_s());
    }
    /* 校完顺手检查「今天是不是还没上屏」：掉电重装 + 已过换日时刻 这种组合靠这一步补。 */
    catchup_today(s_screen_unknown);
}

static void sntp_task(void *arg)
{
    (void)arg;
    calendar_sntp_run();
    vTaskDelete(NULL);
}

void calendar_sntp_autostart(void)
{
    if (!calendar_sntp_needed()) return;
    if (xTaskCreate(sntp_task, "sntp", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "sntp task not created (heap?), clock untouched");
    }
}

/* 渲染并上屏当天日历。0 = 成功。天号只在成功时记（R1.5.3②），失败留给下一次开机重试。 */
static int calendar_show(int async)
{
    int y, mo, d, hh, mm, ss, wd;
    if (!calendar_today(&y, &mo, &d, &hh, &mm, &ss, &wd)) return -1;

    int64_t now = 0;
    (void)wall_epoch(&now);
    s_try_day = (int32_t)(now / SECONDS_OF_DAY);

    cal_date_t date = { .year = y, .month = mo, .day = d, .wday = wd,
                        .lang = s_cal.lang, .style = s_cal.style };
    int r = async ? frame_render_queue(EPD_CAL_W, EPD_CAL_H, cal_face_draw, &date)
                  : frame_render_display(EPD_CAL_W, EPD_CAL_H, cal_face_draw, &date);

    if (r == 0) {
        s_shown_day = (int32_t)(now / SECONDS_OF_DAY);
        s_screen_unknown = false;   /* 从这一刻起，屏上挂着的就是当天日历 */
        persist_day();          /* 落 NVS：复位 / 掉电后仍知道今天刷过没有 */
        ESP_LOGI(TAG, "calendar %04d-%02d-%02d displayed (%s)", y, mo, d,
                 async ? "queued" : "now");
    } else {
        /* 偶发的 EPD busy / 排队失败烧掉天号 = 这一天从此不再补，所以要留着下次试。
           同一个清醒窗口里的重复尝试由 s_try_day 挡住。 */
        ESP_LOGW(TAG, "calendar %04d-%02d-%02d draw/display failed, day left open",
                 y, mo, d);
    }
    return r;
}

int calendar_boot_tick(void) { return calendar_show(0); }

/* ---------- 请求体 / 参数解析（与 main.c、carousel.c 同一套路） ---------- */

static int kv_int(const char *qs, const char *key, int *out)
{
    size_t klen = strlen(key);
    const char *p = qs;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            *out = atoi(p + klen + 1);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

/* 64 位版：秒级时间戳过 2038 就会翻负，int 解析必须换成这个。 */
static int kv_ll(const char *qs, const char *key, long long *out)
{
    size_t klen = strlen(key);
    const char *p = qs;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            *out = strtoll(p + klen + 1, NULL, 10);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

static void read_body(httpd_req_t *req, char *buf, size_t n)
{
    size_t len = req->content_len;
    size_t cap = (len > n - 1) ? n - 1 : len;
    size_t got = 0;
    while (got < len) {
        if (got < cap) {
            int k = httpd_req_recv(req, buf + got, cap - got);
            if (k <= 0) break;
            got += (size_t)k;
        } else {
            char sink[32];
            int k = httpd_req_recv(req, sink, sizeof(sink));
            if (k <= 0) break;
            got += (size_t)k;
        }
    }
    size_t kept = (got < cap) ? got : cap;
    buf[kept] = '\0';
}

/* 统一的日期状态应答：页面据此回显「今天是几号 / 下次几点改」。
 * car_off 只在 /api/calendar/cfg 的成功应答里为 1（= 这次保存顺手关掉了相册轮播），
 * 其他应答恒为 0，字段本身始终存在，页面不必分辨新旧固件。 */
static void reply_state_ex(httpd_req_t *req, int car_off)
{
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0, wd = 0;
    int synced = calendar_today(&y, &mo, &d, &hh, &mm, &ss, &wd);
    int64_t now = 0;
    if (!synced || !wall_epoch(&now)) now = 0;

    char buf[320];
    int n = snprintf(buf, sizeof(buf),
        "{\"synced\":%d,\"t\":%lld,\"y\":%d,\"m\":%d,\"d\":%d,"
        "\"hh\":%d,\"mm\":%d,\"ss\":%d,\"wd\":%d,"
        "\"on\":%u,\"tod_s\":%lu,\"lang\":%u,\"style\":%u,"
        "\"shown_day\":%ld,\"next_in_s\":%lu,"
        "\"tz_min\":%ld,\"sntp_at\":%lu,\"mono\":%lu,"
        "\"ntp_try\":%u,\"car_off\":%d}",
        synced ? 1 : 0, (long long)now, y, mo, d, hh, mm, ss, wd,
        (unsigned)s_cal.on, (unsigned long)s_cal.tod_s, (unsigned)s_cal.lang,
        (unsigned)s_cal.style,
        (long)s_shown_day,
        (unsigned long)calendar_next_in_s(),
        (long)(s_cal.tz_s / 60), (unsigned long)s_sntp_at_s,
        (unsigned long)power_mono_s(), (unsigned)s_ntp_fail, car_off);
    if (n < 0 || n >= (int)sizeof(buf)) buf[sizeof(buf) - 1] = '\0';
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
}

static void reply_state(httpd_req_t *req) { reply_state_ex(req, 0); }

/* ---------------- GET /api/time ---------------- */

static esp_err_t time_get_handler(httpd_req_t *req)
{
    power_activity();
    reply_state(req);
    return ESP_OK;
}

/* ---------------- POST /api/time  t=秒（页面按本地日历读数打包的伪 UTC 秒）
 *                     tz_min=分钟（可选，东为正）：页面顺带告知手机时区，供 SNTP 换算
 * -------------------------------------------------------------------------- */

static esp_err_t time_post_handler(httpd_req_t *req)
{
    power_activity();

    char body[64];
    read_body(req, body, sizeof(body));

    long long t;
    if (!kv_ll(body, "t", &t)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing t");
        return ESP_OK;
    }
    if (t < (long long)CAL_MIN_EPOCH || t > (long long)CAL_MAX_EPOCH) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad time");
        return ESP_OK;
    }

    /* 时区偏移越界时只忽略它、不拒绝日期：屏上日期对不对比时区对不对重要得多。 */
    int tzmin;
    if (kv_int(body, "tz_min", &tzmin)) {
        if (tzmin >= -(CAL_TZ_MAX_S / 60) && tzmin <= CAL_TZ_MAX_S / 60) {
            s_cal.tz_s = (int32_t)tzmin * 60;
            persist_cfg();
        } else {
            ESP_LOGW(TAG, "tz_min=%d out of range, kept %lds", tzmin, (long)s_cal.tz_s);
        }
    }

    struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "clock set failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "wall clock set to %lld (from page, tz=%lds)", t, (long)s_cal.tz_s);
    catchup_today(s_screen_unknown);
    reply_state(req);
    return ESP_OK;
}

/* ------- POST /api/calendar/cfg  on=0|1 & tod_s=秒 & lang=0中|1英 & style=0..CAL_STYLE_N-1 ------- */

static esp_err_t cfg_handler(httpd_req_t *req)
{
    power_activity();

    char body[64];
    read_body(req, body, sizeof(body));

    int v, changed = 0, face_changed = 0;
    int was_on = s_cal.on;
    if (kv_int(body, "on", &v)) {
        uint8_t nv = v ? 1 : 0;
        if (nv != s_cal.on) { s_cal.on = nv; changed = 1; }
    }
    if (kv_int(body, "tod_s", &v)) {
        if (v < 0 || v > CAL_TOD_MAX_S) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad tod_s");
            return ESP_OK;
        }
        if ((uint32_t)v != s_cal.tod_s) {
            s_cal.tod_s = (uint32_t)v;
            /* 时刻改了：当天若已刷过就保持，未刷过则按新时刻重新判到期（next_in 自算）。 */
            changed = 1;
        }
    }
    if (kv_int(body, "lang", &v)) {
        uint8_t nv = (v == CAL_LANG_EN) ? CAL_LANG_EN : CAL_LANG_ZH;
        if (nv != s_cal.lang) { s_cal.lang = nv; changed = 1; face_changed = 1; }
    }
    if (kv_int(body, "style", &v)) {
        uint8_t nv = (v >= 0 && v < CAL_STYLE_N) ? (uint8_t)v : CAL_STYLE_WARM;
        if (nv != s_cal.style) { s_cal.style = nv; changed = 1; face_changed = 1; }
    }
    /* 互斥（R1.5.2）：日历被打开 → 相册轮播让出这块屏，谁后开谁赢。这个副作用要算
       一次变更，否则「日历本身没改、只为了夺回屏幕而点保存」会被 400 no changes 挡掉。 */
    int car_off = 0;
    if (s_cal.on) car_off = carousel_set_off();
    if (car_off) changed = 1;

    if (!changed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no changes");
        return ESP_OK;
    }
    persist_cfg();
    /* 换语言 / 换样式立刻看得见；刚开启日历也立刻把当天刷上屏（方案 B：
       日历开着 = 屏幕归日历管）。都是异步排队，不挂 HTTP 应答。
       turned_on 走强制重刷而不是 catchup_today：轮播可能正占着屏，开日历就得夺回来，
       哪怕 cal_day 已经记着「今天刷过」。 */
    int turned_on = s_cal.on && !was_on;
    if ((face_changed || turned_on) && s_cal.on) (void)calendar_show(1);
    else catchup_today(s_screen_unknown);
    reply_state_ex(req, car_off);
    return ESP_OK;
}

/* ---------------- POST /api/calendar/show  立刻把当天日历上屏 ---------------- */

static esp_err_t show_handler(httpd_req_t *req)
{
    power_activity();

    if (!calendar_clock_synced()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "clock not set");
        return ESP_OK;
    }
    /* 异步：15~25 秒的整屏刷新不能挂在 HTTP 应答上。 */
    if (calendar_show(1) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "draw failed");
        return ESP_OK;
    }
    reply_state(req);
    return ESP_OK;
}

/* ---------- 路由注册 ---------- */

void calendar_register(httpd_handle_t server)
{
    static const httpd_uri_t URIS[] = {
        { .uri = "/api/time",           .method = HTTP_GET,  .handler = time_get_handler },
        { .uri = "/api/time",           .method = HTTP_POST, .handler = time_post_handler },
        { .uri = "/api/calendar/cfg",   .method = HTTP_POST, .handler = cfg_handler },
        { .uri = "/api/calendar/show",  .method = HTTP_POST, .handler = show_handler },
    };
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        if (httpd_register_uri_handler(server, &URIS[i]) != ESP_OK)
            ESP_LOGW(TAG, "register %s failed", URIS[i].uri);
    }
}
