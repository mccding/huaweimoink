// smoke_page.js —— 页面冒烟测试（Node）。
// 抽取 page/index.html 主脚本的纯函数段（算法 + 打包 + 帧头），在最小 sandbox 里跑断言。
// 覆盖：CRC16 金标准、帧头 16 字节字段、packIdx 纯色打包、
//       R1.0.12 结构（Cropper 内嵌 / 文字对象 / 留白 / 双 script 闭合符）、
//       R1.2.0 统一升级入口（/api/upload）+ 单一版本号显示。
//
// 用法：node tools/smoke_page.js   （tools/ 下已有 package.json 声明 commonjs）
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const page = fs.readFileSync(path.join(__dirname, "..", "page", "index.html"), "utf8");
// R1.0.12 起页面有两个 <script> 块（第 1 块 = 内嵌 Cropper.js，第 2 块 = 主脚本）。
// \r?\n：兼容 core.autocrlf 检出为 CRLF 的工作区（2026-09-22 修正）。
const m = page.match(/<script>\r?\n"use strict";\r?\n([\s\S]*?)\r?\n<\/script>/);
if (!m) { console.error("no main <script> found"); process.exit(1); }
const js = m[1];

// 只取 DOM 代码之前的纯函数段：截止到 `var $ = function` 之前。
const cut = js.indexOf("var $ = function");
if (cut < 0) { console.error("pure-section marker not found"); process.exit(1); }
const pure = js.slice(0, cut);

const sandbox = { window: {}, console: console, Uint8Array: Uint8Array,
                  Float64Array: Float64Array, Int32Array: Int32Array,
                  Math: Math, JSON: JSON, Infinity: Infinity, NaN: NaN,
                  Date: Date };
sandbox.globalThis = sandbox;
vm.createContext(sandbox);

let failed = 0;
function ok(name, cond) {
  console.log((cond ? "PASS" : "FAIL") + "  " + name);
  if (!cond) failed++;
}
function eq(name, got, want) { ok(name + " (got " + got + " want " + want + ")", got === want); }

