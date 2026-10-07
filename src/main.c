/*
 * 墨印 · MoInk — 固件主装配（R 系列，重构版）
 *
 * 职责一句话：固件只做「热点 / 接收 / 刷图 / 休眠 / 唤醒 / OTA」，画面全部由页面
 * 在浏览器里合成。没有任何 STA 配网、没有拉图、没有固件端图像处理。
 *
 * 模块分工：
 *   epd_drv   锁定屏驱动（A0/A1，RST=GPIO3），不改
 *   settings  持久化：面板 / 翻转 / 休眠时长 / 自动唤醒 / 热点凭据
 *   netif_ap  纯 SoftAP（默认开放，SSID 默认 MoInk-XXXX）
 *   captive   DNS 劫持 + OS 探测劫持（连上即弹控制页）
 *   frame     帧接收（16B 头 + CRC16 校验）+ 显示触发
 *   power     电池 ADC / 深睡 / 唤醒源 / 空闲休眠
 *   store     轮播帧存储（store 分区，8 × 128 KB 槽位轮转）
 *   carousel  相册轮播：定时唤醒换一张（NVS 游标 + 槽位表）
 *   ota_web   OTA 双槽 + 控制页热更（web 分区）+ 统一升级入口
 *
 * 路由：
 *   GET  /                控制页（web 分区优先，否则内嵌页）
 *   GET  /api/info        状态（版本 / 接口 / 面板 / 堆 / 电量 / 客户端数）
 *   GET/POST /api/settings 读写设置
 *   POST /api/frame       传图
 *   POST /api/upload      ★统一升级入口（自动识别 .bin 固件 / .html 控制页）
 *   POST /api/web/clear   清除已上传控制页，回退内嵌页
 *   POST /api/clear       残影清理
 *   POST /api/factory     恢复出厂（连轮播帧一起清）
 *   GET  /api/carousel/list      轮播状态 + 帧列表
 *   GET  /api/carousel/frame     第 i 张帧缩略数据（载荷裸字节）
 *   POST /api/carousel/add       收帧入轮播槽
 *   POST /api/carousel/del?i=k   删除第 k 张
 *   POST /api/carousel/advance   立即下一张
 *   POST /api/carousel/cfg       开关 / 模式 / 换图节奏 int_s
 *   GET  /api/time               墙上日期 + 日历状态（同步了吗 / 下次几点改）
 *   POST /api/time               t=伪 UTC 秒（页面把手机日历读数按 UTC 打包推来）
 *   POST /api/calendar/cfg       on=0|1 & tod_s=换日时刻（当天 00:00 起算秒数）
 *   POST /api/calendar/show      立刻把当天日历上屏
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "version.h"
#include "epd_drv.h"
#include "settings.h"
#include "netif_ap.h"
#include "captive.h"
#include "frame.h"
#include "power.h"
#include "ota_web.h"
#include "store.h"
#include "carousel.h"
#include "calendar.h"
#include "todo.h"

#include "index_html.h"     /* 由 tools/make_index_header.py 固化，改页必重跑 */

static const char *TAG = "main";

#define HOLD_RESET_S    5          /* 长按恢复出厂 */
#define HTTPD_STACK     4096
#define HTTPD_MAX_URI   24         /* 基础 8 + OTA/页面 2 + 轮播 6 + 日历 4 + 待办 2 + 余量 */

/* ---------------- 请求小工具 ---------------- */

static void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') { *w++ = ' '; r++; }
        else if (*r == '%' && r[1] && r[2]) {
            int hi = 0, lo = 0;
            char h = r[1], l = r[2];
            hi = (h <= '9') ? h - '0' : ((h | 0x20) - 'a' + 10);
            lo = (l <= '9') ? l - '0' : ((l | 0x20) - 'a' + 10);
            *w++ = (char)((hi << 4) | lo);
            r += 3;
        } else { *w++ = *r++; }
    }
    *w = '\0';
}

