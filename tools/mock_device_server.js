// MoInk mock 设备服务器 —— 页面浏览器实测用（不碰真硬件）。仓库内副本，字段随 docs/CONTRACT.md 走。
// 静态托管 moink/page/ + 按契约桩出 /api/*，状态存内存，进程退出即干净。
//
// 运行（node 不在 PATH，必须绝对路径）：
//   PORT=8123 /Users/tonyding/.nvm/versions/node/v22.19.0/bin/node tools/mock_device_server.js &
//   MOCK_LEGACY=1 …   只保留 R1.4.0 之前的路由 → carousel/calendar/time 全回 404（测「旧固件降级」文案）
//   MOCK_HTTP500=1 …  所有 /api/* 回 500（测 err 药丸；停掉进程是另一种测法）
//   MOCK_OLD=1 …      日历应答去掉 R1.5.2 新字段（测旧固件降级文案），/api/info 也去掉 R1.5.4 新字段
//   MOCK_FB=0 …       帧缓冲「未分配」；MOCK_OTA_STATE / MOCK_OTA_OTHER_STATE=VALID|PENDING|NEW|ABORTED|INVALID
//                     测页面升级状态药丸的三条分支（默认 ota_0/VALID + ota_1/UNDEFINED）
// 调试：GET /api/_mock/log 读请求日志；GET /api/_mock/state 读 mock 内部状态；
//       GET /api/_mock/sync?age=秒 假装「N 小时前自动校过时」（测页面 R1.5.2 校时回显的另一分支）。
//
// 已知简化：时钟按真实秒前进；next_in_s 是粗算（够页面转述 0/1/>1 三态）；帧载荷只存字节、不校验 CRC。
// 时间基准：mono = 进程启动至今的秒数（对应固件 power_mono_s()，掉电归 0），sntp_at 同基准。
"use strict";
const http = require("http");
const fs = require("fs");
const path = require("path");

const PORT = +(process.env.PORT || 8123);
const ROOT = process.env.MOINK_PAGE_ROOT || "/Users/tonyding/Desktop/NAS/moink/page";
const LEGACY = process.env.MOCK_LEGACY === "1";
const OLD = process.env.MOCK_OLD === "1";   /* 假装 R1.5.1 固件：日历应答不含 tz_min/sntp_at/mono/car_off */
const HTTP500 = process.env.MOCK_HTTP500 === "1";
const LEN = 105984, SLOTS = 8;
const BOOT = Math.floor(Date.now() / 1000);
/* MOCK_MONO_OFF：把单调秒基准往前挪（纯测试旋钮，用来当场验证「N 小时前校过时」的长文案，
   不必真的把 mock 挂着等一小时）。 */
const MONO_OFF = +(process.env.MOCK_MONO_OFF || 0);
/* R1.5.4 的 /api/info 新字段。初值取环境变量，也可运行期用 GET /api/_mock/ota 改写，
   这样一轮浏览器实测能把「已确认 / 正在确认 / 被静默回滚 / 帧缓冲未分配 / 旧固件无字段」
   五条分支全走一遍，不必为每个场景重启一次 mock。 */
const num = function (name, dflt) { return process.env[name] !== undefined ? +process.env[name] : dflt; };
const ota = {
  fb: num("MOCK_FB", 105984), heap_max: num("MOCK_HEAP_MAX", 118768),
  run: process.env.MOCK_OTA_RUN || "ota_0", state: process.env.MOCK_OTA_STATE || "VALID",
  other: process.env.MOCK_OTA_OTHER || "ota_1", other_state: process.env.MOCK_OTA_OTHER_STATE || "UNDEFINED"
};
let OTA_HIDE = false;   /* true = 整个字段组不发，模拟 R1.5.3 及更早固件 */

const MIME = { ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".css": "text/css",
  ".png": "image/png", ".jpg": "image/jpeg", ".ico": "image/x-icon", ".json": "application/json" };