// 静态防线：脚本串里若出现字面量 </script>，浏览器会提前终结脚本块。
// R1.0.12 起：合法闭合符数量随功能块增加而调整（R1.1.0 = 3），详见下方断言。
// R1.1.0 起 = 3 个合法闭合符（内嵌 Cropper 块 + 页脚版本填充块 + 主脚本块），多一个都不行。
eq("script closer count (cropper + footer + main = exactly 3)", (page.match(/<\/script>/g) || []).length, 3);
ok("offline export keeps </script> escaped in injected html", page.indexOf("<\\/script>") >= 0);
ok("cropper.js inlined (first script block)", /<script>\s*\n?\/\* ===== Cropper\.js v1\.6\.2/.test(page));
ok("startup block intact (loadSettings call present)", /loadSettings\(\)/.test(js));
// R1.0.12 结构检查
ok("page version meta present (R1.x.y)", /moink-page-version" content="R1\.\d+\.\d+"/.test(page));
// 10. R1.0.18 UI 重构：齿轮入口 / 返回按钮 / 无页签 / 步骤条 / 设备信息网格 / 分栏 / 文案
ok("R1.0.18 gear + back nav present", page.includes('id="gearBtn"') && page.includes('id="backBtn"'));
ok("R1.0.18 tab bar removed", page.indexOf('data-t="image"') < 0 && page.indexOf('id="tabs"') < 0);
ok("R1.0.18 subtitle", page.includes("把喜欢的瞬间，放在身边"));
ok("R1.0.18 header version line removed", page.indexOf('id="i-fw"') < 0);
ok("R1.0.18 dev copy cleaned (no 预览已跑通/预览还没跑通)",
   page.indexOf("预览已跑通") < 0 && page.indexOf("预览还没跑通") < 0);
ok("R1.0.18 steps bar + updateSteps", page.includes('id="steps"') && js.indexOf("function updateSteps") >= 0);
ok("R1.0.18 infoGrid device info", page.includes('class="infoGrid"'));
ok("R1.0.18 editor split columns", page.includes('id="editorGrid"') && page.includes('id="editRight"'));
ok("R1.0.21 mode desc removed", page.indexOf("\u50cf\u62cd\u7acb\u5f97\u4e00\u6837") < 0 && page.indexOf("\u5199\u70b9\u60f3\u8bf4\u7684") < 0 && page.indexOf("modeDesc") < 0 && js.indexOf("MODE_DEF") < 0);
ok("R1.0.19 image h2 removed", page.indexOf("选图 · 裁剪 · 上传") < 0);
ok("R1.0.19 autoPush fully removed", js.indexOf("scheduleAutoPush") < 0 && page.indexOf("autoPushRow") < 0 && js.indexOf("cfg.autoPush") < 0);
ok("R1.0.19 four-color stroke/bg/tbg/objbg", page.includes("黄边") && page.includes("红边") && page.includes("黄底") && page.includes("红底"));
ok("R1.0.19 dither hint wired", page.includes('id="dmodeHint"') && js.indexOf("DMODE_HINT") >= 0);
ok("R1.0.20 pad per-side checkboxes + default-on-check",
   page.includes('id="padTOn"') && page.includes('id="padBOn"')
   && page.includes('id="padLOn"') && page.includes('id="padROn"')
   && js.indexOf("function padSideVal") >= 0 && js.indexOf('pv("padTOn"') < 0);
ok("R1.0.20 numeric-only display (no units)", js.indexOf("return s + c.unit") < 0 && js.indexOf('v * 100 : v') >= 0);
ok("R1.0.20 textarea always editable", js.indexOf('tObjBg","txt') < 0);
ok("R1.0.21 steps 跟随选图 / 上传状态（R1.5.0 便签下线后只剩拍立得一条流程）",
   js.indexOf('var steps = ["① 选图", "② 裁剪调整", "③ 上传"];') >= 0
   && js.indexOf("var s = img ? (goEl.disabled ? 2 : 3) : 1;") >= 0
   && js.indexOf("① 编辑文字") < 0);
ok("R1.0.21 padGrid grid layout", js.indexOf('padOn ? "grid"') >= 0 && page.indexOf("grid-template-columns:repeat(4,1fr)") >= 0);
ok("R1.0.21 底色控件：只剩「文字底色」（便签的屏幕底色随模式一起移除）",
   page.indexOf('id="tbgRow"') < 0 && js.indexOf("tbgRow") < 0
   && page.indexOf('id="tObjBg"') >= 0 && page.indexOf('for="tBg"') < 0
   && js.indexOf('$("tBg")') < 0);
ok("R1.0.19 text add no auto-create", js.indexOf('text:"新文字"') < 0 && js.indexOf("请先输入文字") >= 0);
ok("R1.0.19 offline lab hint removed", page.indexOf("\u79bb\u7ebf\u5ba2\u6237\u7aef\u5bfc\u51fa\u4e3a\u5b9e\u9a8c\u6027\u529f\u80fd") < 0);
ok("R1.0.19 gamma unit + signed b/c", page.indexOf('unit:"\u00d7"') >= 0 && js.indexOf("fmtCtrl") >= 0);
// R1.0.22 修复与清理
ok("R1.0.22 upload error precedence + timeout",
   js.indexOf('xhr.responseText || ("HTTP " + xhr.status)') >= 0 && js.indexOf("xhr.timeout = 30000") >= 0);
ok("R1.0.22 stroke four-color echo", js.indexOf('tStrokeC").value = String(clamp(+o.strokeC|0, 0, 3))') >= 0);
ok("R1.0.22 pointerId guard", js.indexOf("pid:e.pointerId") >= 0 && js.indexOf("e.pointerId !== od.pid") >= 0);
ok("R1.0.22 offline export cleans runtime nodes",
   js.indexOf("cloneNode(true)") >= 0 && js.indexOf("#ctrls > *, #objLayer > *") >= 0);
ok("R1.0.22 EXIF revoke timing fixed",
   js.indexOf("im2.onload = function(){ URL.revokeObjectURL(url); adoptImg(im2, u2, f); };") >= 0);
// 9. R1.0.17 EXIF 归一 + 换图重置旋转 + 文字开关过滤
ok("R1.0.17 exifOrientation present", /function exifOrientation\(/.test(page));
ok("R1.0.17 cropRot reset on adoptImg", /curRot = 0; cfg\.cropRot = 0; syncRotUI\(\);/.test(page));
ok("R1.0.17 textOn filter in renderComposite", /cfg\.textOn \? cfg\.objsImg : \[\]/.test(page));
ok("text object system (drawObjs + normObjs)", js.indexOf("function drawObjs") >= 0 && js.indexOf("function normObjs") >= 0);
ok("padding module (contentRect/contentAR)", js.indexOf("function contentRect") >= 0 && js.indexOf("function contentAR") >= 0);
ok("cropper bridge (initCrop/getCropCanvas)", js.indexOf("function initCrop") >= 0 && js.indexOf("function getCropCanvas") >= 0);
ok("rot snap 90 (rotSnap)", js.indexOf("function rotSnap") >= 0);
ok("old crop code retired", js.indexOf("drawCropInto") < 0 && js.indexOf("imgSig") < 0 && js.indexOf("textMetrics") < 0);
ok("frozen algorithm intact (WARM_SAT_K + power law)", js.indexOf("WARM_SAT_K = 0.75") >= 0 && js.indexOf("Math.pow(s, 1 - WARM_SAT_K*g)") >= 0);

try {
  vm.runInContext(pure, sandbox, { filename: "page.js" });
} catch (e) {
  console.error("evaluate failed:", e.message);
  process.exit(1);
}

// 1. CRC16 金标准
const crc = sandbox.crc16(new Uint8Array([0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39]));
eq("crc16('123456789')", crc, 0x29B1);

// 2. 帧头 16 字节
const payload = new Uint8Array(105984).fill(0x55);
const frame = sandbox.frameWithHeader(payload);
eq("frame length", frame.length, 105984 + 16);
eq("magic0", frame[0], 0xA5);
eq("magic1", frame[1], 0x5A);
eq("version", frame[2], 1);
eq("width", (frame[4] << 8) | frame[5], 768);
eq("height", (frame[6] << 8) | frame[7], 552);
const len = (frame[8] << 24) | (frame[9] << 16) | (frame[10] << 8) | frame[11];
eq("payload length", len, 105984);
eq("crc match", ((frame[12] << 8) | frame[13]), sandbox.crc16(payload));

// 3. packIdx 纯色
const white = new Uint8Array(768 * 552).fill(1);
const buf = sandbox.packIdx(white);
eq("packIdx buffer length", buf.length, 105984);
eq("packIdx all-white == 0x55", buf.every(b => b === 0x55), true);

// 4/5. R1.0.22：modeAR / statsOf 死代码已删除
ok("R1.0.22 dead code removed (modeAR/statsOf)",
   js.indexOf("function modeAR") < 0 && js.indexOf("function statsOf") < 0);

// 6. R1.0.16 竖屏编辑 + rotateCW 桥接
eq("PANEL device frame 768x552", sandbox.PANEL.w + "x" + sandbox.PANEL.h, "768x552");
eq("edit space portrait 552x768", sandbox.EW + "x" + sandbox.EH, "552x768");
const rot = sandbox.rotateCW(new Uint8Array([1,2,3,4,5,6]), 3, 2);
eq("rotateCW mapping (CW 90)", Array.from(rot).join(","), "4,1,5,2,6,3");
ok("packFull bridge (rotateCW -> packIdx)", js.indexOf("packIdx(rotateCW(renderComposite(EW, EH), EW, EH))") >= 0);
// 7. R1.0.16 文字框宽度边柄
ok("wrap handle h-w (left-bottom) + red delete handle h-del",
   js.indexOf('"hnd h-w"') >= 0 && js.indexOf('"hnd h-del"') >= 0
   && js.indexOf('od.act === "wl"') >= 0 && js.indexOf('"hnd h-wr"') < 0
   && page.indexOf(".h-wl") < 0);
// 8. R1.0.16 卡片顺序：选图 → 风格 → 留白 → 文字
{
  const o1 = page.indexOf('id="pickCard"'), o2 = page.indexOf('id="styleCard"'),
        o3 = page.indexOf('id="padCard"'), o4 = page.indexOf('id="textCard"');
  ok("card order pick < style < pad < text", o1 >= 0 && o1 < o2 && o2 < o3 && o3 < o4);
}
ok("portrait migration (m16rot)", js.indexOf("cfg.m16rot") >= 0);

// R1.1.0（FB-010）：A1 驱动模式 + 编辑空间几何随模式切换
// R1.1.1（FB-011/FB-012）：已删 a1Hint + DEV_API 三态 + OTA 同步页面
// R1.1.2（FB-013）：真机定案后 A1 收敛为两档（768x552 顺序默认 / 800x600 对照）
ok("R1.1.2 A1 drive mode UI (a1Row/a1Sel, a1Hint removed)",
   page.includes('id="a1Row"') && page.includes('id="a1Sel"') && page.indexOf("a1Hint") < 0);
ok("R1.1.2 A1 options reduced to exactly two",
   page.includes('<option value="1">768×552 顺序</option>')
   && page.includes('<option value="2">方案A 原生800×600</option>')
   && page.indexOf("方案D 相位1") < 0 && page.indexOf("方案D 相位2") < 0
   && page.indexOf("方案A′") < 0 && page.indexOf("A1MODE_MAX = 4") < 0);
ok("R1.1.2 A1MODE default = 1 / max = 2",
   js.indexOf("var A1MODE = 1;") >= 0 && js.indexOf("var A1MODE_MAX = 2;") >= 0);
ok("R1.1.1 geometry helpers",
   js.indexOf("function panelGeomOf") >= 0 && js.indexOf("function applyPanelGeom") >= 0
   && js.indexOf("function migrateGeomTo") >= 0 && js.indexOf("function onGeomChanged") >= 0);
ok("R1.1.0 frame version follows geometry", js.indexOf("(PANEL.w === 800) ? 2 : 1") >= 0);
ok("R1.1.1 DEV_API tri-state + apiAtLeast2()",
   js.indexOf("var DEV_API = 0") >= 0 && js.indexOf("function apiAtLeast2()") >= 0
   && js.indexOf("DEV_API === 0 || DEV_API >= 2") >= 0);
ok("R1.1.0 a1_mode saved with panel",
   js.indexOf('fields.a1_mode = $("a1Sel").value') >= 0 && js.indexOf("syncPanelRows") >= 0);
// R1.2.0（FB-015）：版本号由 fw/page 两套合并为 MOINK_VERSION 一套；
//   固件与控制页合并到同一入口 /api/upload，由固件按请求体首块内容嗅探分流。
ok("R1.2.0 统一升级入口 UI (upFile/upGo/upmsg)",
   page.includes('id="upFile"') && page.includes('id="upGo"')
   && page.includes('class="savestate" id="upmsg"')
   && page.includes('accept=".bin,.html,.htm"') && page.includes('id="webClear"'));
ok("R1.2.0 旧的两个入口与同步复选框已删",
   page.indexOf('id="fwFile"') < 0 && page.indexOf('id="fwGo"') < 0
   && page.indexOf('id="webFile"') < 0 && page.indexOf('id="webGo"') < 0
   && page.indexOf("fwSyncPage") < 0 && page.indexOf("sync_page") < 0);
ok("R1.2.0 单一入口常量 + 无旧路由残留",
   js.indexOf('var UP_URL = api("/api/upload");') >= 0
   && js.indexOf("OTA_URL") < 0 && js.indexOf('"/api/ota"') < 0
   && page.indexOf('"/api/ota"') < 0);
ok("R1.2.0 双端同规则嗅探 (sniffKind + readHead, 固件 0xE9 / 页面 <!doctype)",
   js.indexOf("function sniffKind(bytes)") >= 0
   && js.indexOf("function readHead(file, cb)") >= 0
   && js.indexOf("bytes[0] === 0xE9") >= 0
   && js.indexOf('head.indexOf("<!doctype")') >= 0
   && js.indexOf("改 sniffKind() 必须同步改 src/ota_web.c 的 sniff_kind()") >= 0);
ok("R1.2.0 固件重启导致响应可能收不到 -> sentAll 兜底",
   js.indexOf("var sentAll = false") >= 0 && js.indexOf("if (isFw && sentAll)") >= 0);
// R1.1.3（FB-014）：设置项就地保存状态（不再依赖图片页里不可见的 #msg）+ 保存后回读核对
ok("R1.1.3 就地状态位 3 处 (sleepmsg/apmsg/panelmsg)",
   page.indexOf('class="savestate" id="sleepmsg"') >= 0
   && page.indexOf('id="apmsg"') >= 0 && page.indexOf('id="panelmsg"') >= 0
   && page.indexOf('id="sleepmsg" style="margin:0"') < 0);
ok("R1.1.3 就地状态助手 (saveState/readSettings/verifyFields)",
   js.indexOf("function saveState(id, kind, text)") >= 0
   && js.indexOf("function readSettings(cb)") >= 0
   && js.indexOf("function verifyFields(fields, cb)") >= 0);
ok("R1.1.3 保存后回读 /api/settings 逐字段核对（不轻信 HTTP 200）",
   js.indexOf('fetch(api("/api/settings?t=" + Date.now()))') >= 0
   && js.indexOf("badLabel(bad)") >= 0 && js.indexOf("SETT_LABEL") >= 0);
ok("R1.1.3 失败/未生效/没收到应答都有明确提示",
   js.indexOf('"✗ 保存失败：设备返回 "') >= 0 && js.indexOf('"✗ 未生效："') >= 0
   && js.indexOf('"⚠ 没收到应答：') >= 0);
ok("R1.1.3 本地预览不谎报网络异常（且照常切档预览）",
   js.indexOf("if (LOCAL_PREVIEW){") >= 0
   && js.indexOf("本地预览（未连设备）：只在本页生效") >= 0);
ok("R1.1.3 旧的静默提示路径已移除",
   page.indexOf('msg("保存失败", "err")') < 0 && page.indexOf('msg("网络错误", "err")') < 0);
// R1.3.0（评审 P0）：设备页维护操作（恢复内置页面/清理残影/恢复出厂）反馈原写进
//   图片页 #msg —— 切到设备页操作时全程不可见（连失败都静默）。改就地 savestate 胶囊。
ok("R1.3.0 维护操作就地状态位 3 处 + 不再写图片页 #msg",
   page.indexOf('class="savestate" id="webmsg"') >= 0
   && page.indexOf('class="savestate" id="clrmsg"') >= 0
   && page.indexOf('class="savestate" id="facmsg"') >= 0
   && page.indexOf('msg("已恢复内置页面') < 0
   && page.indexOf('msg("清理残影中') < 0 && page.indexOf('msg("已恢复出厂') < 0);
ok("R1.3.0 维护操作 saveState 反馈 + factory 无应答兜底",
   js.indexOf('saveState("webmsg", "busy", "恢复中…")') >= 0
   && js.indexOf('saveState("clrmsg", "ok", "✓ 残影清理完成")') >= 0
   && js.indexOf('saveState("clrmsg", "err", "✗ 清理失败，请重试")') >= 0
   && js.indexOf('saveState("facmsg", "warn", "⚠ 没收到应答：设备可能正在重启') >= 0);
ok("R1.2.0 热点密码单字段语义（留空=保持 / 勾选清除=开放，FB-014①）",
   page.indexOf("留空 = 保持当前密码") >= 0 && page.indexOf('id="apClearPw"') >= 0
   && page.indexOf("留空 = 清除密码") < 0 && page.indexOf("留空则不修改") < 0);
ok("R1.2.0 热点保存按需携带 pass（缺省不发送 = 固件保持）",
   js.indexOf('if ($("apClearPw").checked) fields.pass = "";') >= 0
   && js.indexOf('else if ($("apPass").value) fields.pass = $("apPass").value;') >= 0);
(function(){
  /* 版本号断言跟着 src/version.h 走，避免每次发版都要手改这条（历史踩过：改了 meta 忘了改这里）。 */
  const vh = fs.readFileSync(path.join(__dirname, "..", "src", "version.h"), "utf8");
  const m = /#define\s+MOINK_VERSION\s+"([^"]+)"/.exec(vh);
  ok("版本 meta == version.h MOINK_VERSION（" + (m ? m[1] : "?") + "）",
     !!m && page.indexOf('content="' + m[1] + '"') >= 0);
})();
// DEV_API 三态行为：0 = 未连设备（离线乐观） / 1 = 旧固件（收紧） / 2 = 新固件
// R1.1.2（FB-013）：mode 1 = 768x552 顺序（默认正解，任何 api 下都是 768x552）；
//                   mode 2 = 方案A 原生 800x600（需 api>=2，离线按可用乐观）。
sandbox.DEV_API = 0;
eq("R1.1.2 offline(0) + mode 1 (default) -> 768x552",
   sandbox.panelGeomOf(1, 1).w + "x" + sandbox.panelGeomOf(1, 1).h, "768x552");
eq("R1.1.2 offline(0) + mode 2 (native800) -> 800x600",
   sandbox.panelGeomOf(1, 2).w + "x" + sandbox.panelGeomOf(1, 2).h, "800x600");
sandbox.DEV_API = 1;
eq("R1.1.2 legacy api=1 + mode 2 -> 768x552 (tightened)",
   sandbox.panelGeomOf(1, 2).w + "x" + sandbox.panelGeomOf(1, 2).h, "768x552");
eq("R1.1.2 legacy api=1 + mode 1 -> 768x552",
   sandbox.panelGeomOf(1, 1).w + "x" + sandbox.panelGeomOf(1, 1).h, "768x552");
sandbox.DEV_API = 2;
eq("R1.1.2 api=2 + mode 2 -> native 800x600",
   sandbox.panelGeomOf(1, 2).w + "x" + sandbox.panelGeomOf(1, 2).h, "800x600");
eq("R1.1.2 api=2 + mode 1 (default) -> 768x552",
   sandbox.panelGeomOf(1, 1).w + "x" + sandbox.panelGeomOf(1, 1).h, "768x552");
sandbox.DEV_API = 0;   /* 还原初态，避免污染后续断言 */
eq("R1.1.0 panelGeomOf A0 ignores mode", sandbox.panelGeomOf(0, 1).w + "x" + sandbox.panelGeomOf(0, 1).h, "768x552");
eq("R1.1.0 GEOM_SRC default geometry", sandbox.GEOM_SRC.w + "x" + sandbox.GEOM_SRC.h, "552x768");

ok("R1.2.0 状态栏单版本号 (版本/接口, 无 fw/page 双行)",
   page.includes('id="i-verD"') && page.includes('id="i-apiD"')
   && page.indexOf('id="i-fw"') < 0 && page.indexOf('id="i-page"') < 0);
ok("R1.2.0 热更页落后固件时红字警示 (DEV_FW + verNum + j.ver||j.fw)",
   js.indexOf('var DEV_FW = "";') >= 0 && js.indexOf("function verNum(v)") >= 0
   && js.indexOf("var ver = j.ver || j.fw") >= 0
   && js.indexOf("控制页版本低于固件") >= 0);
ok("R1.2.0 A1.1 诊断变体 UI 整块删除 (选项/行/字段)",
   page.indexOf("A1.1") < 0 && page.indexOf('id="a11Sel"') < 0
   && page.indexOf('id="a11Row"') < 0 && js.indexOf("a11_var") < 0
   && js.indexOf("a11Sel") < 0);
ok("R1.2.0 版本号比较按 主*10000+次*100+修订",
   js.indexOf("+m[1] * 10000 + +m[2] * 100 + +m[3]") >= 0);
ok("页脚存在 (footer#appFoot)",
   page.indexOf('id="appFoot"') >= 0 && page.indexOf("footer#appFoot") >= 0);
ok("页脚含 GitHub 地址", page.indexOf('href="https://github.com/mccding/huaweimoink"') >= 0);
ok("页脚版本号运行时填充 (footVer + meta)",
   page.indexOf('id="footVer"') >= 0 && page.indexOf('meta[name="moink-page-version"]') >= 0);
// R1.3.0 UI 打磨：固定底栏 / accent-color / 对比度 / 0° 复位 / 上传按钮状态机 / 步骤 ✓
ok("R1.3.0 mobile fixed action bar (#actions + body 预留)",
   page.includes('id="actions"') && page.includes("#actions{ position:fixed")
   && page.includes("padding-bottom:112px"));
ok("R1.3.0 accent-color for native controls",
   page.includes("accent-color:var(--acc)"));
ok("R1.3.0 contrast tokens (--mut deepened + --acc-deep for text)",
   page.includes("--mut:#7a6f5e") && page.includes("--acc-deep:#b25719")
   && page.includes("var(--acc-deep)"));
ok("R1.3.0 crop row 0° reset button",
   page.includes('id="rotZero"') && js.indexOf('$("rotZero").addEventListener') >= 0);
ok("R1.3.0 crop rotate 90° pair uses matched glyphs (↺ U+21BA / ↻ U+21BB)",
   page.indexOf('&#8634; 90') >= 0 && page.indexOf('&#8635; 90') >= 0
   && page.indexOf('&#8630;') < 0);
ok("R1.3.0 auto-scroll to crop card on mobile",
   js.indexOf("function scrollToCrop()") >= 0 && js.indexOf("scrollToCrop();") >= 0);
ok("R1.3.0 upload label state machine (合成→上传中 xx%→已上传 ✓→还原)",
   js.indexOf('goEl.textContent = "正在合成…"') >= 0
   && js.indexOf('"上传中 " + Math.round') >= 0
   && js.indexOf('"已上传 ✓"') >= 0
   && js.indexOf("goResetTmr") >= 0);
ok("R1.3.0 step ③ done state (stepDone + span.done)",
   js.indexOf("var stepDone = false;") >= 0 && js.indexOf('class="done"') >= 0
   && page.includes("#steps span.done"));
// R1.3.0 UI 打磨 P1：文字面板分组 + 数值回显 / 触屏热区 / 预览空态 / 表单可访问性
ok("R1.3.0 P1 text panel pair groups + outputs",
   page.includes('id="o-tSize"') && page.includes('id="o-tRot"') && page.includes('id="o-tStroke"')
   && page.includes('<span class="pair"><label for="tFont">')
   && page.includes('for="tStrokeC"'));
ok("R1.3.0 P1 touch targets on coarse pointer",
   page.includes("@media (pointer:coarse)") && page.includes(".tobj .hnd::before")
   && page.includes("button.mini{ padding:12px 14px; min-height:44px; }"));
ok("R1.3.0 P1 preview empty-state hint",
   page.includes('id="prevHint"') && js.indexOf("function syncPrevHint()") >= 0
   && js.indexOf("syncPrevHint();") >= 0);
ok("R1.3.0 P1 a11y (slider labels + keyboard drop zone)",
   js.indexOf("lab.htmlFor = c.id") >= 0
   && page.includes('id="drop" role="button" tabindex="0"')
   && js.indexOf('dropEl.addEventListener("keydown"') >= 0
   && page.includes('for="rotRange"') && page.includes('for="core"'));
// R1.5.0：文字面板的「待办列表」三件套已下线 —— 清单归「待办」tab（设备存文档 + 两套版式）。
ok("R1.5.0 文字面板不再有列表模式（tList / tListBox / tFit 与 groups 度量全删）",
   page.indexOf('id="tList"') < 0 && page.indexOf('id="tListBox"') < 0
   && page.indexOf('id="tFit"') < 0 && js.indexOf("m.list") < 0
   && js.indexOf("groups.push(ls)") < 0 && js.indexOf("list:+o.list") < 0
   && js.indexOf('if (!regs.length || +o.list) return "";') < 0);
ok("R1.5.0 删列表模式没伤到普通文字：折行 / 描边 / 度量同源仍在",
   js.indexOf("var sz = m.sz;") >= 0 && js.indexOf("ctx.strokeText(m.lines[j], x0, yy2);") >= 0
   && js.indexOf('var lines = t.replace(/\\s+$/, "") ? wrapText(mc, t, maxW) : [];') >= 0);

// R1.3.0 UI 评审修复（P1 全 3 类 + P2 全 5 类 + 次要项 4 处）
ok("R1.3.0 P1-1 设备页控件可访问名 (11 label-for + upFile aria-label)",
   ["sleepSel","wakeMin","apSsid","apPass","apClearPw","wifiPwrSel",
    "staEnable","staSsid","staPass","panelSel","a1Sel"].every(function(id){
      return page.includes('for="' + id + '"');
    })
   && /id="upFile"[^>]*aria-label="选择升级文件/.test(page));
ok("R1.3.0 P1-1 编辑页补齐 (dmode/space label + 留白像素 aria-label)",
   page.includes('for="dmode"') && page.includes('for="space"')
   && page.includes('aria-label="上留白像素"') && page.includes('aria-label="下留白像素"')
   && page.includes('aria-label="左留白像素"') && page.includes('aria-label="右留白像素"')
   && page.includes('aria-label="启用上留白"') && page.includes('aria-label="启用右留白"'));
ok("R1.3.0 P1-2 #msg aria-live polite",
   page.includes('<p id="msg" aria-live="polite"></p>'));
ok("R1.3.0 P1-3 对比度 (#conn→4.9:1 / modes→4.6:1)",
   page.includes("color:#6b5c44") && page.includes("color:#6f6353"));
ok("R1.3.0 P2-1 标题层级 (h2 统一 + 无 h3 跳级)",
   page.indexOf("<h3") < 0 && page.indexOf("h3{") < 0
   && page.includes(".card h2{ margin:16px 0 6px")
   && page.includes('<h2 style="margin-top:0">设备信息</h2>'));
ok("R1.3.0 P2-2 objLayer aria-hidden",
   page.includes('<div id="objLayer" aria-hidden="true">'));
ok("R1.3.0 P2-3 设置页进历史 (pushState/popstate + 滚动恢复)",
   js.indexOf('history.pushState({ tab:"device" }') >= 0
   && js.indexOf('window.addEventListener("popstate"') >= 0
   && js.indexOf("var editScrollY = 0;") >= 0
   && js.indexOf("window.scrollTo(0, keepScroll ? editScrollY : 0)") >= 0);
ok("R1.3.0 P2-4 触屏表单 16px (iOS 聚焦不放大)",
   page.includes("input[type=text],input[type=password],input[type=number],select,textarea{ font-size:16px; }"));
ok("R1.3.0 P2-5 cropImg 中文 alt",
   page.includes('<img id="cropImg" alt="待裁剪的图片">'));
ok("R1.3.0 次要: upGo 双击防护 + 按钮按压反馈",
   js.indexOf('if ($("upGo").disabled) return;') >= 0
   && page.includes("button:not(:disabled):active{ filter:brightness(.93); }"));
ok("R1.3.0 次要: 内容 tab 44px 热区 + drop.has 旧提示隐藏",
   page.includes("#cvtabs .cvTab{ min-height:44px; }")
   && page.includes("#drop.has br,#drop.has span{ display:none; }"));
// R1.3.0 留白文字：新文字自动排入留白区（自适应字号 + 居中 + 左右竖排）
ok("R1.3.0 留白文字: padRegions/padPlaceObj + 自动排入接线",
   js.indexOf("function padRegions()") >= 0 && js.indexOf("function padPlaceObj(o, objs)") >= 0
   && js.indexOf("var placed = padPlaceObj(o, objs);") >= 0 && js.indexOf('已排入「') >= 0);
ok("R1.3.0 留白文字: 区域顺序 上→下→左→右 + 左右竖排（中文逐字 / 英文按单词）",
   js.indexOf('id:"T"') >= 0 && js.indexOf('id:"B"') >= 0
   && js.indexOf('id:"L"') >= 0 && js.indexOf('id:"R"') >= 0
   && js.indexOf("vert:1") >= 0
   && js.indexOf("[A-Za-z0-9]+(?:['’\\-][A-Za-z0-9]+)*|[^\\s]") >= 0
   && js.indexOf('toks.join("\\n")') >= 0);
ok("R1.3.0 留白文字: 字号二分自适应 + 槽位居中写入",
   js.indexOf("function padPlaceOne(o, reg, slot)") >= 0
   && js.indexOf("o.wrap = Math.max(24, Math.round(slot.w * 0.94))") >= 0
   && js.indexOf("while (hi - lo > 1)") >= 0
   && js.indexOf("o.pad = reg.id;") >= 0
   && js.indexOf("o.pad = reg.id;") < js.indexOf("while (hi - lo > 1)")   /* 先落区标记再二分：竖排行距生效 */
   && js.indexOf("o.x = Math.round(slot.x); o.y = Math.round(slot.y); o.pad = reg.id;") >= 0);
ok("R1.3.0 修复: 左右竖排单词不拆行（fits 要求折行数 === 分词行数）",
   js.indexOf("var need = reg.vert && o.text ? o.text.split(\"\\n\").length : 0;") >= 0
   && js.indexOf("(m.lines.length === need || !need)") >= 0);
ok("R1.3.0 留白文字: 同区分摊 padSlot（上下分行 / 左右分列）",
   js.indexOf("function padSlot(reg, k, n)") >= 0
   && js.indexOf("occ.push(o)") >= 0
   && js.indexOf("padSlot(best, j, occ.length)") >= 0);
ok("R1.3.0 留白文字: 占位分摊 + pad 字段持久化 (normObjs/newObj)",
   js.indexOf("objs[j].pad === regs[i].id") >= 0
   && js.indexOf('pad:/^[TBLR]$/.test(String(o.pad||"")) ? String(o.pad) : ""') >= 0
   && js.indexOf('wrap:0, pad:"" };') >= 0);
ok("R1.3.0 留白文字: padCard 使用提示",
   page.indexOf("添加文字时自动排入留白区") >= 0 && page.indexOf("左/右留白文字自动竖排") >= 0
   && page.indexOf("中文逐字、英文按单词") >= 0);
ok("R1.3.0 修复: 未选图时裁剪卡整卡隐藏（R1.5.0 便签下线后归 syncCropCard）",
   js.indexOf('function syncCropCard(){ $("cropCard").style.display = img ? "" : "none"; }') >= 0
   && js.indexOf("  syncCropCard();\n  initCrop();") >= 0);
// R1.3.0 手机友好：清空输入按钮 + 编辑留白文字实时重适配 + 左右竖排行距/上限
ok("R1.3.0 手机友好: 「清空输入」按钮（HTML + 清框清对象 + 聚焦续输）",
   page.indexOf('id="txtClear">清空输入</button>') >= 0
   && js.indexOf('$("txtClear").addEventListener("click"') >= 0
   && js.indexOf('txtEl.value = "";') >= 0
   && js.indexOf('if (o){ o.text = ""; relayoutOne(selIdx); }') >= 0
   && js.indexOf("txtEl.focus()") >= 0);
ok("R1.3.0 修复: 编辑留白文字实时重适配（padRefitOne + input 接线 + 保留 x/y/rot）",
   js.indexOf("function padRefitOne(o, objs)") >= 0
   && js.indexOf("    padRefitOne(o, curObjs());") >= 0
   && js.indexOf("padSlot(reg, k, occ.length)") >= 0
   && js.indexOf("o.x = x0; o.y = y0; o.rot = r0;") >= 0);
ok("R1.3.0 修复: 左右竖排行距收紧 1.08 + 高度上限 0.92",
   js.indexOf('(o.pad === "L" || o.pad === "R")') >= 0
   && js.indexOf("vert ? 1.08 : 1.35") >= 0
   && js.indexOf("slot.h * (reg.vert ? 0.92 : 0.8)") >= 0);
ok("R1.3.0 修复: 上下留白折行短行与其它行左对齐（块仍居中）",
   js.indexOf('var padTB = (o.pad === "T" || o.pad === "B");') >= 0
   && js.indexOf('ctx.textAlign = padTB ? "left" : "center";') >= 0
   && js.indexOf("var x0 = padTB ? -m.mw/2 : 0;") >= 0);

// 11. R1.4.0（FB-017）：相册轮播 —— 设备页面板 / 接线 / 端点契约 / 缩略图解码 / 反馈文案
ok("R1.4.0 轮播面板: 控件 + 状态位齐备（HTML）",
   page.indexOf('id="paneCar"') >= 0
   && page.indexOf('id="carOn"') >= 0
   && page.indexOf('id="carModeSel"') >= 0
   && page.indexOf('id="carWakeHint"') >= 0
   && page.indexOf('id="carSaveCfg"') >= 0
   && page.indexOf('id="carcfgmsg"') >= 0
   && page.indexOf('id="carAdd"') >= 0
   && page.indexOf('id="carAdvance"') >= 0
   && page.indexOf('id="carList"') >= 0
   && page.indexOf('id="carCount"') >= 0
   && page.indexOf('id="carlistmsg"') >= 0);
ok("R1.4.0 轮播接线: 六个 car* 函数 + 启动尾 carRefresh + 节奏回显走设备读数",
   js.indexOf("function carPill(") >= 0
   && js.indexOf("function carSyncHint(") >= 0
   && js.indexOf("function carDecode(") >= 0
   && js.indexOf("function carFetchThumb(") >= 0
   && js.indexOf("function carRenderList(") >= 0
   && js.indexOf("function carRefresh(") >= 0
   && js.indexOf("  carSyncHint(j);") >= 0
   && js.indexOf("carRefresh();   /* R1.4.0：轮播列表（R1.5.0 起在首页「轮播」tab） */") >= 0);
ok("R1.4.0 轮播端点契约: 六条 /api/carousel/* 齐备",
   js.indexOf('api("/api/carousel/list?t=') >= 0
   && js.indexOf('api("/api/carousel/frame?i=') >= 0
   && js.indexOf('api("/api/carousel/add")') >= 0
   && js.indexOf('api("/api/carousel/del")') >= 0
   && js.indexOf('api("/api/carousel/advance")') >= 0
   && js.indexOf('api("/api/carousel/cfg")') >= 0);
ok("R1.4.0 轮播缩略图: packIdx 逆变换公式 + 两种画布尺寸",
   js.indexOf("pay.length === 105984") >= 0
   && js.indexOf("pay.length === 120000") >= 0
   && js.indexOf("var col = H - 1 - rp;") >= 0
   && js.indexOf("PAL[(b >> (k << 1)) & 3]") >= 0
   && js.indexOf("((((xb << 2) + 3 - k) * H) + col) << 2") >= 0);
ok("R1.4.0 轮播反馈: 守卫 / 已满 / 回读不一致 / 删除确认 / 旧固件提示",
   js.indexOf("✗ 预览生成中，请稍候再试") >= 0
   && js.indexOf("轮播已满") >= 0
   && js.indexOf("回读不一致") >= 0
   && js.indexOf('confirm("删除第 ') >= 0
   && js.indexOf("此设备固件不含轮播功能") >= 0
   && js.indexOf("本地预览（未连设备）：无法读取轮播列表") >= 0);
ok("R1.4.0 轮播设置: carLast 回读记录 + 无变化不撞 400（前端守卫）",
   js.indexOf("var carLast = null;") >= 0
   && js.indexOf("carLast = { on: !!j.on, mode: +j.mode || 0, int_s: +j.int_s || 0 };") >= 0
   && js.indexOf("carLast && carLast.on === !!wantOn && carLast.mode === wantMode") >= 0
   && js.indexOf("✓ 设置未变化，无需保存") >= 0);
ok("R1.4.0 轮播设置: 400 no changes 响应体甄别（carLast 未知窗口兜底）",
   js.indexOf('String(e.text || "").indexOf("no changes")') >= 0
   && js.indexOf('throw { code: r.status, text: t || "" };') >= 0);

// 12. R1.5.0 功能2：节奏自由指定（分钟）+ 换图间隔独立 + 提示改走设备读数
ok("R1.5.0 自定义节奏控件: 两个分钟数值框，写死的 wakeSel 已移除",
   page.includes('id="wakeMin"') && page.includes('id="carIntMin"')
   && page.includes('class="mins" min="0" max="1440"')
   && page.indexOf('id="wakeSel"') < 0
   && page.includes('input[type=number].mins{ width:88px; flex:none; }'));
ok("R1.5.0 分钟↔秒换算 + 越界校验（0~1440，0 = 关闭）",
   js.indexOf("var MIN_MAX = 1440;") >= 0
   && js.indexOf("function readMin(el, stateId, label)") >= 0
   && js.indexOf("function minToSec(el, stateId, label)") >= 0
   && js.indexOf("function secToMin(sec)") >= 0
   && js.indexOf("var wake = minToSec($(\"wakeMin\"), \"sleepmsg\", \"自动唤醒\");") >= 0
   && js.indexOf("var wantInt = minToSec($(\"carIntMin\"), \"carcfgmsg\", \"换图间隔\");") >= 0
   && js.indexOf("的整数分钟（0 = 关闭）") >= 0
   && js.indexOf("还是空的：填 0 = 关闭") >= 0);
ok("R1.5.0 cfg POST 携带 int_s + 回读逐字段核对 + bad int_s 专译",
   js.indexOf('body: "on=" + wantOn + "&mode=" + wantMode + "&int_s=" + wantInt') >= 0
   && js.indexOf("(j.int_s != null && +j.int_s !== wantInt)") >= 0
   && js.indexOf('indexOf("bad int_s")') >= 0
   && js.indexOf("✓ 已保存并生效（\" + fmtEvery(wantInt) + \"）") >= 0);
ok("R1.5.0 节奏回显只转述设备读数（int_s / next_in_s），旧谎报文案已消失",
   js.indexOf("function fmtEvery(sec)") >= 0 && js.indexOf("function fmtIn(sec)") >= 0
   && js.indexOf("+j.next_in_s") >= 0 && js.indexOf("已到换图点") >= 0
   && js.indexOf("自动唤醒为「关闭」，轮播不会被触发") < 0
   && js.indexOf("carSyncHint(s.wake_s)") < 0
   && js.indexOf("需 R1.5.0+") >= 0);
ok("R1.5.0 不休眠语义说明（有节奏时空闲仍会睡，否则定时器不武装）",
   page.includes("「不休眠」不等于永不入睡")
   && page.includes("换图间隔与「自动唤醒」各自独立计时")
   && page.includes("自动唤醒、轮播换图或日历改日期"));

// 13. R1.5.0 功能1：日历自动改日期 —— 换算与回显真跑一遍（时刻/日期错位是最容易咬人的 bug）
(function(){
  const start = js.indexOf("var CAL_WD = [");
  const end   = js.indexOf("function calRefresh(){");
  if (start < 0 || end < 0 || end <= start){ ok("R1.5.0 日历函数块可抽取", false); return; }
  const nodes = {};
  function el(id){ return nodes[id] || (nodes[id] = { textContent: "", value: "", checked: false, style: {} }); }
  const sandbox = {
    $: el, console: console, Date: Date, Promise: Promise,
    clamp: function(v,a,b){ return v<a?a:(v>b?b:v); },
    fmtIn: function(s){ return "<" + s + ">"; },
    api: function(p){ return p; },
    LOCAL_PREVIEW: true,
    saveState: function(id, kind, text){ el(id).textContent = text; el(id).kind = kind; }
  };
  vm.runInNewContext(js.slice(start, end), sandbox, { filename: "calendar-block.js" });
  ok("R1.5.0 日历函数块可抽取", true);

  eq("todToSec 00:01", sandbox.todToSec("00:01"), 60);
  eq("todToSec 23:59", sandbox.todToSec("23:59"), 86340);
  eq("todToSec 7:05（单位数小时）", sandbox.todToSec("7:05"), 25500);
  ok("todToSec 拒绝 24:00 / 空 / 乱填",
     sandbox.todToSec("24:00") === null && sandbox.todToSec("") === null
     && sandbox.todToSec(null) === null && sandbox.todToSec("ab:cd") === null
     && sandbox.todToSec("12:60") === null);
  eq("secToTod 60", sandbox.secToTod(60), "00:01");
  eq("secToTod 86399 → 23:59（秒位不进位）", sandbox.secToTod(86399), "23:59");
  eq("secToTod 0", sandbox.secToTod(0), "00:00");
  eq("时刻往返（页面填 → 设备存 → 页面回显）",
     sandbox.todToSec(sandbox.secToTod(36000)), 36000);

  /* 核心契约：伪 UTC 秒 = 把手机本地日历读数当 UTC 打包，固件 gmtime_r 还原出的就是它。 */
  const t = sandbox.calWallPack(), g = new Date(t * 1000), n = new Date();
  eq("calWallPack：按 UTC 解回来 = 手机屏幕上的日期时间",
     [g.getUTCFullYear(), g.getUTCMonth(), g.getUTCDate(), g.getUTCHours(), g.getUTCMinutes()].join("-"),
     [n.getFullYear(), n.getMonth(), n.getDate(), n.getHours(), n.getMinutes()].join("-"));
  ok("calWallPack 落在固件认可的日期窗口（2020..2099）", t > 1577836800 && t <= 4102444799);

  sandbox.calTake({ on: 1, tod_s: 60, synced: 1, y: 2026, m: 10, d: 5, wd: 1, next_in_s: 3600 });
  sandbox.calShowState(sandbox.calState);
  eq("回显：设备日期用「几年几月几日 星期X」", el("calNow").textContent, "2026年10月5日 星期一");
  eq("回显：控件回填设备值", el("calTod").value + "/" + el("calOn").checked, "00:01/true");
  eq("回显：倒计时转述设备 next_in_s", el("calNext").textContent, "约 <3600>后（每天 00:01）");

  sandbox.calShowState({ on: 1, tod_s: 60, synced: 1, y: 2026, m: 2, d: 1, wd: 0, next_in_s: 1 });
  eq("回显：星期日用「日」不是「天」", el("calNow").textContent, "2026年2月1日 星期日");
  ok("回显：next_in_s=1 译成「已到换日点」而不是还剩 1 秒",
     el("calNext").textContent.indexOf("已到换日点") === 0);

  sandbox.calShowState({ on: 0, tod_s: 60, synced: 1, y: 2026, m: 10, d: 5, wd: 1, next_in_s: 0 });
  ok("回显：关闭时不谎报排期", el("calNext").textContent === "自动改日期已关闭");

  sandbox.calShowState({ on: 1, tod_s: 60, synced: 0, next_in_s: 0 });
  ok("回显：掉电后（synced=0）标黄并说明不会自动改日期",
     el("calNow").textContent.indexOf("未同步") >= 0
     && el("calNext").textContent.indexOf("不会自动改日期") >= 0
     && el("calNow").style.color === "#a5701a");
  sandbox.calShowState({ on: 1, tod_s: 60, synced: 1, y: 2026, m: 10, d: 5, wd: 1, next_in_s: 0 });
  ok("回显：synced 却 next_in_s=0 时说「刷新页面重试」而不是硬编一个倒计时",
     el("calNext").textContent.indexOf("未排期") >= 0);

  /* 旧固件（无 /api/time）：404 必须译成「需 R1.5.0」，不能报「连接中断」。 */
  ok("R1.5.0 旧固件 404 专译（日历三处入口都认得）",
     js.indexOf("此设备固件不含日历功能（需 R1.5.0 及以上）") >= 0
     && (js.match(/设备固件不含日历功能（需 R1\.5\.0\+ 固件）/g) || []).length >= 3);
  ok("R1.5.0 日历端点齐备（GET/POST /api/time + cfg + show）",
     js.indexOf('api("/api/time?t="') >= 0
     && js.indexOf('api("/api/time")') >= 0
     && js.indexOf('api("/api/calendar/cfg")') >= 0
     && js.indexOf('api("/api/calendar/show")') >= 0
     && js.indexOf('body: "on=" + wantOn + "&tod_s=" + wantTod + "&lang=" + wantLang + "&style=" + wantStyle') >= 0
     && js.indexOf('body: "t=" + calWallPack()') >= 0);
  ok("R1.5.0 日历回读核对：cfg 应答即状态，不一致/未同步各自专译",
     js.indexOf("if (!!j.on !== !!wantOn || +j.tod_s !== wantTod || ((+j.lang === 1 ? 1 : 0) !== wantLang)") >= 0
     && js.indexOf("|| (calStyleNum(j.style) !== wantStyle)){") >= 0
     && js.indexOf("但设备日期还没同步") >= 0
     && js.indexOf("设置未变化，无需保存") >= 0
     && js.indexOf("/no changes/.test(e.text") >= 0);
  ok("R1.5.0 日历与轮播抢屏：R1.5.2 起改由设备端互斥解决（旧「互相盖屏 / 不抢屏」文案已撤）",
     page.indexOf("两者会互相盖掉对方刚上屏的画面") < 0
     && page.indexOf("不会</b>立刻改掉当前画面") < 0
     && js.indexOf("但轮播也开着 —— 两者会互相盖屏") < 0
     && page.indexOf("只能有一个主人") >= 0);
  ok("R1.5.0 日历控件与就地反馈齐备（含 label for / 本地预览守卫 / 启动回显）",
     page.includes('id="calBox"') && page.includes('id="calTod" class="tod" value="00:01"')
     && ["calOn", "calNow", "calNext", "calcfgmsg", "calmsg"].every(function(id){
          return page.includes('id="' + id + '"'); })
     && ["calOn", "calTod"].every(function(id){ return page.includes('for="' + id + '"'); })
     && page.includes('input[type=time].tod{ width:124px; flex:none; }')
     && js.indexOf('saveState("calcfgmsg", "warn", "⚠ 本地预览') >= 0
     && js.indexOf('saveState("calmsg", "warn", "⚠ 本地预览') >= 0
     && js.indexOf("calRefresh();") >= 0
     && js.indexOf("if (!j.synced) return calPush(true)") >= 0);

  /* 中英文日历：lang 是第三个配置项，回显 / 无变化判定 / 保存 / 核对一条链路都不能漏。 */
  sandbox.calTake({ on: 1, tod_s: 60, lang: 1, synced: 1, y: 2026, m: 10, d: 5, wd: 1, next_in_s: 3600 });
  eq("回显：lang=1 选中 English", el("calLang").value + "/" + sandbox.calLast.lang, "1/1");
  sandbox.calTake({ on: 1, tod_s: 60, lang: 0, synced: 1, y: 2026, m: 10, d: 5, wd: 1, next_in_s: 3600 });
  eq("回显：lang=0 选回中文", el("calLang").value + "/" + sandbox.calLast.lang, "0/0");
  el("calLang").value = "1";
  sandbox.calTake({ on: 1, tod_s: 60, synced: 1, y: 2026, m: 10, d: 5, wd: 1, next_in_s: 3600 });
  ok("旧固件应答不含 lang：不动用户已选的下拉，calLast 归 0（下次保存会补发一次）",
     el("calLang").value === "1" && sandbox.calLast.lang === 0);
  ok("R1.5.0 语言下拉：label for + 中英两个选项",
     page.includes('<label for="calLang">日历语言</label>')
     && page.includes('<select id="calLang"><option value="0">中文</option>'
                    + '<option value="1">English</option></select>'));
  ok("R1.5.0 语言进无变化判定（否则只改语言会被本地拦下、永远存不进去）",
     js.indexOf("var wantLang = ($(\"calLang\").value === \"1\") ? 1 : 0;") >= 0
     && js.indexOf("calLast.lang === wantLang") >= 0);
  ok("R1.5.0 切换语言 / 样式的反馈：开着立刻重排、关着明说不动画面",
     js.indexOf("var faceSwitch = !!calLast && (calLast.lang !== wantLang || calLast.style !== wantStyle);") >= 0
     && js.indexOf("已按新语言 / 样式重排画面") >= 0
     && js.indexOf("语言 / 样式切换不会改动当前画面") >= 0);

  /* 四套版面（暖纸 / 红格 / 翻页牌 / 打卡点阵）：style 和 lang 同一条链路，
     回显 / 钳位 / 拦截 / 保存 / 核对都不能漏。注意 "A1" 在页面里已经是驱动模式名
     （a1Sel / A1MODE），日历样式只能叫 calStyle。 */
  sandbox.calTake({ on: 1, tod_s: 60, lang: 0, style: 1, synced: 1, y: 2026, m: 10, d: 5, wd: 1, next_in_s: 3600 });
  eq("回显：style=1 选中红格", el("calStyle").value + "/" + sandbox.calLast.style, "1/1");
  sandbox.calTake({ on: 1, tod_s: 60, style: 2, synced: 1, y: 2026, m: 10, d: 5, wd: 1, next_in_s: 3600 });
  eq("回显：style=2 选中翻页牌", el("calStyle").value + "/" + sandbox.calLast.style, "2/2");
  sandbox.calTake({ on: 1, tod_s: 60, style: 3, synced: 1, y: 2026, m: 10, d: 5, wd: 1, next_in_s: 3600 });
  eq("回显：style=3 选中打卡点阵", el("calStyle").value + "/" + sandbox.calLast.style, "3/3");
  /* 越界 / 缺字段的归一必须和固件同一口径（越界归 0），否则页面存进去了、设备却是另一套版面。 */
  sandbox.calTake({ on: 1, tod_s: 60, style: 7, synced: 1, y: 2026, m: 10, d: 5, wd: 1, next_in_s: 3600 });
  eq("越界钳位：style=7 归 0（与固件一致）", el("calStyle").value + "/" + sandbox.calLast.style, "0/0");
  el("calStyle").value = "3";
  sandbox.calTake({ on: 1, tod_s: 60, synced: 1, y: 2026, m: 10, d: 5, wd: 1, next_in_s: 3600 });
  ok("旧固件应答不含 style：不动用户已选的下拉，calLast 归 0（下次保存会补发一次）",
     el("calStyle").value === "3" && sandbox.calLast.style === 0);
  ok("R1.5.0 样式下拉：label for + 暖纸/红格/翻页牌/打卡点阵四个选项 + 竖版说明",
     page.includes('<label for="calStyle">日历样式</label>')
     && page.includes('<select id="calStyle"><option value="0">暖纸（淡黄块 + 无框月格）</option>'
                    + '<option value="1">红格（大红字 + 表格）</option>'
                    + '<option value="2">翻页牌（黑瓷砖 + 竖列月格）</option>'
                    + '<option value="3">打卡点阵（每行 5 个日期圆点）</option></select>')
     && page.includes("四套都是竖版；日历开着时切换会立刻按新样式重排当前画面"));
  ok("R1.5.0 样式走 0..3 整数口径（不再用 === \"1\" 二值判断）+ 进无变化判定与回读核对",
     js.indexOf("function calStyleNum(v){ var n = Math.trunc(+v || 0); return (n >= 0 && n <= 3) ? n : 0; }") >= 0
     && js.indexOf("var wantStyle = calStyleNum($(\"calStyle\").value);") >= 0
     && js.indexOf("calLast.style === wantStyle") >= 0
     && js.indexOf("calStyleNum(j.style) !== wantStyle") >= 0
     && js.indexOf("var wantStyle = ($") < 0);
})();

// 15. R1.5.0 功能3：待办清单 —— 设备存文档原文、页面画版式（真源在设备 NVS）
(function(){
  const start = js.indexOf("var TODO_MAX_ITEMS = 12;");
  const end   = js.indexOf('$("todoAdd").addEventListener');
  if (start < 0 || end < 0 || end <= start){ ok("R1.5.0 待办函数块可抽取", false); return; }

  function mkEl(){
    const o = { textContent: "", value: "", checked: false, style: {}, className: "",
                type: "", title: "", disabled: false, children: [], attrs: {}, kind: "",
                addEventListener: function(){},
                appendChild: function(c){ this.children.push(c); },
                setAttribute: function(k, v){ this.attrs[k] = v; } };
    Object.defineProperty(o, "innerHTML", {
      get: function(){ return this._html; },
      set: function(v){ this.children.length = 0; this._html = v; }
    });
    o._html = "";
    return o;
  }
  const nodes = {};
  function el(id){ return nodes[id] || (nodes[id] = mkEl()); }
  function pill(id){ return el(id).kind + "|" + el(id).textContent; }
  function rowsOf(){
    return el("todoList").children.map(function(c){
      return { cls: c.className, cb: c.children[0], no: c.children[1].textContent,
               txt: c.children[2].textContent, up: c.children[3], dn: c.children[4],
               del: c.children[5], n: c.children.length };
    });
  }

  /* 同步 thenable：让「POST → 回读核对 → 上屏」整条链在断言里当场跑完，
     否则 ok 药丸的文案要等微任务，最后的汇总行会先打印。 */
  function T(st){
    return {
      _st: st,
      then: function(f){
        if (!st.ok) return T(st);
        if (!f) return T(st);
        try { const r = f(st.val); return (r && r._st) ? r : T({ ok: true, val: r }); }
        catch(e){ return T({ ok: false, err: e }); }
      },
      catch: function(f){
        if (st.ok) return T(st);
        if (!f) return T(st);
        try { const r = f(st.err); return (r && r._st) ? r : T({ ok: true, val: r }); }
        catch(e){ return T({ ok: false, err: e }); }
      }
    };
  }
  function resp(status, body){
    return { status: status, ok: status >= 200 && status < 300,
             text: function(){ return T({ ok: true, val: body }); } };
  }
  const posts = [];
  const device = { reply: "echo" };
  let askYes = true;
  const sandbox = {
    $: el, console: console, Date: Date, TextEncoder: globalThis.TextEncoder,
    setTimeout: function(){ return 1; }, clearTimeout: function(){},
    PAL: [[0,0,0],[255,255,255],[255,255,0],[255,0,0]],
    FONTS: [{ w: 400, css: "sans-serif" }], EW: 552, EH: 768,
    LOCAL_PREVIEW: false,
    api: function(p){ return p; },
    confirm: function(){ return askYes; },
    saveState: function(id, kind, text){ el(id).kind = kind; el(id).textContent = text; },
    document: { createElement: function(){ return mkEl(); } },
    fetch: function(url, opt){
      posts.push({ url: url, body: opt && opt.body });
      if (device.reply === "404") return T({ ok: true, val: resp(404, "") });
      if (device.reply === "500") return T({ ok: true, val: resp(500, "doc too large") });
      if (device.reply === "mismatch")
        return T({ ok: true, val: resp(200, opt.body.replace('"items"', '"itemz"')) });
      return T({ ok: true, val: resp(200, opt.body) });
    }
  };
  sandbox.globalThis = sandbox;
  vm.runInNewContext(js.slice(start, end), sandbox, { filename: "todo-block.js" });
  ok("R1.5.0 待办函数块可抽取", true);

  /* ---------- 文档归一：必须和固件同一口径 ---------- */
  eq("空文档骨架 = 固件 TODO_EMPTY", sandbox.todoText(sandbox.todoNorm(null)),
     '{"v":1,"style":0,"lang":0,"push":1,"done":[],"items":[]}');
  ok("骨架满足固件的三道字节闸门（首 { 尾 } 且含 \"items\" 键）",
     (function(){ const s = sandbox.todoText(sandbox.todoNorm(null));
       return s[0] === "{" && s[s.length - 1] === "}" && s.indexOf('"items"') >= 0; })());

  const many = [], manyDone = [];
  for (var i = 0; i < 15; i++){ many.push("第" + (i + 1) + "条"); manyDone.push(1); }
  const capped = sandbox.todoNorm({ items: many, done: manyDone });
  eq("条数钳到 12，done 一起截", capped.items.length + "/" + capped.done.length, "12/12");
  eq("单条钳到 20 个码点（不是 20 字节）",
     Array.from(sandbox.todoNorm({ items: [new Array(22).join("汉")] }).items[0]).length, 20);
  eq("空白条目丢弃后 done 仍按原文对齐",
     (function(){ const o = sandbox.todoNorm({ items: ["  a   b ", "", null, "c"], done: [0,1,1,1] });
       return o.items.join("|") + "/" + o.done.join(""); })(), "a b|c/01");
  eq("样式 / 语言 / 开关归一：越界归 0，push 只认 0",
     [sandbox.todoNorm({ items: ["x"], style: 2 }).style,
      sandbox.todoNorm({ items: ["x"], lang: "1" }).lang,
      sandbox.todoNorm({ items: ["x"], push: 0 }).push,
      sandbox.todoNorm({ items: ["x"] }).push].join("-"), "0-1-0-1");
  eq("todoText 键顺序固定（回读核对靠字节一致，不靠对象形状）",
     sandbox.todoText({ v: 9, style: 1, lang: 0, push: 0, done: [1], items: ["a"] }),
     '{"v":1,"style":1,"lang":0,"push":0,"done":[1],"items":["a"]}');
  eq("todoBytes 按 UTF-8 计数", sandbox.todoBytes("汉字"), 6);
  const worstTxt = sandbox.todoText({ style: 0, lang: 0, push: 1, done: [],
      items: [1,2,3,4,5,6,7,8,9,10,11,12].map(function(){ return new Array(21).join("汉"); }) });
  ok("最坏情况（12 条 × 20 汉字）仍在 1536 字节闸门内",
     sandbox.todoBytes(worstTxt) > 700 && sandbox.todoBytes(worstTxt) <= 1536);

  /* ---------- 牌头日期：与固件日历同一口径 ---------- */
  const now = new Date(), wd = now.getDay();
  const MON = ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];
  const WD  = ["Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"];
  const dcn = sandbox.todoDate(false), den = sandbox.todoDate(true);
  eq("中文牌头 = 数字月 + 日 + 星期X",
     dcn.date, (now.getMonth() + 1) + "月" + now.getDate() + "日 星期" + "日一二三四五六"[wd]);
  ok("中文月份绝不出现「十月」这类汉字月",
     !/[一二两三四五六七八九]月/.test(dcn.date + dcn.big));
  eq("大字版带年份（B 版牌头）", dcn.big.indexOf(now.getFullYear() + "年"), 0);
  eq("工作日 / 休息日按周一至周五", dcn.tag, (wd >= 1 && wd <= 5) ? "工作日" : "休息日");
  eq("英文牌头是同一天（一套版面只说一种语言）",
     den.date, MON[now.getMonth()] + " " + now.getDate() + ", " + WD[wd]);

  /* ---------- 行计算 ---------- */
  eq("当前项 = 第一条未完成", sandbox.todoRows({ items: ["a","b","c"], done: [1,0,0] }).cur, 1);
  eq("完成数统计", sandbox.todoRows({ items: ["a","b","c"], done: [1,0,1] }).dn, 2);
  eq("全勾完就没有当前项", sandbox.todoRows({ items: ["a"], done: [1] }).cur, -1);
  eq("空清单不炸", [sandbox.todoRows({ items: [], done: [] }).n,
                    sandbox.todoRows({ items: [], done: [] }).cur].join("/"), "0/-1");

  /* ---------- 设备回读 → 控件回填（样式和 cal_style 一样存在设备）---------- */
  sandbox.todoTake('{"v":1,"style":1,"lang":1,"push":0,"done":[1,0],"items":["甲","乙"]}');
  eq("回读后下拉 / 勾选回填的是设备上的设置",
     [el("todoTpl").value, el("todoLang").value, el("todoPush").checked,
      sandbox.todoDoc.style].join("/"), "1/1/false/1");
  const r2 = rowsOf();
  eq("每条一行：勾选框 + 序号 + 文字 + 上移 / 下移 / 删除",
     r2.length + "/" + r2.map(function(x){ return x.n; }).join(","), "2/6,6");
  eq("已完成那条：勾上 + done 类 + 无障碍名写「取消完成」",
     r2[0].cls + "/" + r2[0].cb.checked + "/" + r2[0].cb.attrs["aria-label"],
     "tdItem done/true/取消完成：甲");
  ok("首条禁上移、末条禁下移（不给点了没反应的按钮）",
     r2[0].up.disabled === true && r2[0].dn.disabled === false
     && r2[1].dn.disabled === true && r2[1].up.disabled === false);
  eq("删除按钮有明确无障碍名", r2[1].del.attrs["aria-label"], "删除「乙」");

  /* ---------- 写入设备：先落盘、逐字节核对回读，才谈上屏 ---------- */
  sandbox.todoDoc = { style: 0, lang: 0, push: 0, done: [0], items: ["丙"] };
  sandbox.todoSaved = ""; device.reply = "echo"; posts.length = 0;
  sandbox.todoCommit(false);
  eq("一次改动 = 一份完整文档 POST（设备端零解析）",
     posts.length + " " + posts[0].url, "1 /api/todo");
  eq("发出去的就是 todoText 的原文", posts[0].body,
     '{"v":1,"style":0,"lang":0,"push":0,"done":[0],"items":["丙"]}');
  eq("落盘成功、开关关着 → 明说没上屏", pill("todomsg"), "ok|✓ 已保存到设备（未上屏）");
  sandbox.todoCommit(false);
  eq("内容没变就不重复写设备", posts.length, 1);

  device.reply = "mismatch";
  sandbox.todoDoc = { style: 0, lang: 0, push: 0, done: [0], items: ["丁"] };
  sandbox.todoCommit(false);
  eq("设备回读与提交不一致 → 警告而不是谎报成功", pill("todomsg"),
     "warn|⚠ 已提交，但设备回读不一致 —— 刷新页面核对");

  device.reply = "404";
  sandbox.todoSaved = ""; sandbox.todoDoc = { style:0, lang:0, push:0, done:[0], items:["戊"] };
  sandbox.todoCommit(false);
  eq("旧固件没有该路由 → 说清要升级到 R1.5.0+", pill("todomsg"),
     "err|✗ 设备固件不含待办功能（需 R1.5.0+ 固件）");

  device.reply = "500";
  sandbox.todoSaved = ""; sandbox.todoDoc = { style:0, lang:0, push:0, done:[0], items:["己"] };
  sandbox.todoCommit(false);
  eq("固件超长拒收（doc too large）专译成存储问题", pill("todomsg"),
     "err|✗ 设备拒收：清单超出存储 1536 字节");

  const huge = { style: 0, lang: 0, push: 0, done: [], items: [] };
  for (var q = 0; q < 100; q++){ huge.items.push(new Array(21).join("汉")); huge.done.push(0); }
  sandbox.todoSaved = ""; sandbox.todoDoc = huge; const before = posts.length;
  sandbox.todoCommit(false);
  eq("页面侧闸门：超 1536 字节的文档不发（省一次注定失败的往返）", posts.length, before);
  ok("闸门文案带上实际字节数与上限",
     /字节，超出设备存储 1536 字节 —— 删几条或缩短句子/.test(el("todomsg").textContent));

  /* ---------- 勾选 / 排序 / 删除：改动整体写回，真源在设备 ---------- */
  device.reply = "echo"; posts.length = 0;
  sandbox.todoTake('{"v":1,"style":0,"lang":0,"push":0,"done":[0,0],"items":["取快递","倒垃圾"]}');
  sandbox.todoToggle(1, true);
  eq("勾选第 2 条 → done 变 [0,1] 并整体写回", JSON.parse(posts[0].body).done.join(","), "0,1");
  sandbox.todoMove(0, 1);
  eq("下移第 1 条：items 与 done 同步交换（勾跟着事项走）", posts[1].body,
     '{"v":1,"style":0,"lang":0,"push":0,"done":[1,0],"items":["倒垃圾","取快递"]}');
  askYes = false; sandbox.todoDel(0);
  eq("删除要确认，取消就不动设备", posts.length, 2);
  askYes = true; sandbox.todoDel(0);
  eq("确认后 items / done 一起变短",
     JSON.parse(posts[2].body).items.join(","), "取快递");

  /* ---------- 添加：一条一句，超字 / 满额当场拦住 ---------- */
  const n0 = sandbox.todoDoc.items.length;
  el("todoIn").value = "驿站取快递";
  sandbox.todoAdd();
  eq("添加后输入框清空并回执条数",
     el("todoIn").value + "|" + pill("todoaddmsg"),
     "|ok|✓ 已加入第 " + (n0 + 1) + " 条");
  el("todoIn").value = "   ";
  sandbox.todoAdd();
  eq("空白输入被拦", pill("todoaddmsg"), "warn|⚠ 先写点什么再添加");
  el("todoIn").value = new Array(22).join("汉");
  sandbox.todoAdd();
  eq("超 20 字当场拦住并报实际字数", el("todoaddmsg").textContent,
     "✗ 一条最多 20 个字（现在 21 个）");
  sandbox.todoDoc = { style: 0, lang: 0, push: 0, done: [], items: [] };
  for (var z = 0; z < 12; z++){ sandbox.todoDoc.items.push("x"); sandbox.todoDoc.done.push(0); }
  el("todoIn").value = "第13条";
  sandbox.todoAdd();
  eq("满 12 条时拒绝并给出出路", el("todoaddmsg").textContent,
     "✗ 已到上限 12 条：先勾掉或删掉几条");

  /* ---------- 空态 / 旧固件态 / 计数行 ---------- */
  sandbox.todoOff = false;
  sandbox.todoDoc = { style: 0, lang: 0, push: 1, done: [], items: [] };
  sandbox.todoRenderList();
  ok("空清单是给下一步的邀请，不是「暂无数据」",
     /还没有待办：上面输入一句，点「添加」/.test(el("todoList").innerHTML));
  sandbox.todoOff = true; sandbox.todoRenderList(); sandbox.todoSyncCount();
  ok("旧固件：条目区换成升级指引、计数行不瞎报",
     /此设备固件不含待办功能（需 R1.5.0 及以上）/.test(el("todoList").innerHTML)
     && el("todoCount").textContent === "—");
  sandbox.todoOff = false;
  sandbox.todoDoc = { style: 0, lang: 0, push: 1, done: [0, 1], items: ["甲", "乙"] };
  sandbox.todoSyncCount();
  eq("计数行报「条数 / 完成数 / 字节占用」", el("todoCount").textContent,
     "2 / 12 条 · 已完成 1 · 清单 " + sandbox.todoBytes(sandbox.todoText(sandbox.todoDoc))
     + " / 1536 字节");

  /* ---------- 与固件的跨文件一致性（闸门数字不许各说各话）---------- */
  const todoH = fs.readFileSync(path.join(__dirname, "..", "src", "todo.h"), "utf8");
  const todoC = fs.readFileSync(path.join(__dirname, "..", "src", "todo.c"), "utf8");
  eq("页面字节闸门 = 固件 TODO_DOC_MAX",
     String(sandbox.TODO_DOC_MAX), (todoH.match(/#define\s+TODO_DOC_MAX\s+(\d+)/) || [])[1]);
  eq("页面条数上限与固件头注释的 12 × 20 口径一致",
     sandbox.TODO_MAX_ITEMS + "x" + sandbox.TODO_MAX_CHARS, "12x20");
  ok("固件只认结构骨架、不解析内容（首尾花括号 + items 键 + 拒 NUL）",
     todoC.indexOf('strstr(buf, "\\"items\\"")') >= 0
     && todoC.indexOf("buf[0] == '{'") >= 0 && todoC.indexOf("buf[got - 1] == '}'") >= 0
     && todoC.indexOf("if (strlen(buf) != got) return 0;") >= 0);
  ok("固件先落 NVS 再换内存镜像（不会「回显成功、重启就丢」）",
     todoC.indexOf("nvs_set_str(s_nvs, NVS_KEY, buf)") < todoC.indexOf("memcpy(s_doc, buf, got + 1)"));
  const fwEmpty = ((todoC.match(/#define\s+TODO_EMPTY\s+(.*)/) || [])[1] || "")
    .replace(/^"|"$/g, "").replace(/\\"/g, '"');
  eq("固件开机回的空文档骨架 = 页面骨架（逐字节，否则第一次改动白写一趟）",
     fwEmpty, sandbox.todoText(sandbox.todoNorm(null)));

  /* ---------- 卡片结构 + 渲染链路 ---------- */
  ok("R1.5.0 待办卡控件齐备：输入 + 添加 + 版式 + 语言 + 勾选即上屏 + 预览画布 + 计数",
     page.includes('<section class="card pane" id="paneTodo" role="tabpanel" aria-labelledby="cvTabT">')
     && page.includes('<h2 style="margin-top:0">待办清单</h2>')
     && page.includes('id="todoIn" maxlength="20"')
     && page.includes('<button class="mini" id="todoAdd">添加</button>')
     && page.includes('<span><select id="todoTpl"><option value="0">A 横线条目表（黑牌 + 打勾删除线）</option><option value="1">B 红色时间轴（红脊 + 黄底当前项）</option></select></span>')
     && page.includes('<span><select id="todoLang"><option value="0">中文</option><option value="1">English</option></select></span>')
     && page.includes('<label for="todoPush">勾选即上屏</label>')
     && page.includes('<canvas id="todoPrev" width="552" height="768"')
     && page.includes('id="todoCount"'));
  ok("R1.5.0 三条契约文案：真源在设备 / 待办不进轮播 / 日期取手机今天",
     page.includes("清单存在<b>设备里</b>（不在手机浏览器）")
     && page.includes("待办<b>不参与</b>「相册轮播」")
     && page.includes("画面上的日期取<b>手机上</b>的今天")
     && page.includes("升级固件不会丢清单；恢复出厂或长按按键会连同清单一起清空"));
  ok("R1.5.0 条目行样式 + 触屏热区（44px 按钮 / 28px 勾选框）",
     page.includes("#todoList .tdItem{ display:flex;")
     && page.includes("#todoList .tdItem.done .tdTxt{ color:var(--mut); text-decoration:line-through; }")
     && page.includes("#todoList .tdBtn{ width:44px; height:44px; font-size:17px; }")
     && page.includes("#todoList .tdItem input[type=checkbox]{ width:28px; height:28px; }"));
  ok("R1.5.0 上屏复用传图链路（量化 → 旋转 → 打包 → 帧头 → POST /api/frame）",
     js.indexOf("frameWithHeader(packIdx(rotateCW(todoIdx(todoDoc), EW, EH)))") >= 0
     && js.indexOf('xhr.open("POST", api("/api/frame"));') >= 0);
  ok("R1.5.0 两套版式都是 552×768 竖版，A/B 由文档里的 style 决定",
     js.indexOf("function drawTodoA(ctx, doc, D){") >= 0
     && js.indexOf("function drawTodoB(ctx, doc, D){") >= 0
     && js.indexOf("if (doc.style === 1) drawTodoB(ctx, doc, todoDate(!!doc.lang));") >= 0
     && js.indexOf("var s = Math.min(EW / 552, EH / 768);") >= 0);
  ok("R1.5.0 预览就是量化后的上屏画面（不是草稿）",
     js.indexOf("idx[i] = snapIdx(d[i*4], d[i*4+1], d[i*4+2])") >= 0
     && js.indexOf("预览 = 量化后的上屏画面，不是草稿") >= 0);
  ok("R1.5.0 上屏文案诚实（约 10~20 秒 / 屏幕刷新中）",
     js.indexOf("上屏中（整屏刷新约 10~20 秒）…") >= 0
     && js.indexOf("✓ 已上屏（屏幕刷新中）") >= 0);
  ok("R1.5.0 先落盘再上屏（回读逐字节核对通过才发帧）",
     js.indexOf("if (t !== s){") >= 0 && js.indexOf("todoSaved = s;") < js.indexOf("todoPushOrIdle();"));
  ok("R1.5.0 真源在设备：只有未连设备的本地预览才写 localStorage",
     js.indexOf("localStorage.setItem(TODO_LS, s);") >= 0
     && js.indexOf('fetch(api("/api/todo?t=" + Date.now()))') >= 0);
  ok("R1.5.0 旧固件识别：/api/todo 404 → todoOff，按钮不再假装能用",
     js.indexOf("todoOff = (e && e.code === 404);") >= 0
     && js.indexOf('if (r.status === 404) throw { code: 404 };') >= 0
     && js.indexOf('if (todoOff){ saveState("todomsg", "err", "✗ 设备固件不含待办功能（需 R1.5.0+ 固件）"); return; }') >= 0);
  ok("R1.5.0 设备页启动即读设备清单", js.indexOf("todoRefresh();") >= 0);
  ok("R1.5.0 todoTake 吃的是设备原文字符串（必须先 parse，解析失败按空清单起步）",
     js.indexOf('if (typeof o === "string"){ try { o = JSON.parse(o); } catch(e){ o = null; } }') >= 0);
  /* 量宽必须在 textAlign 归 left 之后做：center / right 下 actualBoundingBoxLeft 与
     Right 互相抵消成 ~0，「按宽缩字号 + 截尾」会静默失效，长句子直接冲出画面。 */
  const tdInkSrc = js.slice(js.indexOf("function tdInk(ctx, txt, sz){"),
                            js.indexOf("function tdTextW("));
  ok("R1.5.0 tdInk 量宽前先把 textAlign 归 left（防 center / right 抵消墨迹宽度）",
     tdInkSrc.length > 40 && tdInkSrc.indexOf('ctx.textAlign = "left";') >= 0
     && tdInkSrc.indexOf("ctx.measureText(txt)") >= 0
     && tdInkSrc.indexOf('ctx.textAlign = "left";') < tdInkSrc.indexOf("ctx.measureText(txt)"));
  /* 行为回归：假 ctx 只在 textAlign=center 时把墨迹宽度「抵消」成 0 ——
     少了那句归 left，缩字号与截尾会静默失效（长句子冲出 552 右边）。 */
  const fakeCtx = { font: "", textAlign: "left",
    measureText: function(t){
      const n = Array.from(t).length, sz = parseFloat(/(\d+(?:\.\d+)?)px/.exec(this.font)[1]);
      const adv = n * sz, half = this.textAlign === "center";
      return { width: adv,
               actualBoundingBoxLeft: half ? adv / 2 : 0,
               actualBoundingBoxRight: half ? adv / 2 : adv,
               actualBoundingBoxAscent: sz * 0.8, actualBoundingBoxDescent: sz * 0.2 };
    } };
  const long20 = new Array(21).join("汉");
  fakeCtx.textAlign = "center";
  eq("刚画过居中文字后接着量宽：仍按真实宽度缩到地板",
     sandbox.tdFitW(fakeCtx, long20, 300, 36, 18, 2), 18);
  eq("缩到地板还放不下就砍尾（20 字 / 300 px / 18 号 → 留 16 字）",
     Array.from(sandbox.tdClip(fakeCtx, long20, 18, 300)).length, 16);
  /* 反过来：量宽归 left 不能把画字时的对齐冲掉（写在 fillText 参数里就会冲掉）。 */
  const tdTextSrc = js.slice(js.indexOf("function tdText(ctx, x, yMid, txt, sz, ci, align){"),
                             js.indexOf("function tdFitW("));
  ok("R1.5.0 tdText 先量墨迹、再设对齐、后落笔",
     tdTextSrc.indexOf("tdInk(ctx, txt, sz).mid") >= 0
     && tdTextSrc.indexOf("tdInk(ctx, txt, sz).mid") < tdTextSrc.indexOf('ctx.textAlign = align')
     && tdTextSrc.indexOf("ctx.fillText(txt, x, yMid + mid);") >= 0);
  const rec = { font: "", fillStyle: "", textAlign: "left", textBaseline: "", drawn: [],
    measureText: function(t){
      const sz = parseFloat(/(\d+(?:\.\d+)?)px/.exec(this.font)[1]);
      return { width: Array.from(t).length * sz, actualBoundingBoxLeft: 0,
               actualBoundingBoxRight: Array.from(t).length * sz,
               actualBoundingBoxAscent: sz * 0.7, actualBoundingBoxDescent: sz * 0.2 };
    },
    fillText: function(t, x, y){ this.drawn.push(this.textAlign + "@" + x + "," + y); } };
  sandbox.tdText(rec, 504, 64, "10月5日", 30, 2, "right");
  eq("右对齐的日期仍以 right 落笔（墨迹居中偏移照旧）", rec.drawn[0], "right@504,71.5");
  ok("R1.5.0 条数少时从上往下排（行距封顶），不把唯一一条顶到屏幕中间",
     js.indexOf("var top = 122, bottom = 672, pitch = Math.min((bottom - top) / r.n, 96);") >= 0
     && js.indexOf("var top = 150, bottom = 682, pitch = Math.min((bottom - top) / r.n, 104);") >= 0);
  ok("R1.5.0 B 的黄底 / 序号块跟着行高与字号收，一条时不糊满屏、12 条时不互相压",
     js.indexOf("var span = pitch * r.n, bandH = Math.min(pitch * 0.88, tsz * 1.7) | 0;") >= 0
     && js.indexOf("var nb = Math.min(52, pitch * 0.9) | 0;") >= 0
     && js.indexOf("tdFillRR(ctx, 60, top - 14, 16, span + 28, 8, TD_R);") >= 0);
  ok("R1.5.0 回车即添加（手机打字不必去找按钮）",
     js.indexOf('if (e.key === "Enter" || e.keyCode === 13){ e.preventDefault(); todoAdd(); }') >= 0);
})();

// 16. R1.5.0 首页三向内容 tab（拍立得 / 待办 / 轮播）+ 便签模式下线
ok("R1.5.0 首页 tab 条：三个 tab、role/aria 齐备、默认停在拍立得",
   page.includes('<div id="cvtabs" role="tablist" aria-label="画面内容">')
   && page.includes('<button type="button" class="cvTab on" id="cvTabP" role="tab" aria-selected="true" aria-controls="panePolaroid">拍立得</button>')
   && page.includes('<button type="button" class="cvTab" id="cvTabT" role="tab" aria-selected="false" aria-controls="paneTodo">待办</button>')
   && page.includes('<button type="button" class="cvTab" id="cvTabC" role="tab" aria-selected="false" aria-controls="paneCar">轮播</button>'));
ok("R1.5.0 三个 pane 都在首页（tab-image）里，且只有拍立得默认可见",
   page.indexOf('<section class="card pane on" id="panePolaroid"') >= 0
   && page.indexOf('<section class="card pane" id="paneTodo"') >= 0
   && page.indexOf('<section class="card pane" id="paneCar"') >= 0
   && page.indexOf('id="paneTodo"') < page.indexOf('<div id="tab-device"')
   && page.indexOf('id="paneCar"') < page.indexOf('<div id="tab-device"'));
ok("R1.5.0 pane 显隐用 class（.pane / .pane.on），不靠 hidden 属性打架",
   page.includes(".pane{ display:none; }") && page.includes(".pane.on{ display:block; }"));
ok("R1.5.0 showPane：切 tab 同步 class + aria-selected，记住上次停留处",
   js.indexOf("var PANES = [[\"polaroid\", \"cvTabP\", \"panePolaroid\"],") >= 0
   && js.indexOf('cfg.contentTab = key; saveCfg();') >= 0
   && js.indexOf('b.setAttribute("aria-selected", on ? "true" : "false");') >= 0
   && js.indexOf('showPane(cfg.contentTab || "polaroid", true);') >= 0);
ok("R1.5.0 切到轮播重读设备、切到待办重排预览、切回拍立得重排文字覆盖层",
   js.indexOf('if (key === "polaroid"){ layoutObjs(); syncPrevHint(); }') >= 0
   && js.indexOf('if (key === "todo" && !silent) todoSchedule();') >= 0
   && js.indexOf('if (key === "car"  && !silent) carRefresh();') >= 0);
ok("R1.5.0 便签模式彻底下线：无 #modes / MODE / setMode / objsNote 绘制分支",
   page.indexOf('id="modes"') < 0 && js.indexOf("var MODE") < 0
   && js.indexOf("function setMode(") < 0 && js.indexOf("cfg.objsNote = normObjs") < 0
   && js.indexOf("(MODE === 0)") < 0 && js.indexOf("MODE !== 0") < 0
   && page.indexOf("便签</button>") < 0);
ok("R1.5.0 旧便签文字并进拍立得（不静默吃字），lastMode / textBg 一并清掉",
   js.indexOf("if (!Array.isArray(cfg.objsImg) && Array.isArray(cfg.objsNote)){") >= 0
   && js.indexOf("cfg.objsImg = cfg.objsNote;") >= 0
   && js.indexOf("delete cfg.objsNote; delete cfg.lastMode; delete cfg.textBg;") >= 0);
ok("R1.5.0 拍立得加字功能不变：勾选「添加文字」仍是唯一开关",
   page.includes('<div class="row" id="textOnRow">')
   && js.indexOf('function refreshTextUI(){\n  $("textPanel").style.display = cfg.textOn ? "" : "none";') >= 0
   && js.indexOf('function curObjs(){ return cfg.objsImg; }') >= 0
   && js.indexOf('var objs = cfg.textOn ? cfg.objsImg : [];') >= 0
   && js.indexOf('drawObjs(ctx, cfg.textOn ? curObjs() : [], prevEl.width, prevEl.height);') >= 0);
ok("R1.5.0 轮播 / 待办不再藏在设置页的折叠里（carBox / todoBox 已移出）",
   page.indexOf('id="carBox"') < 0 && page.indexOf('id="todoBox"') < 0
   && page.indexOf("<summary>相册轮播</summary>") < 0
   && page.indexOf("<summary>待办清单上屏</summary>") < 0);
ok("R1.5.0 日历仍留在设置页（首页只放三个内容 tab）",
   page.indexOf('<details id="calBox">') >= 0
   && page.indexOf('<details id="calBox">') > page.indexOf('<div id="tab-device"'));
ok("R1.5.0 轮播取图的提示改口到「拍立得」页",
   js.indexOf('carPill("err", "✗ 请先在「拍立得」页选择图片")') >= 0);

/* ---------- R1.5.2：自动校时（tz + SNTP 回显）+ 日历/轮播互斥 ---------- */
ok("R1.5.2 推日期时把手机时区一起送给设备（tz_min = -getTimezoneOffset）",
   js.indexOf('body: "t=" + calWallPack() + "&tz_min=" + (-new Date().getTimezoneOffset())') >= 0);
ok("R1.5.2 同步成功的药丸里回显已落盘的时区（旧固件无 tz_min 则不说）",
   js.indexOf('(j.tz_min == null ? "" : " · 时区 " + fmtTz(j.tz_min) + " 已写入（自动校时用）")') >= 0);
ok("R1.5.2 fmtTz：东正西负 + 支持半小时偏移",
   js.indexOf("function fmtTz(min){") >= 0
   && js.indexOf('return "UTC" + (m < 0 ? "-" : "+") + Math.floor(a / 60)') >= 0);
ok("R1.5.2 新增「自动校时」回显行 #calSntp，并由 calShowState 三态填写",
   page.includes('id="calSntp"')
   && js.indexOf('var now = $("calNow"), next = $("calNext"), sn = $("calSntp");') >= 0
   && js.indexOf('sn.textContent = tz + " · 日历关着时不自动校时"') >= 0
   && js.indexOf('sn.textContent = tz + " · 尚未自动校时（下次醒来时补）"') >= 0
   && js.indexOf('"前自动校过时"') >= 0);
ok("R1.5.2 旧固件（应答无 tz_min/sntp_at）不说「尚未自动校时」，改说需 R1.5.2",
   js.indexOf('if (j.tz_min == null && j.sntp_at == null) sn.textContent = "此设备固件不含自动校时（需 R1.5.2 及以上）";') >= 0);
ok("R1.5.2 校时回显在读取失败 / 本地预览两条路上一起复位",
   js.indexOf('if (sn) sn.textContent = "本地预览（未连设备）";') >= 0
   && js.indexOf('if (sn) sn.textContent = "—";') >= 0);
ok("R1.5.2 日历保存：设备回 car_off 才算真关掉轮播，页面跟着重读轮播状态",
   js.indexOf('if (j.car_off) carRefresh();') >= 0
   && js.indexOf('(j.car_off ? " · 相册轮播已自动关闭（这块屏交给日历）" : "")') >= 0);
ok("R1.5.2 日历刚被打开时说明「今天这张正在上屏」，不再让用户自己点「立即显示今天」",
   js.indexOf("var turnedOn = !!wantOn && !!calLast && !calLast.on;") >= 0
   && js.indexOf('(turnedOn ? " · 今天这张正在上屏（约 15~25 秒）"') >= 0);
ok("R1.5.2 轮播保存：留住 cfg 应答里的 cal_off（list 回读不带它），并回显 + 重读日历",
   js.indexOf("j.cal_off = (c && c.cal_off) ? 1 : 0;") >= 0
   && js.indexOf('var mutex = j.cal_off ? " · 日历已自动关闭（同一块屏只让一个主人用）" : "";') >= 0
   && js.indexOf("if (j.cal_off) calRefresh();") >= 0);
ok("R1.5.2 互斥落地：旧的「两者会互相盖屏 / 建议二选一」文案全部删除",
   page.indexOf("互相盖") < 0 && page.indexOf("建议二选一") < 0);
ok("R1.5.2 两处提示各自说明互斥由设备自动完成",
   page.indexOf("<b>开一个，另一个自动被关掉</b>") >= 0
   && page.indexOf("<b>打开轮播，日历会被设备自动关掉</b>") >= 0);
ok("R1.5.2 提示讲清自动校时链路（NTP 服务器 + 12 小时闸 + UDP 123）",
   page.indexOf("ntp.aliyun.com") >= 0 && page.indexOf("cn.pool.ntp.org") >= 0
   && page.indexOf("距上次校时满 12 小时") >= 0 && page.indexOf("UDP 123") >= 0);
ok("R1.5.2 掉电未同步的警告改口：自己会校，不必手动",
   js.indexOf("开着「启用联网」时它下次醒来也会自己校时") >= 0);
ok("R1.5.3 未同步那行改口成退避重试（转述 next_in_s + ntp_try）",
   js.indexOf("后自己再试校时") >= 0 && js.indexOf("j.ntp_try") >= 0
   && js.indexOf("已连续失败") >= 0);
ok("R1.5.3 旧固件降级：没有 ntp_try 字段时不谎报排期",
   js.indexOf('if (j.ntp_try == null) next.textContent = "⚠ 日期没同步时不会自动改日期"') >= 0);
ok("R1.5.3 没配联网时说明白「不会自动改日期」",
   js.indexOf("没配「启用联网」，不会自动改日期") >= 0);
ok("R1.5.3 hint 讲清退避阶梯 + 开机强制回屏的理由",
   page.indexOf("1→2→4→8→12 小时退避") >= 0
   && page.indexOf("强制把当天日历回屏一次") >= 0);
/* ---------- R1.5.4：OTA 回滚可见 + 帧缓冲读数（页面侧）---------- */
eq("R1.5.4 otaNote 旧固件无 ota_run 时不渲染", String(sandbox.otaNote({ heap: 1 })), "null");
var oAb = sandbox.otaNote({ ota_run:"ota_0", ota_state:"VALID",
                            ota_other:"ota_1", ota_other_state:"ABORTED" });
ok("R1.5.4 otaNote 另一槽 ABORTED → err，点名被回滚的槽与现跑槽",
   oAb.kind === "err" && oAb.text.indexOf("ota_1") >= 0
   && oAb.text.indexOf("已被自动退回") >= 0 && oAb.text.indexOf("ota_0") >= 0);
ok("R1.5.4 otaNote INVALID 与 ABORTED 同口径",
   sandbox.otaNote({ ota_run:"ota_0", ota_other_state:"INVALID" }).kind === "err");
var oPend = sandbox.otaNote({ ota_run:"ota_1", ota_state:"PENDING" });
ok("R1.5.4 otaNote 自证窗口内 → warn + 别断电",
   oPend.kind === "warn" && oPend.text.indexOf("别断电") >= 0);
ok("R1.5.4 otaNote NEW 也算未确认",
   sandbox.otaNote({ ota_run:"ota_1", ota_state:"NEW" }).kind === "warn");
var oValid = sandbox.otaNote({ ota_run:"ota_0", ota_state:"VALID",
                               ota_other:"ota_1", ota_other_state:"UNDEFINED" });
ok("R1.5.4 otaNote 正常态 → ok 且不带告警字样",
   oValid.kind === "ok" && oValid.text.indexOf("不会被回滚") >= 0);
ok("R1.5.4 帧缓冲读数上屏（#i-fb + heap_max 进 title + fb=0 红字）",
   page.includes('id="i-fb"') && js.indexOf('$("i-fb")') >= 0
   && js.indexOf("j.heap_max") >= 0 && js.indexOf("if (j.fb > 0)") >= 0
   && js.indexOf('j.fb === 0') >= 0);
ok("R1.5.4 升级状态药丸接线（#otaState，样式先复位再按 kind 上色）",
   page.includes('id="otaState"') && js.indexOf('oEl.className = "savestate"') >= 0
   && js.indexOf("oEl.classList.add(oNote.kind)") >= 0);
ok("R1.5.4 旧固件无 ota 字段时药丸显示「—」并注明，而不是留空",
   js.indexOf('oEl.textContent = oNote ? oNote.text : "\u2014"') >= 0
   && js.indexOf("旧固件不上报这一项") >= 0);
ok("R1.5.4 维护区讲清自证窗口 + 回滚不报错这件事",
   page.indexOf("12 秒") >= 0 && page.indexOf("<b>不报错</b>") >= 0
   && page.indexOf("请重新升级") >= 0);

/* ---------- R1.5.5：日历卡片补读（页面侧唯一的日历假阴性修补）---------- */
ok("R1.5.5 开页只补读一次，治 sntp_at 单调量造成的「尚未自动校时」假阴性",
   js.indexOf("var calReread = false;") >= 0
   && js.indexOf("if (!calReread && j.on && !(+j.sntp_at > 0)) {") >= 0
   && js.indexOf("calReread = true; setTimeout(calRefresh, 12000);") >= 0);
ok("R1.5.5 补读排在「未同步就补推一次」之前，且写明不许轮询（每次请求都喂 power_activity）",
   js.indexOf("calReread = true; setTimeout(calRefresh, 12000);")
   < js.indexOf("if (!j.synced) return calPush(true);")
   && js.indexOf("轮询会把深睡彻底废掉") >= 0);

ok("R1.5.7 版本号两处一致（页面 meta 与断言基线）",
   page.includes('<meta name="moink-page-version" content="R1.5.7">'));

/* ---------- R1.5.6：换图间隔下限 5 分钟（页面前置拦截 + 文案）---------- */
ok("R1.5.6 换图间隔下限常量 = 5 分钟，并写明对应固件 CAR_INT_MIN_S 300 秒",
   js.indexOf("var CAR_MIN = 5;") >= 0
   && page.indexOf("下限 5 分钟（R1.5.6，= 固件 CAR_INT_MIN_S 300 秒）") >= 0);
ok("R1.5.6 保存前就地挡 1~4 分钟（0 仍放行 = 不自动换图），不再等固件回 400",
   js.indexOf("if (wantInt > 0 && wantInt < CAR_MIN * 60){") >= 0
   && js.indexOf('saveState("carcfgmsg", "err", "\u2717 换图间隔最短 " + CAR_MIN + " 分钟（0 = 不自动换图）")') >= 0);
ok("R1.5.6 守卫排在「与设备当前一致就不发请求」之前，坏值不会先撞上 no changes 误译",
   js.indexOf("if (wantInt > 0 && wantInt < CAR_MIN * 60){")
   < js.indexOf("if (carLast && carLast.on === !!wantOn && carLast.mode === wantMode && carLast.int_s === wantInt){"));
ok("R1.5.6 输入框旁明示最短 5 分钟",
   page.indexOf("0 = 不自动换图，最短 5 分钟") >= 0);

/* ---------- R1.5.7：换完图即回睡（省电），页面必须把「别开着页面等换图」讲清 ---------- */
ok("R1.5.7 轮播卡明示换完图约 15 秒回深睡、别开着页面等换图、如何再唤醒",
   page.indexOf("换完图后设备约 15 秒内就回深睡") >= 0
   && page.indexOf("别把本页一直开着等它换图") >= 0
   && page.indexOf("按一下机身按键或重新上电即可唤醒") >= 0);
ok("R1.5.7 该提示排在轮播卡内、日历互斥那条之前（先讲行为再讲互斥）",
   page.indexOf("换完图后设备约 15 秒内就回深睡")
   < page.indexOf("轮播与「日历自动改日期」用同一块屏"));

ok("R1.5.5 校时机制长文收进折叠块，常驻只留一句「不必手动同步」",
   page.indexOf("<summary>校时的原理与边界（一般不用读）</summary>") >= 0
   && page.indexOf("开机、到点自醒都是先校时、再决定要不要换日") >= 0
   && page.indexOf("ntp.aliyun.com") > page.indexOf("校时的原理与边界")
   && page.indexOf("1→2→4→8→12 小时退避") > page.indexOf("校时的原理与边界"));

console.log(failed === 0 ? "SMOKE: ALL PASS" : ("SMOKE: " + failed + " FAILED"));
process.exit(failed === 0 ? 0 : 1);
