/*
 * 墨印 · MoInk — 相册轮播（FB-017 第二部分，R1.4.0；换图节奏 R1.5.0）
 *
 * 见 carousel.h 的契约说明。实现要点：
 *   - 状态 = NVS 七键（car_on / car_mode / car_int_s / car_n / car_slot / car_cur /
 *     car_wseq），每次变更一次 nvs_commit 落盘；断电最坏回退到上一次已提交状态。
 *   - 分配（alloc）从 car_wseq 起找第一个不在列表里的槽，写完后 wseq 前移一格，
 *     形成「永不原地覆写」的轮转 —— 写放大 = 1，擦写均匀。
 *   - 出图端点：先 store_slot_verify（分块 CRC）再分片流式发送，静态 4KB 缓冲，
 *     不占第二份 120 KB 帧内存；httpd 任务逐请求串行执行，缓冲无并发。
 *   - 节奏时刻用 power_mono_s() 打点（跨深睡的单调秒），与热点 wake_s 互不相干。
 */
#include "carousel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "calendar.h"
#include "frame.h"
#include "power.h"
#include "store.h"

static const char *TAG = "carousel";

#define NVS_NS "moink"

#define MODE_SEQ  0
#define MODE_RAND 1

static struct {
    uint8_t on;
    uint8_t mode;
    uint8_t n;                        /* 参与轮播的张数（<= store_slots()） */
    uint8_t cur;                      /* 最近显示的显示序下标 */
    uint8_t slot[STORE_SLOT_MAX];     /* 显示序 -> 物理槽 */
    uint32_t wseq;                    /* 轮转分配游标（下一张新图从这开始找空槽） */
    uint32_t int_s;                   /* 换图间隔秒；0 = 不参与定时唤醒 */
} s_car;

/* 最近一次「消费掉一拍」的时刻（手动 advance 也算消费）；深睡保留、掉电归零。
   掉电后为 0 即等于「从本次开机起算新节奏」，正是期望语义。 */
RTC_DATA_ATTR static uint32_t s_beat_at_s;

static nvs_handle_t s_nvs;
static bool s_nvs_ok;

/* ---------- NVS 持久化 ---------- */

static void persist_cur(void)
{
    if (!s_nvs_ok) return;
    nvs_set_u8(s_nvs, "car_cur", s_car.cur);
    nvs_commit(s_nvs);
}

static void persist_list(void)
{
    if (!s_nvs_ok) return;
    nvs_set_u8(s_nvs, "car_n", s_car.n);
    nvs_set_blob(s_nvs, "car_slot", s_car.slot, s_car.n);
    nvs_set_u8(s_nvs, "car_cur", s_car.cur);
    nvs_set_u32(s_nvs, "car_wseq", s_car.wseq);
    nvs_commit(s_nvs);
}

static void persist_cfg(void)
{
    if (!s_nvs_ok) return;
    nvs_set_u8(s_nvs, "car_on", s_car.on);
    nvs_set_u8(s_nvs, "car_mode", s_car.mode);
    nvs_set_u32(s_nvs, "car_int_s", s_car.int_s);
    nvs_commit(s_nvs);
}

