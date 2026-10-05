#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
日历版面主机预览（不参与固件构建）：把 cal_preview.c 吐出的 2bpp 裸帧还原成 PNG。

    cc -Isrc -o /tmp/cal_preview tools/cal_preview.c src/cal_face.c src/cal_font.c
    python3 tools/cal_preview.py /tmp/cal_preview

解码口径与页面 packIdx / epd_drv.c 完全一致：
    像素 (x,y) 在 fb[(H-1-y)*192 + (191-(x>>2))] 的第 ((x&3)<<1) 位，色码 0K 1W 2Y 3R。
每个日期出「中英 × 暖纸/红格」四组，每组三张：设备横屏原样、竖屏读法（用户实际看到的）、
以及竖屏缩小 1/3（模拟观看距离下奶黄斜纹的混色效果）。
"""
import datetime
import os
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 768, 552
RB = W // 4
PAL = [(0, 0, 0), (255, 255, 255), (255, 255, 0), (255, 0, 0)]

CASES = [
    (2026, 10, 5, 1),    # 国庆假期内 + 月格从周四开始（周一打头 = 第 4 列）
    (2026, 2, 1, 0),     # 周日开头（周一打头 = 最后一列）+ 平年 2 月 28 天
    (2026, 11, 1, 0),    # 6 行满行（贴到最底）
    (2028, 2, 29, 1),    # 闰日
]
LANGS = [(0, "zh"), (1, "en")]
STYLES = [(0, "warm"), (1, "grid"), (2, "flap"), (3, "punch")]


def decode(raw):
    img = Image.new("RGB", (W, H))
    out = img.load()
    for y in range(H):
        base = (H - 1 - y) * RB
        for xb in range(RB):
            b = raw[base + (RB - 1 - xb)]
            for q in range(4):
                out[xb * 4 + q, y] = PAL[(b >> (q * 2)) & 3]
    return img


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "/tmp/cal_preview"
    # 预览图落在 NAS 根的 spark-output/，不进 moink 的 git 树（发版标签保持干净）
    outdir = os.path.join(ROOT, "..", "spark-output", "cal-preview")
    os.makedirs(outdir, exist_ok=True)
    for (yy, mm, dd, _) in CASES:
        wd = datetime.date(yy, mm, dd).weekday()          # 0 = 周一
        wday = (wd + 1) % 7                               # 转成 gmtime 口径：0 = 周日
        for lang, ltag in LANGS:
            for style, stag in STYLES:
                name = "%04d-%02d-%02d_%s_%s" % (yy, mm, dd, ltag, stag)
                raw_path = "/tmp/cal_%s_%s.raw" % (name, "raw")
                subprocess.run([exe, str(yy), str(mm), str(dd), str(wday), raw_path,
                                str(lang), str(style)], check=True)
                with open(raw_path, "rb") as f:
                    img = decode(f.read())
                img.save(os.path.join(outdir, name + "_device.png"))
                port = img.transpose(Image.Transpose.ROTATE_90)
                port.save(os.path.join(outdir, name + "_portrait.png"))
                port.resize((port.width // 3, port.height // 3), Image.BOX).save(
                    os.path.join(outdir, name + "_far.png"))
        print("wrote", "%04d-%02d-%02d" % (yy, mm, dd), "zh/en x warm/grid/flap/punch")
    print("->", outdir)


if __name__ == "__main__":
    main()