/* 从 key=value&... 串里取值，命中返回 1，值经 URL 解码写入 out。 */
static int kv_get(const char *qs, const char *key, char *out, size_t outlen)
{
    size_t klen = strlen(key);
    const char *p = qs;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = seg - klen - 1;
            if (vlen >= outlen) vlen = outlen - 1;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = '\0';
            url_decode(out);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

/* 读请求体到栈缓冲（限长），成功返回长度。
   R1.0.8：httpd_req_recv 单次可能短读（TCP 分段），必须循环收满；
   超出缓冲的部分读走丢弃，避免残体污染 keep-alive 的下一个请求。 */
static int read_body(httpd_req_t *req, char *buf, size_t n)
{
    size_t len = req->content_len;
    if (len == 0) return 0;
    size_t cap = (len > n - 1) ? n - 1 : len;
    size_t got = 0;
    while (got < len) {
        if (got < cap) {
            int k = httpd_req_recv(req, buf + got, cap - got);
            if (k <= 0) break;
            got += (size_t)k;
        } else {
            char sink[64];                    /* 溢出部分丢弃 */
            int k = httpd_req_recv(req, sink, sizeof(sink));
            if (k <= 0) break;
            got += (size_t)k;
        }
    }
    if (got == 0) return 0;
    size_t kept = (got < cap) ? got : cap;
    buf[kept] = '\0';
    return (int)kept;
}

/* ---------------- 控制页 ---------------- */

static esp_err_t page_handler(httpd_req_t *req)
{
    power_activity();

    if (captive_probe_redirect(req)) return ESP_OK;

    if (ota_web_has_page()) return ota_web_stream_page(req);

    /* 页面随固件一起更新，浏览器缓存会让人误判「没生效」——必须禁缓存。
       热更页那条路径在 ota_web_stream_page() 里同样设了 no-store。 */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html, index_html_len);
}

/* ---------------- /api/info ---------------- */

static esp_err_t info_handler(httpd_req_t *req)
{
    power_activity();
    const moink_settings_t *s = settings_get();
    char buf[640];
    ota_web_status_t ost;
    ota_web_status(&ost);

    /* ver = 统一版本号 = 当前生效页面版本（热更页优先，否则等于固件编译号）。
       fw  = 固件编译进去的号：仅供排障，以及判断「热更控制页是否落后于固件」
             （ver < fw 时页面给黄色警示），界面只显示 ver。
       api 仍是帧格式契约号，与发布版本号解耦，页面状态栏另起一行显示。
       R1.5.4 新增三组排障字段：
         heap_max / fb —— 最大连续空闲块与实际分到的帧缓冲；帧缓冲按画像分配后
                          这两个数就是「离崩溃悬崖还有多远」的唯一可见度量
                          （R1.5.3 的 120000 字节只差 1216 字节，直接 OOM 循环）。
         ota_run/ota_state + ota_other/ota_other_state —— 把「上次升级其实被
                          回滚了」变成页面能显示的证据：ABORTED/INVALID 的槽
                          bootloader 永远不会选它，之前只能靠版本号猜。
       a1_mode 取**驱动实际在用的档**，不是 NVS 里的意图：开机若拿不到 800x600
       那块缓冲，frame_init 会把它退回默认档，页面必须按退回后的几何出图。 */
    int n = snprintf(buf, sizeof(buf),
        "{\"ver\":\"%s\",\"fw\":\"%s\",\"api\":%d,"
        "\"panel\":\"%s\",\"a1_mode\":%u,\"heap\":%lu,\"heap_max\":%lu,\"fb\":%lu,"
        "\"bat_mv\":%d,"
        "\"sleep_s\":%lu,\"wake_s\":%lu,\"next_wake_s\":%lu,\"clients\":%d,"
        "\"ssid\":\"%s\",\"sta_ip\":\"%s\",\"uptime\":%lld,\"store_slots\":%d,"
        "\"ota_run\":\"%s\",\"ota_state\":\"%s\",\"ota_other\":\"%s\",\"ota_other_state\":\"%s\"}",
        ota_web_page_version(), MOINK_VERSION, MOINK_API_VERSION,
        epd_panel_name((epd_panel_t)s->panel), (unsigned)epd_get_a1_mode(),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned long)frame_buf_len(), power_battery_mv(),
        (unsigned long)s->sleep_s, (unsigned long)s->wake_s,
        (unsigned long)power_next_wake_s(),
        netif_ap_client_count(), netif_ap_ssid(),
        netif_sta_ip() ? netif_sta_ip() : "",
        (long long)(esp_timer_get_time() / 1000000), store_slots(),
        ost.run, ost.state, ost.other, ost.other_state);
    if (n < 0 || n >= (int)sizeof(buf)) buf[sizeof(buf) - 1] = '\0';

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, strlen(buf));
}

