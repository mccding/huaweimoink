#ifndef MOINK_VERSION_H
#define MOINK_VERSION_H

/*
 * 版本号（R1.2.0 起统一为一套）：
 *
 *   MOINK_VERSION      唯一版本号 —— 固件与内嵌页面共用这一个。
 *   MOINK_API_VERSION  帧格式契约号 —— 与发布版本号解耦：16 字节头 + 2bpp 载荷，
 *                      载荷几何 768x552（hdr[2] = 1）或 A1 原生 800x600（hdr[2] = 2）。
 *                      它**不对外显示为「版本」**，页面状态栏另有一行「接口」。
 *                      仅当帧头字段/偏移或载荷契约变化时才 +1；api 2 自 R1.1.0 起未变。
 *
 * 编号规则（沿用本项目既有节奏，只是不再分 fw / page 两条线）：
 *   主版本 +1 —— 破坏性变更（分区表 / NVS 布局 / 屏驱动基线 / 帧格式契约）
 *   次版本 +1 —— 固件功能批（新接口、新路由、新能力）
 *   第三位 +1 —— 修复批、纯页面变更（含页面热更）
 *
 * 运行时「当前版本」= 当前生效页面的版本：设备上存在页面热更时取热更页
 * <meta name="moink-page-version"> 的值，否则等于 MOINK_VERSION。页面可单独
 * 热更，故它恒 >= 固件编译进去的号（见 ota_web_page_version()）。
 *
 * R1.2.0 变更（2026-09-24，FB-015）：
 *   - 版本号由 fw / page 两套合并为 MOINK_VERSION 一套；删除 MOINK_FW_VERSION /
 *     MOINK_PAGE_VERSION，/api/info 的 fw / page 两个字段改为单一 ver。
 *   - 升级入口合并：POST /api/upload 按文件首块内容自动识别固件（.bin，首字节
 *     0xE9）或控制页（.html，含 <!doctype / <html），分别走 OTA 与 web 分区热更。
 *     原 /api/ota、/api/web 路由与 ?sync_page 查询参数整体删除。
 *   - 升级行为：上传固件时页面一并升级（OTA 校验通过后清除页面热更标记，不再需要
 *     ?sync_page=1）；上传控制页时只换页面，不重启。
 *   - 每次固件升级强制重置设备设置：擦除除热点名/密码以外的所有设置键，避免旧状态
 *     带病升级（见 settings_reset_for_upgrade()）。
 *   - 不保留向后兼容：页面与固件必须同批交付。api 保持 2（帧格式零变动）。
 *   - 清除过时代码：A1.1 诊断变体（V1~V8）与 EPD_PANEL_A11 画像、MOINK_EMBED_PAGE
 *     条件编译与占位页、a1_mode 旧值兼容归一、ota_web_capacity() 死代码。
 *
 * R1.3.0 变更（STA 联网）：
 *   - 网络层由纯 SoftAP 升级为 AP + STA 共存（WIFI_MODE_APSTA）。
 *   - 新增设置 sta_enable / sta_ssid / sta_pass：用户在设置页填局域网 WiFi 名/密码
 *     并启用后，设备连入路由器，经路由器 DHCP 分配 IP 即可在局域网内访问控制页。
 *   - /api/info 新增 sta_ip；/api/settings 新增 sta_enable / sta_ssid / sta_pass_set /
 *     sta_ip。AP 热点保留作兜底；STA 掉线自动重连。
 *   - api 保持 2（帧格式零变动）。分区表不变，可直接 OTA 升级。
 *
 * R1.4.0 变更（相册轮播，FB-017 第二部分）：
 *   - 轮播帧存入 store 分区（8 × 128 KB 槽位，R1.2.0 起分区表已预留）：收帧
 *     整槽轮转写入（绝不原地覆写），游标 / 帧表 / 开关走 NVS，断电不丢。
 *   - 自动唤醒（RTC 定时）且轮播开启时：开机直接把下一张帧上屏 → 回深睡，
 *     不启动网络；间隔复用现有 wake_s 设置。按键唤醒行为不变（留作管理）。
 *   - 新增路由 /api/carousel/{list,frame,add,del,advance,cfg}。
 *   - 固件升级保留轮播帧与配置（settings_reset_for_upgrade 不动 car_* 键）；
 *     恢复出厂 / 长按复位连 store 分区一起清。
 *   - api 保持 2（帧格式零变动）。分区表不变，可直接 OTA 升级。
 *
 * R1.5.0 变更（节奏可自定义 + 日历自动改日期）：
 *   - 唤醒节奏不再限于写死的几档：页面按「分钟」填（0~1440），固件按秒存。
 *     「开热点等人」（settings.wake_s）与「轮播换图」（NVS car_int_s）各自独立
 *     计时，入睡时武装两者中最近的一个到期时刻；轮播换图不再顺带启热点。
 *   - /api/carousel/{list,cfg} 增 int_s / next_in_s，/api/info 增 next_wake_s；
 *     修复「不休眠 + 开轮播 = 永不入睡、节奏一次也不触发」的死角。
 *   - 日历：页面推送年月日 → 固件 settimeofday()（RTC 走时跨深睡），每天定时
 *     唤醒渲染当天日历上屏。api 保持 2，分区表不变，可直接 OTA 升级。
 *
 * 升级判据：改的是页面还是固件由「统一入口上传的文件类型」决定，不再需要人判断；
 * 版本号推进幅度按上表取值。
 */
#define MOINK_API_VERSION   2
#define MOINK_VERSION       "R1.5.0"

#endif /* MOINK_VERSION_H */