void carousel_init(void)
{
    memset(&s_car, 0, sizeof(s_car));

    if (nvs_open(NVS_NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed; carousel disabled");
        return;
    }
    s_nvs_ok = true;

    uint8_t u8;
    if (nvs_get_u8(s_nvs, "car_on", &u8) == ESP_OK) s_car.on = u8 ? 1 : 0;
    if (nvs_get_u8(s_nvs, "car_mode", &u8) == ESP_OK && u8 <= MODE_RAND)
        s_car.mode = u8;
    nvs_get_u32(s_nvs, "car_wseq", &s_car.wseq);
    nvs_get_u32(s_nvs, "car_int_s", &s_car.int_s);
    if (nvs_get_u8(s_nvs, "car_cur", &u8) == ESP_OK) s_car.cur = u8;

    int slots = store_slots();
    uint8_t raw[STORE_SLOT_MAX];
    size_t blen = sizeof(raw);
    if (slots > 0 && nvs_get_blob(s_nvs, "car_slot", raw, &blen) == ESP_OK &&
        blen > 0) {
        if (blen > STORE_SLOT_MAX) blen = STORE_SLOT_MAX;
        int cnt = 0;
        bool dropped = false;
        for (size_t i = 0; i < blen; i++) {
            if ((int)raw[i] >= slots) { dropped = true; continue; }
            bool dup = false;
            for (int k = 0; k < cnt; k++)
                if (raw[k] == raw[i]) { dup = true; break; }
            if (dup) { dropped = true; continue; }
            raw[cnt++] = raw[i];
        }
        memcpy(s_car.slot, raw, (size_t)cnt);
        s_car.n = (uint8_t)cnt;
        if (dropped) {
            /* 未验证完的 cur 不许回存：先钳制再压缩回写。 */
            if (s_car.n == 0) s_car.cur = 0;
            else if (s_car.cur >= s_car.n)
                s_car.cur = (uint8_t)(s_car.cur % s_car.n);
            persist_list();
        }
    }

    if (s_car.n == 0) s_car.cur = 0;
    else if (s_car.cur >= s_car.n) s_car.cur = (uint8_t)(s_car.cur % s_car.n);

    ESP_LOGI(TAG, "carousel: on=%u mode=%u n=%u/%d cur=%u wseq=%lu int=%lus next_in=%lus",
             s_car.on, s_car.mode, s_car.n, slots, s_car.cur,
             (unsigned long)s_car.wseq, (unsigned long)s_car.int_s,
             (unsigned long)carousel_next_in_s());
}

int carousel_enabled(void)
{
    return s_car.on && s_car.n > 0 && store_slots() > 0;
}

int carousel_switch_on(void)
{
    return s_car.on;
}

/* 互斥副作用：日历被打开 → 轮播让出这块屏。只关开关，帧列表 / 游标 / 间隔全留着，
   用户再开轮播时列表还在。 */
int carousel_set_off(void)
{
    if (!s_car.on) return 0;
    s_car.on = 0;
    persist_cfg();
    ESP_LOGW(TAG, "carousel turned off (日历开启，同一块屏只能有一个主人)");
    return 1;
}

esp_err_t carousel_set_int(uint32_t v)
{
    if (v != 0 && (v < CAR_INT_MIN_S || v > CAR_INT_MAX_S))
        return ESP_ERR_INVALID_ARG;
    s_car.int_s = v;
    s_beat_at_s = power_mono_s();   /* 改节奏 = 从此刻重新起算 */
    persist_cfg();
    ESP_LOGI(TAG, "carousel interval = %lus", (unsigned long)v);
    return ESP_OK;
}

uint32_t carousel_next_in_s(void)
{
    if (s_car.int_s == 0 || !carousel_enabled()) return 0;
    uint32_t el = power_mono_s() - s_beat_at_s;   /* 无符号差：掉电归零后自然为 0 */
    return (el >= s_car.int_s) ? 1 : (s_car.int_s - el);
}

/* ---------- 推进逻辑 ---------- */

/* cur 的一后继（顺序 / 随机去重）；n <= 0 时返回 -1。 */
static int next_index_from(int base)
{
    int n = s_car.n;
    if (n <= 0) return -1;
    if (n == 1) return 0;
    if (s_car.mode == MODE_RAND) {
        int j;
        do { j = (int)(esp_random() % (uint32_t)n); } while (j == base);
        return j;
    }
    return (base + 1) % n;
}

/* 从 car_wseq 起找第一个不在列表里的物理槽；填满时返回 -1。 */
static int alloc_slot(void)
{
    int slots = store_slots();
    for (int k = 0; k < slots; k++) {
        uint8_t p = (uint8_t)((s_car.wseq + (uint32_t)k) % (uint32_t)slots);
        bool used = false;
        for (int j = 0; j < s_car.n; j++) {
            if (s_car.slot[j] == p) { used = true; break; }
        }
        if (!used) return p;
    }
    return -1;
}

int carousel_boot_tick(void)
{
    if (!carousel_enabled()) return -1;

    /* 本次唤醒即消费掉一拍，无论最终成图失败：下一拍要再等一整个间隔，
       避免槽位全坏时每次醒来都重试 → 秒级唤醒循环把电池抽干。 */
    s_beat_at_s = power_mono_s();

    int base = s_car.cur;
    for (int k = 0; k < s_car.n; k++) {
        int j = next_index_from(base);
        if (j < 0) return -1;
        if (frame_show_slot(s_car.slot[j]) == 0) {
            s_car.cur = (uint8_t)j;
            persist_cur();
            ESP_LOGI(TAG, "tick: index %d slot %u displayed", j,
                     s_car.slot[j]);
            return 0;
        }
        ESP_LOGW(TAG, "slot %u unreadable, trying next", s_car.slot[j]);
        base = j;
    }
    return -1;
}

void carousel_factory_wipe(void)
{
    store_wipe_all();
    memset(&s_car, 0, sizeof(s_car));
}

/* ---------- 请求小工具（沿用 main.c 的同名口径，数值专用） ---------- */

/* 从 "k=v&..." 串取整数值（仅十进制），命中返回 1。 */
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

/* 读超长请求体到小缓冲（读走溢出部分，保护 keep-alive）。 */
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

/* ---------- /api/carousel 路由 ---------- */

/* GET /api/carousel/list -> {"on","mode","n","cur","slots","int_s","next_in_s","items":[…]} */
static esp_err_t list_handler(httpd_req_t *req)
{
    power_activity();

    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"on\":%u,\"mode\":%u,\"n\":%u,\"cur\":%u,\"slots\":%d,"
        "\"int_s\":%lu,\"next_in_s\":%lu,\"items\":[",
        s_car.on, s_car.mode, s_car.n, s_car.cur, store_slots(),
        (unsigned long)s_car.int_s, (unsigned long)carousel_next_in_s());
    for (int k = 0; k < s_car.n; k++) {
        store_meta_t m;
        if (store_slot_peek(s_car.slot[k], &m) == 0) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                "%s{\"i\":%d,\"slot\":%u,\"ok\":true,\"w\":%u,\"h\":%u,\"len\":%lu}",
                k ? "," : "", k, (unsigned)s_car.slot[k], m.w, m.h,
                (unsigned long)m.len);
        } else {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                "%s{\"i\":%d,\"slot\":%u,\"ok\":false}",
                k ? "," : "", k, (unsigned)s_car.slot[k]);
        }
    }
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* GET /api/carousel/frame?i=k -> 原始 2bpp 载荷（缩略图；先整槽 CRC 校验）。 */
static esp_err_t thumb_handler(httpd_req_t *req)
{
    power_activity();

    int i = -1;
    const char *q = strchr(req->uri, '?');
    if (!q || !kv_int(q + 1, "i", &i) || i < 0 || i >= (int)s_car.n) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
        return ESP_OK;
    }

    uint8_t slot = s_car.slot[i];
    store_meta_t m;
    if (store_slot_verify(slot, &m) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no frame");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    static uint8_t chunk[4096];   /* httpd 任务串行处理请求，静态缓冲无并发 */
    uint32_t off = 0;
    while (off < m.len) {
        uint32_t n = m.len - off;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (store_slot_read_span(slot, off, chunk, n) != 0) return ESP_FAIL;
        if (httpd_resp_send_chunk(req, (const char *)chunk, n) != ESP_OK)
            return ESP_FAIL;
        off += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* POST /api/carousel/add —— 收帧（16B 头 + 载荷）存新槽。体 = 帧格式 api 2。 */
static esp_err_t add_handler(httpd_req_t *req)
{
    power_activity();

    int cap = store_slots();
    if (cap == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no store partition");
        return ESP_OK;
    }
    if (s_car.n >= cap) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "carousel full");
        return ESP_OK;
    }

    uint16_t w, h;
    uint32_t len;
    if (frame_recv_locked(req, &w, &h, &len) != 0)
        return ESP_OK;   /* 错误响应已发出、帧锁已释放 */

    int slot = alloc_slot();
    int r = (slot < 0) ? -1
                       : store_slot_write((uint8_t)slot, frame_buf(), w, h, len);
    frame_unlock();
    if (r != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "slot write failed");
        return ESP_OK;
    }

    if (s_car.n == 0) s_car.cur = 0;
    s_car.slot[s_car.n++] = (uint8_t)slot;
    s_car.wseq = ((uint32_t)slot + 1) % (uint32_t)cap;
    persist_list();
    ESP_LOGI(TAG, "add: index %u -> slot %d (%ux%u), n=%u", s_car.n - 1,
             slot, w, h, s_car.n);

    char buf[48];
    int n = snprintf(buf, sizeof(buf), "{\"n\":%u,\"slot\":%d}",
                     (unsigned)s_car.n, slot);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* POST /api/carousel/del  i=k —— 摘除第 k 张（不擦槽）。 */