const st = {
  sleep_s: 180, wake_s: 0, car_on: 0, car_mode: 0, car_int_s: 0, car_anchor: Math.floor(Date.now() / 1000),
  cal_on: 0, cal_tod_s: 60, cal_shown_day: -1, cal_lang: 0, cal_style: 0,
  cal_tz_s: 0, sntp_at: 0,                  /* R1.5.2：时区偏移 + 上次自动校时的单调秒（0 = 没校过） */
  t: 0, sync_at: 0,                       /* 0 = 未同步（synced=0） */
  todo: '{"items":[]}',                   /* 设备存原文，GET 逐字回给页面 */
  items: []                               /* {buf:Buffer|null} 按显示序 */
};
const log = [];
const mono = () => Math.floor(Date.now() / 1000) - BOOT + MONO_OFF;

const now = () => (st.t && st.sync_at) ? st.t + Math.floor(Date.now() / 1000 - st.sync_at) : 0;
const clock = () => { const s = now(); return s ? new Date(s * 1000) : null; };
const car_next = () => (st.car_on && st.car_int_s)
  ? (function () { const r = st.car_int_s - ((Math.floor(Date.now() / 1000) - st.car_anchor) % st.car_int_s); return r <= 1 ? 1 : r; })() : 0;
const cal_next = () => { const d = clock(); if (!st.cal_on || !d) return 0;
  const left = (86400 - (d.getUTCHours() * 3600 + d.getUTCMinutes() * 60 + d.getUTCSeconds())) + st.cal_tod_s;
  return left <= 1 ? 1 : left; };
/* 与固件 reply_state_ex 同字段；car_off 只在 /api/calendar/cfg 的成功应答里为 1。 */
const snap = (car_off) => { const d = clock(); const s = {
  synced: d ? 1 : 0, t: d ? now() : 0,
  y: d ? d.getUTCFullYear() : 0, m: d ? d.getUTCMonth() + 1 : 0, d: d ? d.getUTCDate() : 0,
  hh: d ? d.getUTCHours() : 0, mm: d ? d.getUTCMinutes() : 0, ss: d ? d.getUTCSeconds() : 0,
  wd: d ? d.getUTCDay() : 0, on: st.cal_on, tod_s: st.cal_tod_s,
  lang: st.cal_lang, style: st.cal_style,
  shown_day: st.cal_shown_day, next_in_s: cal_next(),
  tz_min: Math.round(st.cal_tz_s / 60), sntp_at: st.sntp_at, mono: mono(),
  car_off: car_off ? 1 : 0 };
  if (OLD) { delete s.tz_min; delete s.sntp_at; delete s.mono; delete s.car_off; }
  return s; };
const car_list = () => ({ on: st.car_on, mode: st.car_mode, n: st.items.length, cur: 0, slots: SLOTS,
  int_s: st.car_int_s, next_in_s: car_next(),
  items: st.items.map(function (it, i) { return it.buf
    ? { i: i, slot: i % SLOTS, ok: true, w: it.buf.length === 120000 ? 800 : 768,
        h: it.buf.length === 120000 ? 600 : 552, len: it.buf.length }
    : { i: i, slot: i % SLOTS, ok: false }; }) });

function send(res, code, body, type) {
  res.writeHead(code, { "Content-Type": type || "application/json; charset=utf-8",
    "Access-Control-Allow-Origin": "*", "Cache-Control": "no-store" });
  res.end(typeof body === "string" ? body : JSON.stringify(body));
}
const bad = (res, msg) => send(res, 400, msg, "text/plain");
function form(req) { return new Promise(function (res) { const a = [];
  req.on("data", function (c) { a.push(c); }); req.on("end", function () { res(Buffer.concat(a)); }); }); }
const kv = (buf) => { const o = {}; buf.toString("utf8").split("&").forEach(function (p) { if (!p) return;
  const i = p.indexOf("="); o[decodeURIComponent(p.slice(0, i))] = decodeURIComponent(p.slice(i + 1) || ""); }); return o; };

