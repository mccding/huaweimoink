/*
 * 墨印 · MoInk — 待办清单持久层（R1.5.0，功能3）
 *
 * 契约见 todo.h。实现要点：
 *   - 文档原文进、原文出，设备端零解析：items / done / style / push 的语义全在页面。
 *   - 一份 NVS 字符串键（todo_doc）+ 一份内存镜像：GET 不碰 NVS，POST 先落盘再换镜像，
 *     落盘失败就回 500 且不换镜像 —— 页面因此永远不会「回显成功、重启就丢」。
 *   - 收 body 用堆缓冲（HTTP 任务栈只有 4 KB，塞不下 1.5 KB 文档）。
 */
#include "todo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "power.h"

static const char *TAG = "todo";

#define NVS_NS     "moink"
#define NVS_KEY    "todo_doc"
/* 空文档要和页面 todoText() 的输出逐字节一致：页面靠「提交串 == 回读串」判断是否要重发，
 * 开机回的这个骨架若差一个键，页面第一次改动就会多做一次无谓写入（无害但吵）。 */
#define TODO_EMPTY "{\"v\":1,\"style\":0,\"lang\":0,\"push\":1,\"done\":[],\"items\":[]}"

static char s_doc[TODO_DOC_MAX + 1] = TODO_EMPTY;
static nvs_handle_t s_nvs;
static bool s_nvs_ok;

void todo_init(void)
{
    if (nvs_open(NVS_NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed; /api/todo 只能写不能存");
        return;
    }
    s_nvs_ok = true;

    size_t len = sizeof(s_doc);
    if (nvs_get_str(s_nvs, NVS_KEY, s_doc, &len) == ESP_OK && s_doc[0] == '{') {
        ESP_LOGI(TAG, "doc loaded (%u B)", (unsigned)(len - 1));
    } else {
        strcpy(s_doc, TODO_EMPTY);
    }
}

/* 读完 content_len 字节进 buf；短读 / 含 NUL 返回 0。 */
static size_t read_doc(httpd_req_t *req, char *buf, size_t cap)
{
    size_t len = req->content_len;
    if (len == 0 || len > cap - 1) return 0;

    size_t got = 0;
    while (got < len) {
        int k = httpd_req_recv(req, buf + got, len - got);
        if (k <= 0) break;
        got += (size_t)k;
    }
    if (got != len) return 0;
    buf[got] = '\0';
    if (strlen(buf) != got) return 0;      /* 体里有 NUL：不是我们那份 JSON */
    return got;
}

static void reply_doc(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, s_doc, HTTPD_RESP_USE_STRLEN);
}

/* ---------------- GET /api/todo ---------------- */

static esp_err_t get_handler(httpd_req_t *req)
{
    power_activity();
    reply_doc(req);
    return ESP_OK;
}

/* ---------------- POST /api/todo  body = 整份文档 ---------------- */

static esp_err_t post_handler(httpd_req_t *req)
{
    power_activity();

    if (req->content_len > TODO_DOC_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "doc too large");
        return ESP_OK;
    }

    char *buf = malloc(TODO_DOC_MAX + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    size_t got = read_doc(req, buf, TODO_DOC_MAX + 1);

    /* 只认结构骨架，不认内容：设备不该对页面的文档有想象力。 */
    int shaped = got > 0 && buf[0] == '{' && buf[got - 1] == '}' && strstr(buf, "\"items\"");
    if (!shaped) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad doc");
        return ESP_OK;
    }

    if (!s_nvs_ok || nvs_set_str(s_nvs, NVS_KEY, buf) != ESP_OK) {
        free(buf);
        ESP_LOGE(TAG, "nvs set failed; 文档未保存");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "store failed");
        return ESP_OK;
    }
    nvs_commit(s_nvs);

    memcpy(s_doc, buf, got + 1);
    free(buf);
    ESP_LOGI(TAG, "doc stored (%u B)", (unsigned)got);
    reply_doc(req);
    return ESP_OK;
}

void todo_register(httpd_handle_t server)
{
    static const httpd_uri_t URIS[] = {
        { .uri = "/api/todo", .method = HTTP_GET,  .handler = get_handler },
        { .uri = "/api/todo", .method = HTTP_POST, .handler = post_handler },
    };
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        if (httpd_register_uri_handler(server, &URIS[i]) != ESP_OK) {
            ESP_LOGW(TAG, "register %s failed", URIS[i].uri);
        }
    }
}