static esp_err_t del_handler(httpd_req_t *req)
{
    power_activity();

    char body[64];
    read_body(req, body, sizeof(body));
    int i = -1;
    if (!kv_int(body, "i", &i) || i < 0 || i >= (int)s_car.n) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
        return ESP_OK;
    }

    for (int k = i; k < s_car.n - 1; k++)
        s_car.slot[k] = s_car.slot[k + 1];
    s_car.n--;
    if (s_car.n == 0) s_car.cur = 0;
    else if (i < s_car.cur) s_car.cur--;
    else if (i == s_car.cur)
        s_car.cur = (uint8_t)((i + s_car.n - 1) % s_car.n);
    persist_list();

    ESP_LOGI(TAG, "del: index %d removed, n=%u cur=%u", i, s_car.n, s_car.cur);
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "{\"n\":%u,\"cur\":%u}",
                     (unsigned)s_car.n, s_car.cur);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* POST /api/carousel/advance —— 立即换下一张（排队让显示任务刷新，异步）。 */
static esp_err_t advance_handler(httpd_req_t *req)
{
    power_activity();

    if (s_car.n == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
        return ESP_OK;
    }

    int base = s_car.cur;
    for (int k = 0; k < s_car.n; k++) {
        int j = next_index_from(base);
        if (j < 0) break;
        store_meta_t m;
        if (store_slot_peek(s_car.slot[j], &m) == 0) {
            s_car.cur = (uint8_t)j;
            s_beat_at_s = power_mono_s();   /* 手动换图也算一拍：自动换图顺延整个间隔 */
            persist_cur();
            frame_queue_slot(s_car.slot[j]);
            ESP_LOGI(TAG, "advance: index %d -> slot %u queued", j,
                     s_car.slot[j]);
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "{\"cur\":%d}", j);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, buf, n);
        }
        base = j;
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "no readable frame");
    return ESP_OK;
}

