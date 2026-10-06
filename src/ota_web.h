#ifndef MOINK_OTA_WEB_H
#define MOINK_OTA_WEB_H

#include "esp_http_server.h"
#include <stddef.h>

/*
 * OTA + 控制页热更 —— R1.2.0（FB-015）起共用**同一个升级入口**：
 *
 *   POST /api/upload     统一入口。按请求体首块内容自动识别：
 *                          首字节 0xE9              -> ESP 固件镜像 -> 写另一 OTA 槽
 *                          含 <!doctype / <html     -> 控制页      -> 写 web 分区
 *                        用户无需判断本次升的是固件还是页面。
 *   POST /api/web/clear  清除已上传页面，回退到内嵌页（对应「恢复内置页面」按钮）。
 *
 * 升级行为：
 *   - 固件：写另一 OTA 槽 -> 校验 -> 强制重置设置（保留热点凭据）+ 清除页面热更
 *           标记（页面随固件一起换新）-> 切槽重启；新固件跑稳 12s 取消回滚，
 *           确认之前任何一次重启都会被 bootloader 判成 ABORTED 并静默回滚，
 *           所以睡前必须先 ota_web_confirm_flush()（见下）。
 *   - 页面：写 web 分区（带 CRC32）+ 记录页面 meta 版本；不重启，刷新浏览器即可。
 *
 * 不做向后兼容：R1.1.x 的 /api/ota、/api/web 两条路由与 ?sync_page 参数已删除。
 */

/* 注册 /api/upload 与 /api/web/clear 路由。 */
void ota_web_register(httpd_handle_t server);

/* 上电后调用：跑稳 12s 取消回滚（无回滚待确认时是无害空操作）。 */
void ota_web_confirm(void);

/*
 * 立刻确认（幂等）。深睡入口必须调用：esp_deep_sleep_start() 不返回，
 * 确认任务的那 12 秒还没走完就睡下 = 下次开机镜像被标 ABORTED 永久回滚
 * （R1.5.3 实机踩过：定时自醒刷完日历 2 秒就回睡）。
 */
void ota_web_confirm_flush(void);

/* OTA 槽状态快照（字符串化，供 /api/info 直接拼 JSON）。
   run/other 各 17 = esp_partition_t.label 的长度 + 结束符。 */
typedef struct {
    char run[17];          /* 当前运行槽，如 "ota_0" */
    char state[16];        /* 运行镜像状态：NEW/PENDING/VALID/INVALID/ABORTED/UNDEFINED */
    char other[17];        /* 另一个槽 */
    char other_state[16];  /* 另一槽状态：ABORTED = 上次升级已被回滚 */
} ota_web_status_t;

void ota_web_status(ota_web_status_t *out);

/* web 分区是否存在已上传页面。 */
bool ota_web_has_page(void);

/* 清除已上传页面，回退到内嵌页。 */
void ota_web_clear(void);

/* 当前生效版本：热更页在用时取热更页 meta 版本，否则 = MOINK_VERSION。 */
const char *ota_web_page_version(void);

/* 流式发送 web 分区里的页面（调用前先 ota_web_has_page()）。 */
esp_err_t ota_web_stream_page(httpd_req_t *req);

#endif /* MOINK_OTA_WEB_H */
