#include "frame.h"
#include "epd_drv.h"
#include "power.h"
#include "store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "frame";

static uint8_t *s_fb = NULL;
static SemaphoreHandle_t s_fb_mutex = NULL;
static SemaphoreHandle_t s_frame_ready = NULL;

/* 排队的槽位号（>=0 有效）：HTTP 端只登记，显示任务负责真正刷新。 */
static volatile int s_pending_slot = -1;
static portMUX_TYPE s_pending_mux = portMUX_INITIALIZER_UNLOCKED;

/* 最近一次被接受的帧几何（768x552 或 A1 原生 800x600），显示时交给驱动。 */
static uint16_t s_fw = EPD_A1_W, s_fh = EPD_A1_H;

/* CRC-16/CCITT-FALSE：poly 0x1021，init 0xFFFF，无反射、无终异或。 */
static uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void frame_init(void)
{
    s_fb = malloc(EPD_MAX_BUF_LEN);
    if (!s_fb) {
        ESP_LOGE(TAG, "OOM allocating %d-byte frame buffer", EPD_MAX_BUF_LEN);
        abort();
    }
    s_fb_mutex = xSemaphoreCreateMutex();
    s_frame_ready = xSemaphoreCreateBinary();
    s_fw = epd_profile()->w;
    s_fh = epd_profile()->h;
    ESP_LOGI(TAG, "frame buffer %d bytes ready (profile %ux%u)",
             EPD_MAX_BUF_LEN, (unsigned)s_fw, (unsigned)s_fh);
}

void frame_wait(void)
{
    xSemaphoreTake(s_frame_ready, portMAX_DELAY);
}

/* 锁缓冲 -> 整屏刷新 -> 面板深睡 -> 解锁。成功返回 0。
   显示任务内部使用；外部触发走 frame_queue_slot()。 */
static int frame_display_now(void)
{
    if (xSemaphoreTake(s_fb_mutex, portMAX_DELAY) != pdTRUE) return -1;
    int r = epd_display_2bpp_wh(s_fb, s_fw, s_fh);
    xSemaphoreGive(s_fb_mutex);
    if (r == 0) epd_panel_deep_sleep();
    return r;
}

/* R1.0.8：残影清理与显示/传图共用同一 SPI 与 row 缓冲，必须持同一把锁，
   否则清理期间并发传图会命令交错、画面损坏。 */
int frame_clear_cycles(int cycles)
{
    if (xSemaphoreTake(s_fb_mutex, portMAX_DELAY) != pdTRUE) return -1;
    int r = epd_clear_cycles(cycles);
    xSemaphoreGive(s_fb_mutex);
    if (r == 0) power_activity();
    return r;
}

/* ---------- R1.4.0（FB-017 第二部分）：受控复用（轮播） ---------- */

void frame_unlock(void)
{
    xSemaphoreGive(s_fb_mutex);
}

uint8_t *frame_buf(void)
{
    return s_fb;
}

/* 收帧（头 + 载荷 + CRC）入 s_fb。成功返回 0 且 s_fb_mutex 仍被持有
   （调用方必须 frame_unlock()）；失败返回负值，锁已释放、响应已发出。 */