/* POST /api/carousel/cfg  on=0|1&mode=0|1&int_s=秒（键独立可省；全同回 400 no changes）。
   int_s：0 = 关（轮播不再触发定时唤醒），60..86400 = 换图间隔秒数，越界回 400。 */
static esp_err_t cfg_handler(httpd_req_t *req)
{
    power_activity();

    char body[64];
    read_body(req, body, sizeof(body));
    int v, changed = 0;
    if (kv_int(body, "on", &v)) {
        int nv = v ? 1 : 0;
        if (nv != s_car.on) { s_car.on = (uint8_t)nv; changed = 1; }
    }
    if (kv_int(body, "mode", &v)) {
        if (v != MODE_SEQ && v != MODE_RAND) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mode");
            return ESP_OK;
        }
        if (v != s_car.mode) { s_car.mode = (uint8_t)v; changed = 1; }
    }
    if (kv_int(body, "int_s", &v)) {
        uint32_t want = (v < 0) ? 0 : (uint32_t)v;
        if (want != s_car.int_s) {
            if (carousel_set_int(want) != ESP_OK) {   /* 越界：状态不变 */
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad int_s");
                return ESP_OK;
            }
            changed = 1;
        }
    }
    /* 互斥（R1.5.2）：轮播开着就不让日历也开着，谁后开谁赢。这个副作用同样要算一次
       变更，否则「只为夺回屏幕而点保存」会被 400 no changes 挡掉、互斥不发生。 */
    int cal_off = 0;
    if (s_car.on) cal_off = calendar_set_off();
    if (cal_off) changed = 1;

    if (!changed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no changes");
        return ESP_OK;
    }
    persist_cfg();

    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"on\":%u,\"mode\":%u,\"int_s\":%lu,\"next_in_s\":%lu,\"cal_off\":%d}",
                     s_car.on, s_car.mode, (unsigned long)s_car.int_s,
                     (unsigned long)carousel_next_in_s(), cal_off);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

void carousel_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/carousel/list",    .method = HTTP_GET,  .handler = list_handler,    .user_ctx = NULL },
        { .uri = "/api/carousel/frame",   .method = HTTP_GET,  .handler = thumb_handler,   .user_ctx = NULL },
        { .uri = "/api/carousel/add",     .method = HTTP_POST, .handler = add_handler,     .user_ctx = NULL },
        { .uri = "/api/carousel/del",     .method = HTTP_POST, .handler = del_handler,     .user_ctx = NULL },
        { .uri = "/api/carousel/advance", .method = HTTP_POST, .handler = advance_handler, .user_ctx = NULL },
        { .uri = "/api/carousel/cfg",     .method = HTTP_POST, .handler = cfg_handler,     .user_ctx = NULL },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK)
            ESP_LOGE(TAG, "register %s failed", routes[i].uri);
    }
}
