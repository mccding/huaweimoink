#ifndef MOINK_NETIF_AP_H
#define MOINK_NETIF_AP_H

#include "esp_err.h"
#include <stdbool.h>

/*
 * AP + STA 共存的网络层：
 *   - AP：默认开放热点 MoInk-XXXX，是「兜底」访问路径，逻辑与 R1.2.0 完全一致。
 *   - STA：R1.3.0 新增。用户在设置页填局域网 WiFi 名/密码并开启后，设备同时以
 *     station 身份连入路由器，通过路由器 DHCP 分配的 IP 访问控制页与全部 API。
 *
 * 未配置 STA 凭据（sta_enable=0 或 sta_ssid 为空）时，固件行为与旧版纯 SoftAP
 * 完全相同；配置并启用后走 APSTA 共存，STA 掉线会自动重连，AP 始终保留兜底。
 */

/* 初始化 netif + 事件循环 + WiFi（APSTA 模式），并按当前设置启动热点与 STA。 */
void netif_ap_init(void);

/* 重新应用 AP 设置（SSID / 密码变更后调用），幂等。 */
void netif_ap_apply(void);

/* 重新应用 STA 设置（启用开关 / SSID / 密码变更后调用），幂等。
   关闭 STA 会断开并移除 station netif；开启但未成功时内置自动重连。 */
void netif_sta_apply(void);

/* 按当前设置的功率档位调整发射功率（高 18dBm / 中 10dBm / 低 8.5dBm）。 */
void netif_ap_apply_tx_power(void);

/* 进入深睡前调用：此后 esp_wifi_stop() 抛出的 STA 断线事件不再触发自动重连。
   （R1.5.1：入睡路径自己停 WiFi → 断线回调去 esp_wifi_connect() → WiFi 驱动已停
     → ESP_ERR_WIFI_NOT_STARTED → abort() 重启，导致 STA 开启时永远睡不进去。） */
void netif_sta_suspend_begin(void);

/* STA 是否已上线（拿到 IP）。 */
bool netif_sta_up(void);

/* STA 已取得的局域网 IPv4 点分地址；未上线返回 NULL。返回静态缓冲，勿长期持有。 */
const char *netif_sta_ip(void);

/* 当前已连接 STA 数（供 /api/info 展示）。 */
int netif_ap_client_count(void);

/* 生效中的热点 SSID（默认或用户设置）。 */
const char *netif_ap_ssid(void);

#endif /* MOINK_NETIF_AP_H */