int frame_recv_locked(httpd_req_t *req, uint16_t *w, uint16_t *h, uint32_t *len)
{
    /* R1.1.0：先按最大合法载荷粗筛（A1 原生 800x600 = 120000），细筛在读头后
       按 hdr 里的宽高做——A1 允许 768x552 与 800x600 两种帧。 */
    if (req->content_len < (int)(FRAME_HDR_LEN + 1) ||
        req->content_len > (int)(FRAME_HDR_LEN + EPD_MAX_BUF_LEN)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size mismatch");
        return -1;
    }

    uint8_t hdr[FRAME_HDR_LEN];
    /* httpd_req_recv 单次不保证收满（TCP 分段会短读），必须循环收满 16 字节 */
    size_t got = 0;
    while (got < FRAME_HDR_LEN) {
        int n = httpd_req_recv(req, (char *)(hdr + got), FRAME_HDR_LEN - got);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad header");
            return -1;
        }
        got += (size_t)n;
    }

    if (hdr[0] != FRAME_MAGIC0 || hdr[1] != FRAME_MAGIC1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad magic");
        return -1;
    }
    if (hdr[2] != FRAME_FMT_VERSION && hdr[2] != FRAME_FMT_VERSION_NATIVE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad version");
        return -1;
    }

    uint16_t width  = (uint16_t)((hdr[4] << 8) | hdr[5]);
    uint16_t height = (uint16_t)((hdr[6] << 8) | hdr[7]);
    uint32_t plen   = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                      ((uint32_t)hdr[10] << 8) | hdr[11];
    uint16_t crc_hdr = (uint16_t)((hdr[12] << 8) | hdr[13]);

    /* 载荷长度必须与几何自洽（4 像素/字节），且被当前画像 / A1 模式接受。 */
    if ((width & 3) || plen != (uint32_t)(width / 4) * (uint32_t)height) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "len mismatch");
        return -1;
    }
    if (!epd_frame_geom_ok(width, height, plen)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported geometry");
        return -1;
    }
    if (req->content_len != (int)(FRAME_HDR_LEN + plen)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size mismatch");
        return -1;
    }

    /* 面板类型字段（hdr[3]）仅信息性；实际面板由设置决定，不随帧切换。 */

    if (xSemaphoreTake(s_fb_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return -1;
    }

    size_t off = 0;
    uint16_t crc = 0xFFFF;
    int err = 0;
    while (off < plen) {
        int n = httpd_req_recv(req, (char *)(s_fb + off), plen - off);
        if (n <= 0) { err = -1; break; }
        power_activity();   /* R1.0.8：传输中途不许空闲休眠 */
        crc = crc16_update(crc, s_fb + off, (size_t)n);
        off += (size_t)n;
    }

    if (err != 0) {
        xSemaphoreGive(s_fb_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body truncated");
        return -1;
    }
    if (crc != crc_hdr) {
        xSemaphoreGive(s_fb_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "CRC mismatch");
        return -1;
    }

    if (w) *w = width;
    if (h) *h = height;
    if (len) *len = plen;
    return 0;   /* 锁仍持有，调用方 frame_unlock() */
}

esp_err_t frame_upload_handler(httpd_req_t *req)
{
    power_activity();   /* R1.0.8：上传期间不许空闲休眠 */
    uint16_t width, height;
    uint32_t plen;
    if (frame_recv_locked(req, &width, &height, &plen) != 0)
        return ESP_OK;                     /* 错误响应已由收帧端发出 */

    s_fw = width;                          /* 显示任务据此选写入路径 */
    s_fh = height;
    frame_unlock();

    ESP_LOGI(TAG, "frame accepted: %ux%u (%lu bytes), CRC ok",
             width, height, (unsigned long)plen);
    xSemaphoreGive(s_frame_ready);         /* 唤醒显示任务 */
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

int frame_show_slot(uint8_t slot)
{
    if (xSemaphoreTake(s_fb_mutex, portMAX_DELAY) != pdTRUE) return -1;

    store_meta_t m;
    int r = store_slot_read(slot, s_fb, &m);
    if (r == 0) {
        s_fw = m.w;
        s_fh = m.h;
        r = epd_display_2bpp_wh(s_fb, s_fw, s_fh);
    }
    xSemaphoreGive(s_fb_mutex);

    if (r == 0) {
        epd_panel_deep_sleep();
        ESP_LOGI(TAG, "slot %u displayed: %ux%u", slot, m.w, m.h);
    } else {
        ESP_LOGW(TAG, "slot %u display failed", slot);
    }
    return r;
}

void frame_queue_slot(uint8_t slot)
{
    portENTER_CRITICAL(&s_pending_mux);
    s_pending_slot = slot;
    portEXIT_CRITICAL(&s_pending_mux);
    xSemaphoreGive(s_frame_ready);
}

int frame_display_pending(void)
{
    portENTER_CRITICAL(&s_pending_mux);
    int slot = s_pending_slot;
    s_pending_slot = -1;
    portEXIT_CRITICAL(&s_pending_mux);

    if (slot >= 0) return frame_show_slot((uint8_t)slot);
    return frame_display_now();
}

/* ---------- R1.5.0（功能1）：固件自绘画面（日历） ---------- */

/* 持锁画 -> （可选）当场刷新。async = 1 时只把缓冲交给显示任务，应答不被 15~25 秒挂住。 */
static int render_once(uint16_t w, uint16_t h, frame_draw_fn draw, void *user, int async)
{
    uint32_t len = (uint32_t)(w / 4) * (uint32_t)h;
    if ((w & 3) || !epd_frame_geom_ok(w, h, len)) {
        ESP_LOGW(TAG, "render: unsupported geometry %ux%u", (unsigned)w, (unsigned)h);
        return -1;
    }
    if (xSemaphoreTake(s_fb_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "render: frame buffer busy");
        return -1;
    }
    int r = draw(s_fb, user);
    if (r == 0) {
        s_fw = w;
        s_fh = h;
        if (async) {
            frame_unlock();
            xSemaphoreGive(s_frame_ready);
            return 0;
        }
        r = epd_display_2bpp_wh(s_fb, w, h);
    }
    frame_unlock();
    if (r == 0) epd_panel_deep_sleep();
    return r;
}

int frame_render_display(uint16_t w, uint16_t h, frame_draw_fn draw, void *user)
{
    return render_once(w, h, draw, user, 0);
}

int frame_render_queue(uint16_t w, uint16_t h, frame_draw_fn draw, void *user)
{
    return render_once(w, h, draw, user, 1);
}
