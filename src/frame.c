#include "frame.h"
#include "epd_drv.h"
#include "power.h"
#include "store.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "frame";

static uint8_t *s_fb = NULL;
/* 实际分配到的容量（字节），不是编译期上限：A1 原生 800x600 那一档在 C3 上
   可能拿不到整块连续内存（见 frame_init），此时它退回 768x552。所有写 s_fb
   的路径都必须拿它当上界。 */
static size_t s_fb_len = 0;
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

/* 768x552 @2bpp —— A0 与 A1 默认档都要这么多，也是 C3 上稳拿到的尺寸。 */
#define FB_LEN_SEQ552   (EPD_A1_W / 4 * EPD_A1_H)

void frame_init(void)
{
    /* R1.5.4：按当前画像分配，不再按编译期最大帧（EPD_MAX_BUF_LEN = 800x600
       的 120000）分配。实机堆表（336 ms，崩溃前）：
         [0x3fc9de30, 0x3fcc0000) 139728 B，最大连续 118784 B
         [0x3fcc0000, ...)        116496 B，最大连续 114688 B —— 类型不同，分配器不合并
       所以 120000 这一档物理上根本拿不到：R1.5.0 只是靠 .bss 少 1296 字节
       （最大连续 = 120080）富余 80 字节侥幸活着，日历那些静态数据一涨就翻车。 */
    size_t want = epd_profile()->buf_len;
    if (want > STORE_FRAME_MAX) want = STORE_FRAME_MAX;

    s_fb = malloc(want);
    if (!s_fb && want > FB_LEN_SEQ552) {
        s_fb = malloc(FB_LEN_SEQ552);
        if (s_fb) {
            /* 对照档要不起缓冲 —— 退回默认档，宁可得罪 800x600 也不能不开机。 */
            want = FB_LEN_SEQ552;
            epd_set_a1_mode(EPD_A1_MODE_SEQ552);
            ESP_LOGE(TAG, "800x600 buffer unavailable, A1 drive mode forced to seq552");
        }
    }

    /* 锁与信号量无论如何都建：显示任务在 frame_wait() 上要它们，
       缺缓冲时靠 s_fb==NULL 在各入口拒活，不能让它拿空句柄。 */
    s_fb_mutex = xSemaphoreCreateMutex();
    s_frame_ready = xSemaphoreCreateBinary();
    if (!s_fb) {
        /* 绝不 abort()：R1.5.3 的教训是启动期 abort = 崩溃循环 = 新固件被
           bootloader 标 ABORTED 静默回滚，用户只看到「刷了没变化」。
           设备必须活着把错误报给控制页。 */
        ESP_LOGE(TAG, "frame buffer unavailable (%u B), display disabled",
                 (unsigned)want);
        return;
    }
    s_fb_len = want;
    s_fw = epd_profile()->w;
    s_fh = epd_profile()->h;
    /* ERROR 级是故意的：控制台日志只放行 ERROR/WARN（CONFIG_LOG_DEFAULT_LEVEL），
       这一行是以后每次实机排障都能读到「离悬崖还有多远」的唯一入口。 */
    ESP_LOGE(TAG, "boot stat: fb %u bytes, free heap %u, largest block %u",
             (unsigned)s_fb_len,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

size_t frame_buf_len(void)
{
    return s_fb_len;
}

void frame_wait(void)
{
    xSemaphoreTake(s_frame_ready, portMAX_DELAY);
}

/* 锁缓冲 -> 整屏刷新 -> 面板深睡 -> 解锁。成功返回 0。
   显示任务内部使用；外部触发走 frame_queue_slot()。 */
static int frame_display_now(void)
{
    if (!s_fb) return -1;
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
    /* 粗筛按**实际分配到的**容量，不按编译期上限 —— 缓冲可能比 120000 小。 */
    if (!s_fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame buffer");
        return -1;
    }
    if (req->content_len < (int)(FRAME_HDR_LEN + 1) ||
        req->content_len > (int)(FRAME_HDR_LEN + s_fb_len)) {
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
    if (!s_fb) return -1;
    if (xSemaphoreTake(s_fb_mutex, portMAX_DELAY) != pdTRUE) return -1;

    store_meta_t m;
    /* 容量交给 store：老槽里可能存着 120000 字节的 800x600 帧，当前缓冲装不下
       时必须拒读，而不是把 s_fb 越界写穿堆。 */
    int r = store_slot_read(slot, s_fb, s_fb_len, &m);
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
    if (!s_fb) {
        ESP_LOGE(TAG, "render: no frame buffer, refusing %ux%u", (unsigned)w, (unsigned)h);
        return -1;
    }
    if (len > s_fb_len) {
        ESP_LOGE(TAG, "render: %u bytes exceeds buffer %u", (unsigned)len, (unsigned)s_fb_len);
        return -1;
    }
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