const ROUTES = {
  "GET /api/info": function (res) { const d = clock(); const s = {
    ver: process.env.MOCK_VER || "R1.5.4-mock", fw: process.env.MOCK_VER || "R1.5.4-mock", api: 2,
    panel: "A1", a1_mode: 1, heap: 148320, bat_mv: 4020, sleep_s: st.sleep_s, wake_s: st.wake_s,
    clients: 1, ssid: "MoInk-TEST", sta_ip: "", uptime: 42, store_slots: SLOTS,
    next_wake_s: Math.max(car_next(), d ? cal_next() : 0) };
    if (!OTA_HIDE) {
      /* 帧缓冲容量 + 最大连续空闲块 + OTA 自我确认状态（见 /api/_mock/ota） */
      s.fb = ota.fb; s.heap_max = ota.heap_max;
      s.ota_run = ota.run; s.ota_state = ota.state;
      s.ota_other = ota.other; s.ota_other_state = ota.other_state;
    }
    if (LEGACY || OLD) { ["fb", "heap_max", "ota_run", "ota_state", "ota_other", "ota_other_state"]
      .forEach(function (k) { delete s[k]; }); }   /* 假装 R1.5.3 及更早固件：字段全缺 */
    send(res, 200, s); },
  "GET /api/settings": function (res) { send(res, 200, { panel: "A1", hflip: 0, a1_mode: 1, wifi_pwr: 1,
    sleep_s: st.sleep_s, wake_s: st.wake_s, ssid: "MoInk-TEST", pass_set: 1, sta_enable: 0,
    sta_ssid: "", sta_pass_set: 0, sta_ip: "" }); },
  "POST /api/settings": async function (res, req) { const f = kv(await form(req));
    ["sleep_s", "wake_s"].forEach(function (k) { if (f[k] !== undefined) st[k] = +f[k] || 0; });
    send(res, 200, { ok: 1 }); },
  "GET /api/carousel/list": function (res) { if (LEGACY) return send(res, 404, "not found"); send(res, 200, car_list()); },
  "GET /api/carousel/frame": function (res, req, q) { if (LEGACY) return send(res, 404, "not found");
    const it = st.items[+q.i]; if (!it) return bad(res, "bad index");
    if (!it.buf) return send(res, 404, "no frame");
    send(res, 200, it.buf, "application/octet-stream"); },
  "POST /api/carousel/add": async function (res, req) { if (LEGACY) return send(res, 404, "not found");
    const b = await form(req); if (st.items.length >= SLOTS) return bad(res, "carousel full");
    st.items.push({ buf: b.length > 16 ? b.subarray(16) : null });
    send(res, 200, { n: st.items.length, slot: st.items.length - 1 }); },
  "POST /api/carousel/del": async function (res, req) { if (LEGACY) return send(res, 404, "not found");
    const i = +kv(await form(req)).i; if (!(i >= 0 && i < st.items.length)) return bad(res, "bad index");
    st.items.splice(i, 1); send(res, 200, { n: st.items.length, cur: 0 }); },
  "POST /api/carousel/advance": function (res) { if (LEGACY) return send(res, 404, "not found");
    if (!st.items.length) return bad(res, "empty");
    if (!st.items.some(function (it) { return it.buf; })) return send(res, 500, "no readable frame");
    st.car_anchor = Math.floor(Date.now() / 1000); send(res, 200, { cur: 0 }); },
  "POST /api/carousel/cfg": async function (res, req) { if (LEGACY) return send(res, 404, "not found");
    const f = kv(await form(req));
    if (f.mode !== undefined && +f.mode > 1) return bad(res, "bad mode");
    if (f.int_s !== undefined && +f.int_s !== 0 && (+f.int_s < 60 || +f.int_s > 86400)) return bad(res, "bad int_s");
    /* 互斥（R1.5.2）：轮播被打开 → 日历让出这块屏。这个副作用本身算一次变更，
       所以「轮播本身没改、只为夺回屏幕而保存」不能被 no changes 挡掉。 */
    const cal_off = (f.on !== undefined && +f.on) ? st.cal_on : 0;
    const same = (f.on === undefined || +f.on === st.car_on) && (f.mode === undefined || +f.mode === st.car_mode)
      && (f.int_s === undefined || +f.int_s === st.car_int_s) && !cal_off;
    if (same) return bad(res, "no changes");
    if (f.on !== undefined) st.car_on = +f.on ? 1 : 0;
    if (f.mode !== undefined) st.car_mode = +f.mode;
    if (f.int_s !== undefined) { st.car_int_s = +f.int_s; st.car_anchor = Math.floor(Date.now() / 1000); }
    if (cal_off) st.cal_on = 0;
    const reply = { on: st.car_on, mode: st.car_mode, int_s: st.car_int_s, next_in_s: car_next(),
      cal_off: cal_off ? 1 : 0 };
    if (OLD) delete reply.cal_off;   /* R1.5.1 的轮播应答没有这个字段 */
    send(res, 200, reply); },
  "GET /api/time": function (res) { if (LEGACY) return send(res, 404, "not found"); send(res, 200, snap()); },
  "POST /api/time": async function (res, req) { if (LEGACY) return send(res, 404, "not found");
    const f = kv(await form(req)); if (f.t === undefined) return bad(res, "missing t");
    const t = +f.t; if (!(t >= 1577836800 && t <= 4102444799)) return bad(res, "bad time");
    /* tz_min 越界时只忽略偏移、不拒绝日期（与固件同口径）。 */
    if (f.tz_min !== undefined) { const m = +f.tz_min; if (m >= -840 && m <= 840) st.cal_tz_s = m * 60; }
    st.t = t; st.sync_at = Math.floor(Date.now() / 1000); st.cal_shown_day = Math.floor(t / 86400);
    send(res, 200, snap()); },
  "POST /api/calendar/cfg": async function (res, req) { if (LEGACY) return send(res, 404, "not found");
    const f = kv(await form(req));
    if (f.tod_s !== undefined && (+f.tod_s < 0 || +f.tod_s > 86399)) return bad(res, "bad tod_s");
    const car_off = (f.on !== undefined && +f.on) ? st.car_on : 0;
    const same = (f.on === undefined || +f.on === st.cal_on) && (f.tod_s === undefined || +f.tod_s === st.cal_tod_s)
      && (f.lang === undefined || +f.lang === st.cal_lang) && (f.style === undefined || +f.style === st.cal_style)
      && !car_off;
    if (same) return bad(res, "no changes");
    if (f.on !== undefined) st.cal_on = +f.on ? 1 : 0;
    if (f.tod_s !== undefined) st.cal_tod_s = +f.tod_s;
    if (f.lang !== undefined) st.cal_lang = +f.lang ? 1 : 0;
    if (f.style !== undefined) st.cal_style = (+f.style >= 0 && +f.style <= 3) ? +f.style : 0;
    if (car_off) st.car_on = 0;
    send(res, 200, snap(car_off)); },
  "POST /api/calendar/show": function (res) { if (LEGACY) return send(res, 404, "not found");
    if (!clock()) return bad(res, "clock not set");
    st.cal_shown_day = Math.floor(now() / 86400); send(res, 200, snap()); },
  "POST /api/upload": async function (res, req) { const b = await form(req);
    const head = b.subarray(0, 1024).toString("latin1").toLowerCase();
    if (b[0] === 0xE9) return send(res, 200, { kind: "fw", ok: 1 });
    if (head.indexOf("<!doctype") >= 0 || head.indexOf("<html") >= 0) return send(res, 200, { kind: "page", ok: 1 });
    return bad(res, "unrecognized payload"); },
  "POST /api/clear": function (res) { setTimeout(function () { send(res, 200, { ok: 1 }); }, 2000); },
  "POST /api/web/clear": function (res) { send(res, 200, { ok: 1 }); },
  "POST /api/factory": function (res) { send(res, 200, { ok: 1 }); },
  "GET /api/todo": function (res) { if (LEGACY) return send(res, 404, "not found");
    send(res, 200, st.todo, "application/json; charset=utf-8"); },
  "POST /api/todo": async function (res, req) { if (LEGACY) return send(res, 404, "not found");
    /* 与固件同口径：设备只存页面写来的 JSON 原文，回读必须逐字一致。 */
    const s = (await form(req)).toString("utf8");
    if (s.length > 1536) return bad(res, "too large");
    if (!s.length || s[0] !== "{" || s[s.length - 1] !== "}" || s.indexOf('"items"') < 0) return bad(res, "bad doc");
    st.todo = s; send(res, 200, s, "application/json; charset=utf-8"); },
  "GET /api/_mock/sync": function (res, req, q) {   /* 假装校过时：age=多少秒前 / at=绝对单调秒（0 = 没校过） */
    if (q.get("at") !== null) st.sntp_at = +q.get("at") || 0;
    else st.sntp_at = q.get("age") === "0" ? 0 : Math.max(0, mono() - (+q.get("age") || 0));
    send(res, 200, { sntp_at: st.sntp_at, mono: mono() }); },
  "GET /api/_mock/ota": function (res, req, q) {   /* 改写 /api/info 的 R1.5.4 新字段，测页面五条分支 */
    if (q.get("fb") !== null) ota.fb = +q.get("fb") || 0;
    if (q.get("heap_max") !== null) ota.heap_max = +q.get("heap_max") || 0;
    ["run", "state", "other", "other_state"].forEach(function (k) { if (q.get(k) !== null) ota[k] = q.get(k); });
    OTA_HIDE = q.get("hide") === "1";
    send(res, 200, { ota: ota, hide: OTA_HIDE, fields: OTA_HIDE ? "-" : Object.keys(ota).join(",") }); },
  "GET /api/_mock/log": function (res) { send(res, 200, log.slice(-80)); },
  "GET /api/_mock/state": function (res) { send(res, 200, { st: st, clock: now(), legacy: LEGACY, http500: HTTP500 }); }
};