/* ---------------- /api/settings ---------------- */

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    power_activity();
    const moink_settings_t *s = settings_get();
    char buf[512];

    snprintf(buf, sizeof(buf),
        "{\"panel\":%u,\"hflip\":%u,\"a1_mode\":%u,\"wifi_pwr\":%u,"
        "\"sleep_s\":%lu,\"wake_s\":%lu,\"ssid\":\"%s\",\"pass_set\":%s,"
        "\"sta_enable\":%u,\"sta_ssid\":\"%s\",\"sta_pass_set\":%s,\"sta_ip\":\"%s\"}",
        s->panel, s->hflip, s->a1_mode, s->wifi_pwr,
        (unsigned long)s->sleep_s, (unsigned long)s->wake_s,
        s->ap_ssid, s->ap_pass[0] ? "true" : "false",
        (unsigned)s->sta_enable, s->sta_ssid,
        s->sta_pass[0] ? "true" : "false",
        netif_sta_ip() ? netif_sta_ip() : "");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, strlen(buf));
}

/* FB-014②：AP 配置延迟应用 —— 先把 200 应答发回给页面，300ms 后再把新配置
   应用到热点。改名 / 改密码会立刻踢掉手机连接，若「先应用后应答」，页面大概率
   收不到 200，成败只能靠回读猜；应答先行后，保存判定走正常成功路径。
   esp_timer 一次性回调，绝不在 httpd 任务栈里等。
   R1.3.0：STA 配网变更也复用同一延后窗口（netif_sta_apply）。 */
static esp_timer_handle_t s_ap_timer;

static void ap_apply_timer_cb(void *arg)
{
    (void)arg;
    netif_ap_apply();
    netif_sta_apply();
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    power_activity();
    char body[512];
    read_body(req, body, sizeof(body));

    char v[SETT_PASS_MAX];
    bool ap_changed = false;
    bool panel_changed = false, hflip_changed = false;

    if (kv_get(body, "panel", v, sizeof(v))) {
        settings_set_panel((uint8_t)atoi(v));
        panel_changed = true;
    }
    if (kv_get(body, "hflip", v, sizeof(v))) {
        settings_set_hflip((uint8_t)atoi(v));
        hflip_changed = true;
    }
    if (kv_get(body, "a1_mode", v, sizeof(v))) {
        /* R1.5.4：帧缓冲现在按画像大小分配。对照档要 120000 字节连续内存，
           开机时拿不到就只会有 105984（见 frame_init 的退回逻辑），这时必须
           连驱动带设置一起拒收这一档 —— 否则页面按 800x600 传图、固件按几何
           400 拒收，用户只看到「存了但没生效」。选回默认档或重启后自会重试。 */
        uint8_t want = (uint8_t)atoi(v);
        if (want == EPD_A1_MODE_NATIVE800 && frame_buf_len() < (EPD_A1N_W / 4) * EPD_A1N_H)
            want = EPD_A1_MODE_SEQ552;
        settings_set_a1_mode(want);
        /* 模式会改 A1 的画像几何（768x552 顺序 ↔ 800x600 原生对照），立即生效。 */
        epd_set_a1_mode(settings_get()->a1_mode);
    }
    if (kv_get(body, "wifi_pwr", v, sizeof(v))) {
        settings_set_wifi_pwr((uint8_t)atoi(v));
        netif_ap_apply_tx_power();
    }
    if (kv_get(body, "sleep_s", v, sizeof(v))) {
        settings_set_sleep((uint32_t)atoi(v));
    }
    if (kv_get(body, "wake_s", v, sizeof(v))) {
        settings_set_wake((uint32_t)atoi(v));
        /* 起点跟着新值走：热点节奏的基准原本只在开机时刷，若本次开机时 wake_s=0，
           刚把间隔改成非 0 就会拿陈旧基准算出「已到期」，被 MIN_ARM_S 兜底成 30 秒。
           power_mark_hotspot_wake() 自己判 wake_s>0，关成 0 时是空操作。 */
        power_mark_hotspot_wake();
    }

    /* 热点凭据（FB-014①）：单字段可改 —— pass 键缺省 = 保持当前密码，
       pass 空串 = 清除密码（热点转开放）；ssid 键缺省 = 保持当前热点名。
       页面据此区分「留空不动」与「勾选清除」。 */
    char ssid[SETT_SSID_MAX], pass[SETT_PASS_MAX];
    bool have_ssid = kv_get(body, "ssid", ssid, sizeof(ssid));
    bool have_pass = kv_get(body, "pass", pass, sizeof(pass));
    if (have_ssid || have_pass) {
        settings_set_ap(have_ssid ? ssid : NULL, have_pass ? pass : NULL);
        ap_changed = true;
    }

    /* STA 配网（R1.3.0）：sta_enable / sta_ssid / sta_pass 三键独立可选，
       缺省 = 保持当前。密码不回显（页面用 sta_pass_set 标志位表示「是否已设」）。 */
    bool sta_changed = false;
    bool have_sta_enable = kv_get(body, "sta_enable", v, sizeof(v));
    char sta_ssid[SETT_SSID_MAX], sta_pass[SETT_PASS_MAX];
    bool have_sta_ssid = kv_get(body, "sta_ssid", sta_ssid, sizeof(sta_ssid));
    bool have_sta_pass = kv_get(body, "sta_pass", sta_pass, sizeof(sta_pass));
    if (have_sta_enable || have_sta_ssid || have_sta_pass) {
        uint8_t en = have_sta_enable ? (uint8_t)atoi(v) : settings_get()->sta_enable;
        settings_set_sta(en,
                         have_sta_ssid ? sta_ssid : NULL,
                         have_sta_pass ? sta_pass : NULL);
        sta_changed = true;
    }

    /* 应用到运行时。 */
    if (panel_changed) epd_set_panel((epd_panel_t)settings_get()->panel);
    if (hflip_changed) epd_set_hflip(settings_get()->hflip != 0);

    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = httpd_resp_sendstr(req, "OK");

    /* 应答已发出，再延迟应用 AP/STA 配置（FB-014②）。 */
    if (ap_changed || sta_changed) {
        if (!s_ap_timer) {
            const esp_timer_create_args_t targs = {
                .callback = &ap_apply_timer_cb,
                .arg = NULL,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "ap_apply",
            };
            esp_timer_create(&targs, &s_ap_timer);
        } else {
            esp_timer_stop(s_ap_timer);     /* 连续保存：重置 300ms 窗口 */
        }
        esp_timer_start_once(s_ap_timer, 300 * 1000);
    }
    return err;
}

