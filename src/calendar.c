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

#include "cal_face.h"
#include "frame.h"
#include "power.h"

static const char *TAG = "calendar";

#define NVS_NS "moink"
#define SECONDS_OF_DAY 86400

static struct {
    uint8_t  on;          /* 开关：到换日时刻自己醒来改日期 */
    uint32_t tod_s;       /* 换日时刻，自当天 00:00 起算的秒数 */
    uint8_t  lang;        /* 版面语言：CAL_LANG_ZH / CAL_LANG_EN */
    uint8_t  style;       /* 版面样式：0 .. CAL_STYLE_N-1（暖纸/红格/翻页牌/打卡点阵） */
} s_cal;

/* 已「处理过」的天号（epoch 天）。深睡保留、复位回 -1（= 没渲染过，醒来会补一次）。 */
RTC_DATA_ATTR static int32_t s_shown_day = -1;

static nvs_handle_t s_nvs;
static bool s_nvs_ok;

static void anchor_today(void);

/* ---------- NVS ---------- */

static void persist_cfg(void)
{
    if (!s_nvs_ok) return;
    nvs_set_u8(s_nvs, "cal_on", s_cal.on);
    nvs_set_u32(s_nvs, "cal_tod_s", s_cal.tod_s);
    nvs_set_u8(s_nvs, "cal_lang", s_cal.lang);
    nvs_set_u8(s_nvs, "cal_style", s_cal.style);
    nvs_commit(s_nvs);
}

void calendar_init(void)
{
    s_cal.on = 0;
    s_cal.tod_s = CAL_TOD_DEFAULT_S;
    s_cal.lang = CAL_LANG_ZH;
    s_cal.style = CAL_STYLE_WARM;

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

    int y, mo, d, hh, mm, ss, wd;
    if (calendar_today(&y, &mo, &d, &hh, &mm, &ss, &wd)) {
        anchor_today();      /* 热复位（RTC 走时还在、天号丢了）也不补刷，见 anchor_today */
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
    if (!wall_epoch(&now) || now < CAL_MIN_EPOCH || now > CAL_MAX_EPOCH) return 0;

    int32_t day = (int32_t)(now / SECONDS_OF_DAY);
    int32_t sod = (int32_t)(now % SECONDS_OF_DAY);
    if (day == s_shown_day)                  /* 今天已经刷过了 */
        return (uint32_t)(SECONDS_OF_DAY - sod + (int32_t)s_cal.tod_s);
    if (sod < (int32_t)s_cal.tod_s)
        return (uint32_t)((int32_t)s_cal.tod_s - sod);
    return 1;                                /* 已过换日时刻且今天还没刷 */
}

/* 时钟刚可用（掉电后页面首次推日期）时把「今天」记成已处理：否则下一次定时唤醒会
   毫无征兆地把用户刚传的图换成日历。当天首刷由页面「立即显示今天」显式触发。 */
static void anchor_today(void)
{
    if (s_shown_day >= 0) return;
    int64_t now;
    if (!wall_epoch(&now) || now < CAL_MIN_EPOCH || now > CAL_MAX_EPOCH) return;
    s_shown_day = (int32_t)(now / SECONDS_OF_DAY);
    ESP_LOGI(TAG, "day %ld anchored (今天不再自动补刷，等下一个换日时刻)", (long)s_shown_day);
}

/* 渲染并上屏当天日历。0 = 成功。无论成败都把「天号」记为已处理（见文件头）。 */
static int calendar_show(int async)
{
    int y, mo, d, hh, mm, ss, wd;
    if (!calendar_today(&y, &mo, &d, &hh, &mm, &ss, &wd)) return -1;

    cal_date_t date = { .year = y, .month = mo, .day = d, .wday = wd,
                        .lang = s_cal.lang, .style = s_cal.style };
    int r = async ? frame_render_queue(EPD_CAL_W, EPD_CAL_H, cal_face_draw, &date)
                  : frame_render_display(EPD_CAL_W, EPD_CAL_H, cal_face_draw, &date);

    int64_t now = 0;
    (void)wall_epoch(&now);
    s_shown_day = (int32_t)(now / SECONDS_OF_DAY);
    if (r == 0) ESP_LOGI(TAG, "calendar %04d-%02d-%02d displayed (%s)", y, mo, d,
                         async ? "queued" : "now");
    else ESP_LOGW(TAG, "calendar %04d-%02d-%02d draw/display failed", y, mo, d);
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

/* 统一的日期状态应答：页面据此回显「今天是几号 / 下次几点改」。 */
static void reply_state(httpd_req_t *req)
{
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0, wd = 0;
    int synced = calendar_today(&y, &mo, &d, &hh, &mm, &ss, &wd);
    int64_t now = 0;
    if (!synced || !wall_epoch(&now)) now = 0;

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"synced\":%d,\"t\":%lld,\"y\":%d,\"m\":%d,\"d\":%d,"
        "\"hh\":%d,\"mm\":%d,\"ss\":%d,\"wd\":%d,"
        "\"on\":%u,\"tod_s\":%lu,\"lang\":%u,\"style\":%u,"
        "\"shown_day\":%ld,\"next_in_s\":%lu}",
        synced ? 1 : 0, (long long)now, y, mo, d, hh, mm, ss, wd,
        (unsigned)s_cal.on, (unsigned long)s_cal.tod_s, (unsigned)s_cal.lang,
        (unsigned)s_cal.style,
        (long)s_shown_day,
        (unsigned long)calendar_next_in_s());
    if (n < 0 || n >= (int)sizeof(buf)) buf[sizeof(buf) - 1] = '\0';
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
}

/* ---------------- GET /api/time ---------------- */

static esp_err_t time_get_handler(httpd_req_t *req)
{
    power_activity();
    reply_state(req);
    return ESP_OK;
}

/* ---------------- POST /api/time  t=秒（页面按本地日历读数打包的伪 UTC 秒） ---------- */

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

    struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "clock set failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "wall clock set to %lld (from page)", t);
    anchor_today();
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
    if (!changed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no changes");
        return ESP_OK;
    }
    persist_cfg();
    /* 换语言 / 换样式都要立刻看得见：日历开着时按新配置重刷当天（异步，不挂 HTTP 应答）。
       日历关着就不抢屏 —— 与「开启日历不立刻抢屏」同一条约定。 */
    if (face_changed && s_cal.on) (void)calendar_show(1);
    reply_state(req);
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