http.createServer(function (req, res) {
  const u = new URL(req.url, "http://x");
  const key = req.method + " " + u.pathname;
  log.push(key + (u.search ? u.search : ""));
  if (req.method === "OPTIONS") return send(res, 204, "");
  if (HTTP500 && u.pathname.indexOf("/api/") === 0 && key !== "GET /api/_mock/log")
    return send(res, 500, "mock forced failure");
  const h = ROUTES[key];
  if (h) { Promise.resolve(h(res, req, u.searchParams)).catch(function (e) { send(res, 500, "mock: " + e.message); }); return; }
  if (u.pathname.indexOf("/api/") === 0) return send(res, 404, "mock: no route " + key);
  let f = path.join(ROOT, u.pathname === "/" ? "index.html" : u.pathname);
  if (!f.startsWith(ROOT) || !fs.existsSync(f) || !fs.statSync(f).isFile()) return send(res, 404, "mock: no file");
  res.writeHead(200, { "Content-Type": MIME[path.extname(f).toLowerCase()] || "application/octet-stream",
    "Cache-Control": "no-store" });
  fs.createReadStream(f).pipe(res);
}).listen(PORT, "127.0.0.1", function () {
  const ok = fs.existsSync(path.join(ROOT, "index.html"));
  console.log(ok ? "MOCK OK port=" + PORT + " root=" + ROOT + " legacy=" + (LEGACY ? 1 : 0) + " http500=" + (HTTP500 ? 1 : 0)
                : "MOCK FAIL 找不到 " + path.join(ROOT, "index.html") + "（用 MOINK_PAGE_ROOT 指定页面目录）");
});