/* ---------------- /api/clear / /api/factory ---------------- */

static esp_err_t clear_handler(httpd_req_t *req)
{
    power_activity();
    int cycles = 2;
    char v[16];
    /* 查询串在 req->uri 里，得从 '?' 之后开始匹配（kv_get 认段首 key=）。 */
    const char *q = strchr(req->uri, '?');
    if (q && kv_get(q + 1, "cycles", v, sizeof(v))) cycles = atoi(v);
    if (cycles < 1) cycles = 1;
    if (cycles > 4) cycles = 4;

    ESP_LOGI(TAG, "ghost clear x%d", cycles);
    int r = frame_clear_cycles(cycles);   /* R1.0.8：持帧锁，防与传图并发互踩 */
    if (r != 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "clear failed");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t factory_handler(httpd_req_t *req)
{
    settings_factory_reset();
    /* 应答先发：wipe 要擦整个 store 分区（约 1MB，秒级），先让页面拿到 200。 */
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    carousel_factory_wipe();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ---------------- CORS 预检（离线客户端走 file://） ---------------- */

static esp_err_t options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---------------- HTTP 服务器 ---------------- */

static httpd_handle_t start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* 默认是精确匹配，OPTIONS * 通配路由永远不触发 → 跨源预检 404 →
       离线/本地页面的上传被浏览器拦截。启用通配匹配后预检正常。 */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = HTTPD_STACK;
    cfg.max_uri_handlers = HTTPD_MAX_URI;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return NULL;
    }

    httpd_uri_t routes[] = {
        { .uri = "/",               .method = HTTP_GET,    .handler = page_handler,         .user_ctx = NULL },
        { .uri = "/api/info",       .method = HTTP_GET,    .handler = info_handler,         .user_ctx = NULL },
        { .uri = "/api/settings",   .method = HTTP_GET,    .handler = settings_get_handler, .user_ctx = NULL },
        { .uri = "/api/settings",   .method = HTTP_POST,   .handler = settings_post_handler,.user_ctx = NULL },
        { .uri = "/api/frame",      .method = HTTP_POST,   .handler = frame_upload_handler, .user_ctx = NULL },
        { .uri = "/api/clear",      .method = HTTP_POST,   .handler = clear_handler,        .user_ctx = NULL },
        { .uri = "/api/factory",    .method = HTTP_POST,   .handler = factory_handler,      .user_ctx = NULL },
        { .uri = "*",               .method = HTTP_OPTIONS,.handler = options_handler,      .user_ctx = NULL },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "register %s %d failed", routes[i].uri, routes[i].method);
        }
    }

    ota_web_register(server);
    carousel_register(server);
    calendar_register(server);
    todo_register(server);
    return server;
}

