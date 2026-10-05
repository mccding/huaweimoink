#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
墨印 · MoInk —— 日历字模生成器（R1.5.0 功能1）

把 macOS 系统字体里的字形烘成 1bpp 点阵表，输出 src/cal_font.h / src/cal_font.c。
设备端没有字体栅格化能力（也没有 RAM 放轮廓），日历版式只能靠这张表；
改字号 / 改字集 = 重跑本脚本 + 重编固件。

    python3 tools/make_cal_font.py          # 必须系统 python3（.pio-venv 里没有 PIL）

表清单见下面 SETS。两类排版：
    定宽居中（digits 大字 / 汉字）  —— 字形在自己的 WxH 盒里水平垂直双居中；
    比例 + 基线（拉丁 l48 / l32 / l24 / d32）—— 额外带一张 adv[]（推进宽度），字形按
        **共同基线** 定位。拉丁若按定宽盒垂直居中，"TUESDAY" 里的小写会各自浮到
        半高、基线全乱，所以拉丁必须走这条。
"""
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("需要 Pillow：请用系统 python3 运行本脚本")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NEUE = "/System/Library/Fonts/HelveticaNeue.ttc"
LATIN = (NEUE, 1)          # Helvetica Neue / Bold
LATIN_MED = (NEUE, 10)     # Helvetica Neue / Medium —— 月格数字比标题细一档
CJK = ("/System/Library/Fonts/Hiragino Sans GB.ttc", 2)  # 冬青黑体简体中文 W6

THRESHOLD = 110   # 略低于 128：笔画多吃一点墨，2bpp 无灰度时小字号不断笔

# 拉丁字集：大小写 + 数字 + 空格 + 间隔号 + 斜杠（A1 版式的 "2026 / OCTOBER"）
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ·/"

SETS = [
    # 大字日期：B1（暖纸）用 120 档，A1（红格）用 180 档 —— 两张都是 10 个数字
    dict(name="b",     w=80,  h=120, ink=100, digits=True, chars="0123456789", font=LATIN),
    dict(name="b180",  w=124, h=180, ink=150, digits=True, chars="0123456789", font=LATIN),
    # C/D 之间还差一档：D「打卡点阵」的悬垂红日期只有牌与框之间 82 px 可站
    dict(name="b64",   w=52,  h=76,  ink=64,  digits=True, chars="0123456789", font=LATIN),
    # 汉字：只有「年月日 + 星期 + 一二三四五六」真的会上屏（落款 / 季节行已删）；
    # 后 7 个是 D「打卡点阵」的图例（已过 / 今天 / 节日 / 未至），公用同一档 32 px。
    dict(name="c32", w=32, h=32, ink=30, digits=False,
         chars="年月日星期一二三四五六已过今天节未至", font=CJK),
    # 拉丁：48 档给月份 / 星期，32 档给顶行小字 / 月格表头
    dict(name="l48", w=48, h=48, baseline=36, latin=True, chars=ALPHA, font=LATIN),
    dict(name="l32", w=32, h=32, baseline=24, latin=True, chars=ALPHA, font=LATIN),
    # 24 档只为 D 的英文图例：四格 PAST/TODAY/HOLIDAY/NEXT 在 32 档下撑破圆角框
    dict(name="l24", w=24, h=24, baseline=18, latin=True, chars=ALPHA, font=LATIN),
    # 月格日期：Medium 字重（比标题细一档），比例宽度
    dict(name="d32", w=32, h=32, baseline=24, latin=True, digits_only=True,
         chars="0123456789", font=LATIN_MED),
]


def _font(path, idx, size):
    return ImageFont.truetype(path, size, index=idx)


def raster_mono(ch, path, idx, box_w, box_h, ink_h):
    """定宽档：按字号渲染、裁到墨迹外框，再在 box 里水平垂直双居中。"""
    def ink(size):
        font = _font(path, idx, size)
        side = max(box_w, box_h) * 4
        img = Image.new("L", (side, side), 0)
        ImageDraw.Draw(img).text((side // 2, side // 2), ch, font=font, fill=255, anchor="mm")
        bb = img.getbbox()
        if not bb:
            return None
        crop = img.crop(bb)
        if crop.height > ink_h or crop.width > box_w:
            return None
        return crop

    lo, hi, best = 1, ink_h * 3, None
    while lo <= hi:
        mid = (lo + hi) // 2
        got = ink(mid)
        if got is None:
            hi = mid - 1
        else:
            best, lo = got, mid + 1
    if best is None:
        sys.exit("字模渲染失败：%r @ %dx%d" % (ch, box_w, box_h))

    stride = (box_w + 7) // 8
    out = bytearray(stride * box_h)
    px = best.load()
    bw, bh = best.size
    left, top = (box_w - bw) // 2, (box_h - bh) // 2
    for y in range(bh):
        for x in range(bw):
            if px[x, y] >= THRESHOLD:
                out[(y + top) * stride + ((x + left) >> 3)] |= 0x80 >> ((x + left) & 7)
    return bytes(out)


def _measure(ch, path, idx, size, baseline):
    """以基线为锚渲染，返回 (墨迹图, 墨迹左偏移(相对 pen), 墨迹顶(相对基线), 上升, 下降, 推进)。"""
    font = _font(path, idx, size)
    side = max(256, size * 6)
    img = Image.new("L", (side, side), 0)
    ImageDraw.Draw(img).text((side // 2, side // 2), ch, font=font, fill=255, anchor="ms")
    adv = font.getlength(ch)
    bb = img.getbbox()
    if not bb:
        return None, 0, 0, 0, 0, adv
    return (img.crop(bb), bb[0] - side // 2, bb[1] - side // 2,
            side // 2 - bb[1], bb[3] - side // 2, adv)


def _fits(ch, path, idx, size, box_w, box_h, baseline):
    crop, dx, dy, asc, desc, adv = _measure(ch, path, idx, size, baseline)
    return asc <= baseline and desc <= box_h - baseline and adv <= box_w


def fit_size(chars, path, idx, box_w, box_h, baseline):
    """整表一个字号：拉丁必须按同一 em 排，逐字二分会让小写（x-height）被放大到
       与全大写等高，"Autumn" 就变成假 small-caps。取所有字形的最大公共字号。"""
    lo, hi, best = 1, baseline * 6, 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if all(_fits(c, path, idx, mid, box_w, box_h, baseline) for c in chars):
            best, lo = mid, mid + 1
        else:
            hi = mid - 1
    return best


def raster_adv(ch, path, idx, box_w, box_h, baseline, size):
    """比例档：按整表共用的 size 渲染，墨迹按基线摆进 box（水平按 left side bearing，
       不居中）。返回 (点阵, adv)。"""
    crop, dx, dy, asc, desc, adv = _measure(ch, path, idx, size, baseline)

    stride = (box_w + 7) // 8
    out = bytearray(stride * box_h)
    if crop is not None:
        px = crop.load()
        bw, bh = crop.size
        left, top = dx, baseline + dy      # dy 是负数（墨迹顶在基线之上）
        if left < 0:
            left = 0
        for y in range(bh):
            row = top + y
            if row < 0 or row >= box_h:
                continue
            for x in range(bw):
                col = left + x
                if col < 0 or col >= box_w:
                    continue
                if px[x, y] >= THRESHOLD:
                    out[row * stride + (col >> 3)] |= 0x80 >> (col & 7)
    return bytes(out), max(1, min(box_w, int(round(adv))))


def rows_hex(bits, stride, box_h, indent):
    out = []
    for i in range(box_h):
        r = bits[i * stride:(i + 1) * stride]
        out.append(indent + ",".join("0x%02X" % b for b in r) + ",")
    return "\n".join(out)


def main():
    decls, defs = [], []
    total = 0
    for s in SETS:
        name, bw, bh = s["name"], s["w"], s["h"]
        stride = (bw + 7) // 8
        up = name.upper()
        chars = s["chars"]
        total += stride * bh * len(chars)
        decls += [
            "#define CAL_%s_W      %d" % (up, bw),
            "#define CAL_%s_H      %d" % (up, bh),
            "#define CAL_%s_STRIDE %d" % (up, stride),
        ]
        note = "// ---- %s：%s @ %dx%d ----" % (name, "".join(chars), bw, bh)

        if s.get("latin"):
            baseline = s["baseline"]
            decls.append("#define CAL_%s_BASELINE %d" % (up, baseline))
            size = fit_size(chars, *s["font"], bw, bh, baseline)
            note += "   整表字号 %d（基线 %d，下降余量 %d）" % (size, baseline, bh - baseline)
            pairs = [raster_adv(c, *s["font"], bw, bh, baseline, size) for c in chars]
            decls.append("/* 取字形点阵（盒内已按基线摆好）并给出本字推进宽度；缺字返回 NULL。 */")
            decls.append("const uint8_t *cal_font_%s_get(uint32_t cp, int *adv);" % name)
            cps = ", ".join("0x%X" % ord(c) for c in chars)
            advs = ", ".join(str(a) for _, a in pairs)
            body = ",\n".join("{\n%s\n  }" % rows_hex(b, stride, bh, "    ") for b, _ in pairs)
            defs.append(
                "%s\nstatic const uint32_t cal_font_%s_cps[%d] = { %s };\n"
                "static const uint8_t cal_font_%s_adv[%d] = { %s };\n"
                "static const uint8_t cal_font_%s_rows[%d][%d] = {\n  %s\n};\n"
                "const uint8_t *cal_font_%s_get(uint32_t cp, int *adv)\n{\n"
                "    for (unsigned i = 0; i < sizeof(cal_font_%s_cps) / sizeof(uint32_t); i++) {\n"
                "        if (cal_font_%s_cps[i] != cp) continue;\n"
                "        if (adv) *adv = cal_font_%s_adv[i];\n"
                "        return cal_font_%s_rows[i];\n"
                "    }\n"
                "    if (adv) *adv = 0;\n"
                "    return NULL;\n}"
                % (note, name, len(chars), cps, name, len(chars), advs,
                   name, len(chars), stride * bh, body, name, name, name, name, name))
        elif s["digits"]:
            bits = [raster_mono(c, *s["font"], bw, bh, s["ink"]) for c in "0123456789"]
            decls.append("extern const uint8_t cal_font_%s_digits[10][%d];" % (name, stride * bh))
            body = ",\n".join("{\n%s\n  }" % rows_hex(b, stride, bh, "    ") for b in bits)
            defs.append("%s\nconst uint8_t cal_font_%s_digits[10][%d] = {\n  %s\n};"
                        % (note, name, stride * bh, body))
        else:
            bits = [raster_mono(c, *s["font"], bw, bh, s["ink"]) for c in chars]
            decls.append("/* 按码点查表，缺字返回 NULL（调用方留空位，不会画花屏）。 */")
            decls.append("const uint8_t *cal_font_%s_get(uint32_t cp);" % name)
            cps = ", ".join("0x%X" % ord(c) for c in chars)
            body = ",\n".join("{\n%s\n  }" % rows_hex(b, stride, bh, "    ") for b in bits)
            defs.append(
                "%s\nstatic const uint32_t cal_font_%s_cps[%d] = { %s };\n"
                "static const uint8_t cal_font_%s_rows[%d][%d] = {\n  %s\n};\n"
                "const uint8_t *cal_font_%s_get(uint32_t cp)\n{\n"
                "    for (unsigned i = 0; i < sizeof(cal_font_%s_cps) / sizeof(uint32_t); i++)\n"
                "        if (cal_font_%s_cps[i] == cp) return cal_font_%s_rows[i];\n"
                "    return NULL;\n}"
                % (note, name, len(chars), cps, name, len(bits), stride * bh, body,
                   name, name, name, name))
        defs.append("")

    hdr = ("""/* 由 tools/make_cal_font.py 生成，勿手改（改字号 / 字集 = 重跑脚本 + 重编固件）。
 *
 * 点阵格式：1bpp、行主序、MSB 优先，每行 STRIDE 字节，右侧补 0。
 * 定宽档（b / b180 / b64 / c32）字形在 W x H 盒里水平垂直双居中，按盒子对齐即可；
 * 拉丁档（l48 / l32 / l24 / d32）按 **共同基线 BASELINE** 定位并带 per-glyph 推进宽度，
 * 取值用 cal_font_<name>_get(cp, &adv)，排版时 u += adv —— 别按定宽盒居中。
 * 拉丁 = Helvetica Neue Bold（d32 = Medium，月格数字细一档），汉字 = 冬青黑体 W6。
 */
#ifndef MOINK_CAL_FONT_H
#define MOINK_CAL_FONT_H

#include <stdint.h>

%s

#endif /* MOINK_CAL_FONT_H */
""" % "\n".join(decls))

    src = ("/* 由 tools/make_cal_font.py 生成，勿手改。 */\n"
           "#include <stddef.h>\n\n"
           "#include \"cal_font.h\"\n\n" + "\n".join(defs))

    open(os.path.join(ROOT, "src", "cal_font.h"), "w").write(hdr)
    open(os.path.join(ROOT, "src", "cal_font.c"), "w").write(src)
    n = sum(len(s["chars"]) for s in SETS)
    print("已生成 %d 个字模 -> src/cal_font.{h,c}（%d 字节点阵 = %.1f KB）"
          % (n, total, total / 1024.0))


if __name__ == "__main__":
    main()
