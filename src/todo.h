#ifndef MOINK_TODO_H
#define MOINK_TODO_H

#include "esp_http_server.h"

/*
 * 待办清单（R1.5.0，功能3）：设备侧是这份清单的**持久层**，不是渲染器。
 *
 * 为什么不做固件渲染：任意中文只有页面画得出来（固件里只有离线烘焙的字模表），
 * 待办的两套版式（A 横线条目表 / B 红色时间轴）与 2bpp 帧的合成全在浏览器完成，
 * 合成好的画面仍走既有的 POST /api/frame 上屏 —— 与传图同一条链路，帧契约不变。
 *
 * 设备存的是「页面写来的那份 JSON」原文（items / done / style / push 都在里面），
 * 所以勾选状态的真源在设备：换一台手机打开控制页，读回的是同一份清单。
 * 设备不解析文档内容，只把三道字节闸门：
 *   - 长度 1..TODO_DOC_MAX；
 *   - 必须是 JSON 对象（首 `{` 尾 `}`）且含键 "items"；
 *   - 不含 NUL（HTTP 体里出现 NUL 说明发的不是这份文档）。
 * 越界 / 结构不符 → 400，NVS 与内存态都不动。
 *
 * 上限的存在理由：nvs 分区只有 16 KB。12 项 × 20 汉字（UTF-8 约 60 B/项）
 * 加 JSON 头尾实测约 800 B，取 1536 B 已留近一倍余量；再高就该动分区表了。
 *
 * 路由（页面侧契约见 docs/CONTRACT.md §6.4）：
 *   GET  /api/todo   → 当前文档原文（设备从没被写过时回空清单）
 *   POST /api/todo   → body = 整份文档；成功后回同一份原文，页面据此回读核对
 *
 * 「待办不进轮播」：本模块与 car_* 键、与定时唤醒都没有交集（用户 2026-10-05 定）。
 * 固件升级不重置它（settings_reset_for_upgrade 是白名单擦键），恢复出厂随命名空间一起清空。
 */
#define TODO_DOC_MAX 1536

/* 从 NVS 载入文档；读不到 / 结构可疑就回落空清单。 */
void todo_init(void);

/* 注册 GET/POST /api/todo。 */
void todo_register(httpd_handle_t server);

#endif /* MOINK_TODO_H */