/* ---------------- 任务 ---------------- */

static void frame_task(void *arg)
{
    (void)arg;
    for (;;) {
        frame_wait();
        if (frame_display_pending() == 0) {   /* 轮播排队槽位优先，否则刷新缓冲 */
            power_activity();
        }
    }
}

/* R1.5.7：换完图后不再把整段等人窗口烧完。boot 分支只在「开机那一刻」判到期，而
   一拍常常要再等十几秒才成熟（idle_monitor_task 要等 netif + httpd 起来才跑到），
   于是实际换图发生在这里 —— 不短路就得再睡 sleep_s 才走：R1.5.6 实测 300 s 周期里
   醒着 257 s（占空比 85%），这就是「换图节奏对了但电还是要抽干」的那一半问题。
   静默门槛：传输是一段一段 call power_activity() 的，静默够久才说明这条连接真停了。 */
#define SWAP_QUIET_S 15

static void idle_monitor_task(void *arg)
{
    (void)arg;
    bool swapped = false;   /* 本次开机已经换过一张：只等静默，不重复消费 */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        calendar_awake_tick();   /* 醒着也要换日：不依赖"睡前有没有武装定时器" */
        if (carousel_awake_tick()) swapped = true;   /* 醒着也要换图：见 R1.5.6 */
        /* 回睡条件和 boot 分支同一条（热点没到期才睡），再加两道 boot 分支不需要、
           这里却必需的护栏：① 本次确为定时自醒 —— 按键 / 上电开机是要等人传图的，
           窗口必须留满；② 没人连着热点且请求静默够久 —— 否则会把 OTA 写入或传图
           拦腰截断（截断不brick，但白等一次）。 */
        if (swapped && power_auto_wake() && power_hotspot_in_s() != 1 &&
            netif_ap_client_count() == 0 && power_idle_s() >= SWAP_QUIET_S) {
            power_enter_deep_sleep();   /* 不返回 */
        }
        if (power_should_sleep()) {
            power_enter_deep_sleep();   /* 不返回 */
        }
    }
}

static void button_task(void *arg)
{
    (void)arg;
    int held = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        bool down = (gpio_get_level((gpio_num_t)EPD_PIN_WAKE_BTN) == 0);
        if (down) {
            held++;
            if (held == HOLD_RESET_S * 20) {
                ESP_LOGW(TAG, "wake button held, keep holding %ds to factory reset", HOLD_RESET_S);
            }
            if (held >= HOLD_RESET_S * 20 + 5) {
                ESP_LOGW(TAG, "long press: factory reset + reboot");
                settings_factory_reset();
                carousel_factory_wipe();   /* NVS 之外，轮播帧也要清 */
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            held = 0;
        }
    }
}

/* ---------------- 主入口 ---------------- */

/* R1.5.7②：定时自醒「这一拍还没到期」时的直接回睡门槛 —— 下次到期还要等够 1 分钟
   才值得睡（一次醒来到回睡本身要几秒，门槛太低容易掉进 1~2 秒一次的醒睡循环）。 */
#define BOOT_IDLE_S 60

void app_main(void)
{
    ESP_LOGI(TAG, "=== MoInk %s | 4-color | ap + unified upgrade ===", MOINK_VERSION);

    /* 唤醒原因：GPIO = 按键，TIMER = 定时自动唤醒。 */
    uint32_t wake = power_wakeup_causes();
    bool auto_wake = (wake & (1u << ESP_SLEEP_WAKEUP_TIMER)) != 0;
    power_set_auto_wake(auto_wake);
    if (wake & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        ESP_LOGI(TAG, "woken by GPIO%d button", EPD_PIN_WAKE_BTN);
    } else if (auto_wake) {
        ESP_LOGI(TAG, "woken by RTC timer (auto wake)");
    } else if (wake) {
        ESP_LOGI(TAG, "wakeup bitmap 0x%lx", (unsigned long)wake);
    }

    /* NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    settings_init();
    power_init();
    store_init();       /* 轮播存储底座（FB-017 第一部分）：只探测，不写入 */
    carousel_init();    /* 加载轮播配置 / 游标（从 NVS，读不到即默认关闭） */
    calendar_init();    /* 日历开关 + 换日时刻；墙上日期由页面 POST /api/time 推来 */
    todo_init();        /* 待办文档原文（设备只做持久层，版式与绘制在页面） */

    if (epd_init() != 0) {
        ESP_LOGE(TAG, "EPD init failed");
    }
    epd_set_panel((epd_panel_t)settings_get()->panel);
    epd_set_a1_mode(settings_get()->a1_mode);
    epd_set_hflip(settings_get()->hflip != 0);

    frame_init();

    /* R1.5.4：确认回滚窗口在 frame_init 之后立刻起算，不再等到 main 末尾。
       下面这两条路都可能「不返回」：SNTP 最坏阻塞 34 秒 + 日历刷屏 15~25 秒，
       定时自醒路径更是刷完就 power_enter_deep_sleep()。旧顺序（原来在这里才调）
       让新镜像经常来不及自证 —— 一旦没确认，下次开机它就被标 ABORTED 永久回滚，
       而页面上只看得到「版本还是旧的」，等于静默升级失败（R1.5.3 踩过）。
       走到这一行 = 镜像已经能跑 app_main，回滚要防的是「根本起不来」，仍然有效。 */
    ota_web_confirm();

    /* 定时自醒 + 该校时（距上次校时 > 12 小时，或掉电后 RTC 回了 1970）：先临时起一次
       WiFi 走 SNTP，再决定屏幕上该挂哪天。顺序不能反 —— RTC 走的是内部 RC 慢时钟（板上
       没有 32.768 kHz 晶振），不校时「它的 00:01」会一天天前移，最后在大白天挂出明天；
       而先按漂掉的时钟刷了再校正，就得白刷第二遍屏。
       这里只起 WiFi：不起 HTTP、不进热点等待窗口，校完照旧立刻回睡（netif_ap_init 幂等，
       后面正常启动路径再调一次不会重入）。 */
    if (auto_wake && calendar_sntp_needed()) {
        netif_ap_init();
        calendar_sntp_run();            /* 阻塞但有超时，最坏约 34 秒 */
    }

    /* 屏和帧缓冲就绪后补日历：日历开着 + 时钟可用 → 刷当天（异步，方案 B）。
       只有「从深睡醒来」（定时自醒 / 按键）才允许拿 NVS 的 cal_day 说「今天刷过了」：
       那时屏上挂着的就是上次刷的东西。掉电 / 复位 / OTA 重启之后设备并不知道屏上是
       什么（墨水屏双稳态会留着旧图，比如你手动推上去的待办），这时必须强制回屏一次，
       否则同一天内断电重上电，日历就再也回不来了（R1.5.3①，实机踩到）。 */
    /* 「屏上挂着什么未知」= 本次复位不是从深睡醒的。这里不能写 wake == 0：
       esp_sleep_get_wakeup_causes() 对上电 / 硬复位返回 BIT(ESP_SLEEP_WAKEUP_UNDEFINED)，
       而 UNDEFINED 是枚举 0 → 位图恒为 1，永远不等于 0（sleep_modes.c:2580）。旧写法让
       这道 force 恒假，只要 NVS 的 cal_day 记着今天，掉电重上电就永远不回屏（R1.5.3①
       实际从未生效，2026-10-06 实机定案）。 */
    calendar_boot_catchup((wake & (1u << ESP_SLEEP_WAKEUP_UNDEFINED)) != 0);

    /* 定时唤醒路径（R1.5.0）：各节奏独立到期，只做到期的那件事，默认不起 WiFi。
       换日时刻到期 → 先把日历改到当天；轮播换图到期 → 换一张上屏；
       若热点节奏同时未到期，立即回深睡。其余情形（热点到期 / 离线活失败）
       落回正常启动，起热点等人传图。
       注：日历和轮播互斥（R1.5.2，同一块屏只能有一个主人），不再存在「换日被换图盖掉」。 */
    if (auto_wake) {
        bool ticked = false;
        bool failed = false;    /* 到期但没做成：这条要留窗口给人处理，见下 */
        if (calendar_next_in_s() == 1) {            /* 1 = 换日时刻已到（见 calendar.h） */
            if (calendar_boot_tick() == 0) {
                ticked = true;
                ESP_LOGI(TAG, "calendar day changed, panel updated");
            } else {
                failed = true;
                ESP_LOGW(TAG, "calendar update failed, normal boot");
            }
        } else if (carousel_next_in_s() == 1) {     /* 1 = 换图节奏已到期（见 carousel.h） */
            if (carousel_boot_tick() == 0) {
                ticked = true;
                ESP_LOGI(TAG, "carousel tick displayed");
            } else {
                failed = true;
                ESP_LOGW(TAG, "carousel advance failed, normal boot");
            }
        }
        if (ticked && power_hotspot_in_s() != 1) {
            power_enter_deep_sleep();               /* 不返回 */
        }
        /* R1.5.7②：醒得太早（这一拍根本还没到期）也不原地等 —— 立刻回睡，按原来的
           下次时刻重新武装。实测 300 s 周期里醒着 185~205 s（占空比 65%），这一百多秒
           什么都没干，只是在等下一拍成熟，纯烧电。
           四道门槛，缺一不可：
             ① !failed —— 「到期但做失败」必须落回正常启动，起热点让人来救（槽位全坏
                时轮播会照旧消费掉一拍，next 一下跳到 int_s，不挡住就永远静默失败）；
             ② 热点没同时到期（与上面 ticked 分支同一条）；
             ③ frame_pending_display() == 0 —— 日历补刷走的是异步排队
                （calendar_show(1) → frame_render_queue），而 frame_task 要到本函数末尾
                才创建；带着没上屏的一帧睡下去 = 这一天的日历再也不会重试（s_shown_day
                已经记账了），这是日历功能最容易被这个优化修坏的一条路；
             ④ 还要等 > BOOT_IDLE_S —— 一两秒的剩余不值得一次醒睡。
           只在外层 auto_wake（TIMER 自醒）里判：按键 / 上电开机是要等人传图的。
           必须在 power_mark_hotspot_wake() 之前返回：那条会把热点节奏起点挪到此刻，
           每次早醒都重置一次的话，热点就永远不会到期。 */
        if (!ticked && !failed && power_hotspot_in_s() != 1 &&
            !frame_pending_display() && power_next_wake_s() > BOOT_IDLE_S) {
            /* ERROR 级是故意的：本工程 CONFIG_LOG_MAXIMUM_LEVEL=1，INFO/WARN 全被编译掉，
               这一行是「一次间隔里早醒了几跳」唯一到得了串口的证据。 */
            ESP_LOGE(TAG, "woke early, nothing due for %lus, back to sleep",
                     (unsigned long)power_next_wake_s());
            power_enter_deep_sleep();               /* 不返回 */
        }
        /* 剩下的情形：剩余不足 1 分钟 / 热点同时到期 / 有画面还排着没上屏 ——
           落回正常启动，起热点等人传图，空闲窗口过后按校正过的时间重新武装。 */
    }
    power_mark_hotspot_wake();   /* 走到这里 = 本次开机要起热点，节奏起点挪到此刻 */

    netif_ap_init();
    captive_dns_start();

    if (start_server() == NULL) {
        ESP_LOGE(TAG, "HTTP server failed to start");
    }

    /* 掉电 / 换电池后不必等人开页面：后台起一次性任务连 SNTP，把日期自己追平
       （日历开着 + STA 已配时才起，见 calendar_sntp_needed）。 */
    calendar_sntp_autostart();

    xTaskCreate(frame_task, "frame", 4096, NULL, 5, NULL);
    xTaskCreate(idle_monitor_task, "idle_mon", 3072, NULL, 4, NULL);
    xTaskCreate(button_task, "button", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "ready: %s @ http://192.168.4.1%s%s (heap %lu)",
             netif_ap_ssid(),
             netif_sta_ip() ? " | lan http://" : "",
             netif_sta_ip() ? netif_sta_ip() : "",
             (unsigned long)esp_get_free_heap_size());
}
