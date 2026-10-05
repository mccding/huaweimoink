#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
墨印 · MoInk —— 日历「新方向」概念图离屏渲染（视觉情绪板追加轮；不参与固件编译）

为什么不走 AI 出图：AI 只能给氛围图，字号 / 格线 / 色板全是假的，挑完还得反推。
本脚本按**真机口径**画：552×768 竖版画布、只有 K/W/Y/R 四色（无灰度无渐变）、
字体与 tools/make_cal_font.py 烘焙时用的同一批（Helvetica Neue Bold/Medium +
冬青黑体 Hiragino Sans GB W6）、节假日表与 src/cal_face.c 的 HOLS[] 同一份。
所以「挑中哪张，设备上就能长成哪张」这句话在这批图上是成立的。

    python3 tools/cal_concept_board.py [输出目录]
    # 默认 ../spark-output/board/moink-cal-v2-moodboard

产物：{代号}{1..3}.png（552×768 真实像素）+ {代号}{1..3}@2x.png（近邻放大，便于看细节）
"""
import calendar
import datetime
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

NEUE = "/System/Library/Fonts/HelveticaNeue.ttc"
HIRA = "/System/Library/Fonts/Hiragino Sans GB.ttc"

K, W, Y, R = (0, 0, 0), (255, 255, 255), (255, 255, 0), (255, 0, 0)
LW, LH, M = 552, 768, 14
CW = 74                                   # 月格列宽（与固件一致：7 × 74 = 518）
GX = (LW - 7 * CW) // 2                   # 月格左边界
MON = ["JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE","JULY","AUGUST",
       "SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"]
WD = ["MONDAY","TUESDAY","WEDNESDAY","THURSDAY","FRIDAY","SATURDAY","SUNDAY"]
WD1 = ["MON","TUE","WED","THU","FRI","SAT","SUN"]
HOLS = [(2026,1,1,1),(2026,2,15,23),(2026,4,4,6),(2026,5,1,5),(2026,6,19,21),
        (2026,9,25,27),(2026,10,1,7),(2027,1,1,1)]

DATES = [datetime.date(2026, 10, 5), datetime.date(2026, 2, 17), datetime.date(2026, 5, 1)]


def lat(sz, med=False):
    return ImageFont.truetype(NEUE, sz, index=10 if med else 1)


def cj(sz):
    return ImageFont.truetype(HIRA, sz, index=2)


def is_hol(d):
    return any(d.year == y and d.month == m and a <= d.day <= b for y, m, a, b in HOLS)


def cells(d):
    """(首列, 本月天数, 行数)；列 0 = 周一，与固件同口径。"""
    col0 = d.replace(day=1).weekday()
    dim = calendar.monthrange(d.year, d.month)[1]
    return col0, dim, math.ceil((col0 + dim) / 7)


def new(d, bg=W):
    img = Image.new("RGB", (LW, LH), bg)
    return img, ImageDraw.Draw(img)


def ink(dr, xy, txt, font, anchor="la"):
    """真实墨迹包围盒（不是 em 盒），用它排带与带之间的间距就不会撞。"""
    return dr.textbbox(xy, txt, font=font, anchor=anchor)


def fit(dr, txt, max_w, start=230, floor=56, med=False):
    """把文字自动缩到 max_w 宽以内（巨字排版用，避免 'RUARY' 这种长尾溢出画布）。"""
    sz = start
    while sz > floor and dr.textlength(txt, font=lat(sz, med)) > max_w:
        sz -= 2
    return sz


def fit_grid(d, y0, bottom=None, max_row=92):
    """行高自动吃满 [y0, bottom] 这段高度：月格永远排到下边距，不留空白带。"""
    rows = cells(d)[2]
    b = LH - M if bottom is None else bottom
    return int(min(max_row, max(50.0, (b - y0 - 60) / (rows - 0.5))))


def grid(dr, d, y0, row_h, fg, head, today_bg=None, today_fg=None, hol=None,
         weekend=None, dot=None, num_sz=None, hd=None):
    """通用月格。hol: 'bar' 数字下方黄条 / 'chip' 整格黄底 / None；dot: 数字下方红点；
    weekend: 周六日数字换色；hd: 表头字列表（给汉字表头用）。
    字号、今日块、标记位置全部随 row_h 缩放。"""
    col0, dim, rows = cells(d)
    nsz = num_sz or int(max(26, min(54, row_h * 0.58)))
    hh = int(min(26, row_h * 0.36))
    hw = int(min(31, CW * 0.40))
    r = int(min(14, hh))
    mark = hh + 4
    f_head = cj(22) if hd else lat(24, med=True)
    f_num = lat(nsz, med=True)
    labels = hd or [w[0] for w in WD1]
    for i in range(7):
        c = head if (i < 5 or weekend is None) else weekend
        dr.text((GX + i * CW + CW // 2, y0 - 22), labels[i], font=f_head, fill=c, anchor="mm")
    for day in range(1, dim + 1):
        cell = col0 + day - 1
        row, col = divmod(cell, 7)
        cx, cy = GX + col * CW + CW // 2, y0 + 14 + row * row_h + row_h // 2
        t = (day == d.day)
        chip = (not t) and hol == "chip" and is_hol(d.replace(day=day))
        if t and today_bg:
            dr.rounded_rectangle([cx - hw, cy - hh, cx + hw, cy + hh], r, fill=today_bg)
        elif chip:
            dr.rounded_rectangle([cx - hw, cy - hh, cx + hw, cy + hh], r, fill=Y)
        col_fg = (today_fg if t else (K if chip else (weekend if (weekend and col > 4) else fg)))
        dr.text((cx, cy), str(day), font=f_num, fill=col_fg, anchor="mm")
        if not t and not chip:
            if hol == "bar" and is_hol(d.replace(day=day)):
                dr.rounded_rectangle([cx - 19, cy + mark, cx + 19, cy + mark + 6], 3, fill=Y)
            elif dot and is_hol(d.replace(day=day)):
                dr.ellipse([cx - 5, cy + mark, cx + 5, cy + mark + 10], fill=R)


# ---------- E 黑场：K 当底，W 当字，Y 标今日，R 标周末 / 节假日 ----------

def e1(d):
    img, dr = new(d, K)
    b = ink(dr, (M + 22, 26), str(d.day), lat(200))
    dr.text((M + 22, 26), str(d.day), font=lat(200), fill=W)
    dr.text((LW - M - 22, 40), MON[d.month - 1], font=lat(44), fill=W, anchor="ra")
    dr.text((LW - M - 22, 104), str(d.year), font=lat(44), fill=Y, anchor="ra")
    y = b[3] + 22
    dr.text((M + 26, y), WD[d.weekday()], font=lat(34), fill=R)
    ly = y + 60
    dr.line([M + 22, ly, LW - M - 22, ly], fill=W, width=2)
    y0 = ly + 48
    grid(dr, d, y0, fit_grid(d, y0), W, W, today_bg=Y, today_fg=K, weekend=R, dot=1)
    return img


def e2(d):
    img, dr = new(d, K)
    dr.rectangle([0, 0, LW, 152], fill=Y)
    dr.text((M + 22, 46), MON[d.month - 1], font=lat(50), fill=K)
    dr.text((M + 22, 100), str(d.year), font=lat(28), fill=K)
    dr.text((LW // 2, 268), str(d.day), font=lat(168), fill=W, anchor="mm")
    dr.text((LW // 2, 366), WD[d.weekday()], font=lat(32), fill=R, anchor="mm")
    y0 = 424
    grid(dr, d, y0, fit_grid(d, y0), W, W, today_bg=R, today_fg=W, weekend=Y, dot=1)
    return img


def e3(d):
    """月格在上、巨字日期贴底：两带都用真实墨迹高度相接，不留空白也不撞格头。"""
    img, dr = new(d, K)
    b1 = ink(dr, (LW - M - 22, 30), MON[d.month - 1], lat(40), "ra")
    dr.text((LW - M - 22, 30), MON[d.month - 1], font=lat(40), fill=W, anchor="ra")
    b2 = ink(dr, (LW - M - 22, b1[3] + 14), WD[d.weekday()], lat(30), "ra")
    dr.text((LW - M - 22, b2[3] - 30), WD[d.weekday()], font=lat(30), fill=R, anchor="ra")
    y0 = b2[3] + 44
    rows = cells(d)[2]
    dsz = fit(dr, str(d.day), LW - 2 * M - 70, start=184)
    hb = ink(dr, (M + 22, 0), str(d.day), lat(dsz))
    bar_top = LH - M - 14
    dy = bar_top - 16 - hb[3]                       # 日期墨迹下沿贴到黄条上方
    row_h = fit_grid(d, y0, bottom=dy + hb[1] - 22)
    grid(dr, d, y0, row_h, W, W, today_bg=Y, today_fg=K, weekend=R, dot=1)
    dr.text((M + 22, dy), str(d.day), font=lat(dsz), fill=W)
    dr.rounded_rectangle([M + 26, bar_top, M + 26 + 176, bar_top + 14], 8, fill=Y)
    return img


# ---------- F 印章：W 当底，R 做今日印章，Y 只做节假日下划线 ----------

def f1(d):
    img, dr = new(d, W)
    b0 = ink(dr, (LW // 2, 40), MON[d.month - 1], lat(46), "ma")
    dr.text((LW // 2, 40), MON[d.month - 1], font=lat(46), fill=K, anchor="ma")
    box = [LW // 2 - 128, b0[3] + 22, LW // 2 + 128, b0[3] + 22 + 236]
    stamp = Image.new("RGBA", (LW, LH), (0, 0, 0, 0))
    ds = ImageDraw.Draw(stamp)
    ds.rounded_rectangle(box, 44, fill=R)
    cy = (box[1] + box[3]) // 2
    ds.text((LW // 2, cy), str(d.day), font=lat(168), fill=W, anchor="mm")
    rot = stamp.rotate(-4, expand=False, resample=Image.BICUBIC)
    img.paste(rot, (0, 0), rot)           # mask 必须是旋转后的那张，否则透明黑会漏成假阴影
    dr = ImageDraw.Draw(img)
    wy = box[3] + 38
    dr.text((LW // 2, wy), WD[d.weekday()], font=lat(34), fill=K, anchor="mm")
    y0 = wy + 58
    grid(dr, d, y0, fit_grid(d, y0), K, K, today_bg=R, today_fg=W, hol="chip")
    return img


def f2(d):
    img, dr = new(d, W)
    dr.ellipse([LW // 2 - 122, 60, LW // 2 + 122, 304], fill=R)
    dr.text((LW // 2, 182), str(d.day), font=lat(150), fill=W, anchor="mm")
    dr.text((LW // 2, 336), MON[d.month - 1] + " " + str(d.year), font=lat(40), fill=K, anchor="mm")
    dr.text((LW // 2, 386), WD[d.weekday()], font=lat(30), fill=R, anchor="mm")
    y0 = 444
    grid(dr, d, y0, fit_grid(d, y0), K, K, today_bg=R, today_fg=W, hol="chip")
    return img


def f3(d):
    img, dr = new(d, W)
    dr.rounded_rectangle([M, 40, LW - M, 196], 36, fill=R)
    b = ink(dr, (M + 34, 62), str(d.day), lat(104))
    dr.text((M + 34, 62), str(d.day), font=lat(104), fill=W)
    tx = b[2] + 22
    dr.text((tx, 74), WD[d.weekday()], font=lat(34), fill=W)
    dr.text((tx, 120), MON[d.month - 1] + " " + str(d.year), font=lat(26), fill=Y)
    y0 = 248
    grid(dr, d, y0, fit_grid(d, y0), K, K, today_bg=R, today_fg=W, hol="chip")
    return img


# ---------- G 巨字：月份 / 日期二选一当主角，Y 只染周末列 ----------

def g1(d):
    """月份巨字满宽打头，红日期 + 荧光黄下划线做第二带，月格吃满剩余高度。"""
    img, dr = new(d, W)
    m_ab = MON[d.month - 1][:3]
    s1 = fit(dr, m_ab, LW - 2 * M - 8, start=210)
    b = ink(dr, (M + 4, 16), m_ab, lat(s1))
    dr.text((M + 4, 16), m_ab, font=lat(s1), fill=K)
    y2 = b[3] + 20
    dsz = fit(dr, str(d.day), 240, start=172)
    hb = ink(dr, (M + 6, y2), str(d.day), lat(dsz))
    dr.text((M + 6, y2), str(d.day), font=lat(dsz), fill=R)
    dr.text((LW - M - 8, hb[3]), WD[d.weekday()] + "  ·  " + str(d.year),
            font=lat(28), fill=K, anchor="rd")
    dr.rounded_rectangle([hb[0] - 2, hb[3] + 8, hb[2] + 6, hb[3] + 22], 8, fill=Y)
    y0 = hb[3] + 78
    grid(dr, d, y0, fit_grid(d, y0), K, K, today_bg=K, today_fg=W, weekend=R, dot=1)
    return img


def g2(d):
    """月份拆两行（下行染黄）+ 右上角红日期，全部按墨迹高度排带。"""
    img, dr = new(d, W)
    l1 = MON[d.month - 1][:3]
    l2 = MON[d.month - 1][3:] or str(d.year)
    s1 = fit(dr, l1, 320, start=150)
    b1 = ink(dr, (M + 12, 20), l1, lat(s1))
    dr.text((M + 12, 20), l1, font=lat(s1), fill=K)
    dsz = fit(dr, str(d.day), 210, start=s1)
    bd = ink(dr, (LW - M - 12, 20), str(d.day), lat(dsz), "ra")
    dr.text((LW - M - 12, 20), str(d.day), font=lat(dsz), fill=R, anchor="ra")
    band = max(b1[3], bd[3])
    s2 = fit(dr, l2, LW - 2 * M - 16, start=s1)
    b2 = ink(dr, (M + 12, band + 10), l2, lat(s2))
    dr.text((M + 12, band + 10), l2, font=lat(s2), fill=Y)
    b3 = ink(dr, (M + 14, b2[3] + 18), WD[d.weekday()], lat(30))
    dr.text((M + 14, b2[3] + 18), WD[d.weekday()], font=lat(30), fill=K)
    y0 = b3[3] + 44
    grid(dr, d, y0, fit_grid(d, y0), K, K, today_bg=R, today_fg=W, weekend=R, dot=1)
    return img


def g3(d):
    img, dr = new(d, W)
    dr.text((LW // 2, 150), str(d.day), font=lat(260), fill=K, anchor="mm")
    dr.line([M + 22, 300, LW - M - 22, 300], fill=R, width=4)
    dr.text((M + 22, 322), MON[d.month - 1], font=lat(40), fill=K)
    dr.text((LW - M - 22, 330), str(d.year), font=lat(30), fill=R, anchor="ra")
    y0 = 414
    grid(dr, d, y0, fit_grid(d, y0), K, K, today_bg=Y, today_fg=K, weekend=R, dot=1)
    return img


# ---------- H 本周条：不做月格，改成 7 / 14 行横条清单 ----------

def week_rows(dr, d, y0, rh, n, big, today_bg=K, today_fg=W, hol_bar=Y, head=K):
    """n=7：本周一到今天再往后凑满 7 天；n=14：从今天起连续两周。"""
    f_wd, f_day = lat(30 if big else 24), (lat(56 if big else 40, med=True))
    start = d if n == 14 else d - datetime.timedelta(days=d.weekday())
    for i in range(n):
        day = start + datetime.timedelta(days=i)
        top = y0 + i * rh
        is_t = (day == d)
        if is_t:
            dr.rounded_rectangle([M, top, LW - M, top + rh - 6], 16, fill=today_bg)
        elif is_hol(day):
            dr.rounded_rectangle([M + 4, top + 3, LW - M - 4, top + rh - 9], 14, fill=hol_bar)
        c = today_fg if is_t else (R if is_hol(day) else head)
        dr.text((M + 26, top + (rh - 6) // 2), WD1[day.weekday()], font=f_wd, fill=c, anchor="lm")
        dr.text((LW - M - 26, top + (rh - 6) // 2), str(day.day), font=f_day, fill=c, anchor="rm")
        dr.text((LW - M - 96, top + (rh - 6) // 2),
                MON[day.month - 1][:3] if day.month != d.month else "", font=lat(20), fill=c, anchor="rm")


def h1(d):
    img, dr = new(d, W)
    dr.text((M + 4, 34), MON[d.month - 1], font=lat(52), fill=K)
    dr.text((LW - M - 4, 52), str(d.year), font=lat(34), fill=R, anchor="ra")
    dr.text((M + 4, 100), "WEEK " + str(d.isocalendar()[1]), font=lat(24), fill=K)
    dr.line([M, 132, LW - M, 132], fill=K, width=2)
    week_rows(dr, d, 152, 84, 7, True)
    return img


def h2(d):
    img, dr = new(d, W)
    dr.rectangle([0, 0, LW, 96], fill=K)
    dr.text((M + 4, 26), MON[d.month - 1] + " " + str(d.year), font=lat(40), fill=W)
    dr.text((LW - M - 4, 34), str(d.day), font=lat(48), fill=Y, anchor="ra")
    week_rows(dr, d, 118, 45, 14, False, today_bg=K, today_fg=W, hol_bar=Y)
    return img


def h3(d):
    img, dr = new(d, W)
    dr.text((M + 4, 30), MON[d.month - 1], font=lat(46), fill=K)
    dr.text((M + 4, 86), WD[d.weekday()], font=lat(30), fill=R)
    y = 148
    for i in range(7):
        day = d + datetime.timedelta(days=i - d.weekday())
        top = y + i * 84
        is_t = (day == d)
        if is_t:
            dr.rounded_rectangle([M, top, LW - M, top + 76], 18, fill=Y)
        elif is_hol(day):
            dr.rectangle([M, top + 8, M + 10, top + 68], fill=R)
        c = K
        dr.text((M + 34, top + 38), WD1[day.weekday()], font=lat(28), fill=c, anchor="lm")
        dr.text((LW - M - 20, top + 38), str(day.day),
                font=lat(84 if is_t else 44, med=True), fill=c, anchor="rm")
    return img


# ---------- 共用：竖排 / 机械翻页牌 / 打卡孔 / 看板 ----------

CN = "〇一二三四五六七八九"
MON_CN = [f"{CN[i]}月" for i in range(1, 10)] + ["十月", "十一月", "十二月"]
WD_CN = ["星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日"]
HOLNAME = {(2026, 1, 1): "元旦", (2026, 2, 15): "春节", (2026, 4, 4): "清明",
           (2026, 5, 1): "劳动节", (2026, 6, 19): "端午", (2026, 9, 25): "中秋",
           (2026, 10, 1): "国庆节", (2027, 1, 1): "元旦"}


def cn_day(n):
    if n < 10:
        return CN[n]
    if n == 10:
        return "十"
    if n < 20:
        return "十" + CN[n - 10]
    return CN[n // 10] + "十" + (CN[n % 10] if n % 10 else "")


def vtext(dr, x, y, s, font, fill, sp=1.14):
    """竖排：每字居中于 x，从 y 往下排，返回下一列的起始 y。"""
    step = font.size * sp
    for i, ch in enumerate(s):
        dr.text((x, y + i * step), ch, font=font, fill=fill, anchor="ma")
    return y + len(s) * step


def flap(img, txt, font, fg, bg, cx, cy, gap=7, skew=9, notch=True, solid=True, cut=True):
    """机械翻页牌：一行字切成上下两半、左右错开，中间留一条缝 + 两侧轴钉。
    cut=False 时不切字（小字会被撕碎），缝只画在牌的两端。"""
    dr = ImageDraw.Draw(img)
    bb = dr.textbbox((0, 0), txt, font=font, anchor="mm")
    tw, th = int(bb[2] - bb[0] + 40), int(bb[3] - bb[1] + 40)
    tile = Image.new("RGB", (tw, th), bg)
    ImageDraw.Draw(tile).text((tw // 2, th // 2), txt, font=font, fill=fg, anchor="mm")
    mid = th // 2
    x0, y0 = int(cx - tw / 2), int(cy - th / 2)
    if solid:
        dr.rectangle([x0 - skew, y0, x0 + tw + skew, y0 + th], fill=bg)
    if cut:
        img.paste(tile.crop((0, 0, tw, mid)), (x0 - skew, y0))
        img.paste(tile.crop((0, mid, tw, th)), (x0 + skew, y0 + mid))
    else:
        img.paste(tile, (x0, y0))
    d2 = ImageDraw.Draw(img)
    segs = ([(x0 - skew - 4, x0 + tw + skew + 4)] if cut
            else [(x0 - 4, x0 + 26), (x0 + tw - 30, x0 + tw + 4)])
    for sx0, sx1 in segs:
        d2.rectangle([sx0, y0 + mid - gap // 2, sx1, y0 + mid + gap // 2], fill=bg)
    if notch:
        for nx in (x0 - skew - 4, x0 + tw + skew - 6):
            d2.rectangle([nx, y0 + mid - 9, nx + 10, y0 + mid + 9], fill=fg)
    return x0, y0, x0 + tw, y0 + th


def plate(dr, box, r=34, fg=K, bg=None, w=4):
    """大圆角牌（沿用「凡带框必大 R 角」的规矩）。"""
    if bg is not None:
        dr.rounded_rectangle(box, r, fill=bg)
    dr.rounded_rectangle(box, r, outline=fg, width=w)


# ---------- I 翻页历：只关心「今天这一张」，缝穿过数字 ----------

def i1(d):
    """纯翻页：整版就是今天这一页，不画月格。"""
    img, dr = new(d, W)
    dr.rectangle([0, 0, LW, 92], fill=K)
    dr.text((M + 8, 24), MON[d.month - 1], font=lat(42), fill=W)
    dr.text((LW - M - 8, 44), str(d.year), font=lat(34), fill=Y, anchor="rd")
    b = flap(img, str(d.day), lat(330), K, W, LW // 2, 320, gap=9, skew=11)
    dr.text((LW // 2, b[3] + 30), WD[d.weekday()], font=lat(40), fill=R, anchor="ma")
    nxt = d + datetime.timedelta(days=1)
    y = LH - M - 96
    dr.rounded_rectangle([M, y, LW - M, LH - M], 30, fill=Y)
    dr.text((M + 26, y + 22), "明日", font=cj(26), fill=K)
    dr.text((M + 26 + dr.textlength("明日", font=cj(26)) + 12, y + 24),
            "TOMORROW", font=lat(24), fill=K)
    dr.text((LW - M - 26, y + 48), str(nxt.day), font=lat(56), fill=K, anchor="rm")
    if is_hol(nxt):
        dr.text((LW - M - 116, y + 48), "节假日", font=cj(26), fill=K, anchor="rm")
    return img


def i2(d):
    """翻页 + 紧凑月格：上半是机械牌，下半仍是整月。"""
    img, dr = new(d, W)
    b = flap(img, str(d.day), lat(184), W, K, LW // 2 - 120, 182, gap=7, skew=6)
    plate(dr, [b[0] - 18, b[1] - 18, b[2] + 18, b[3] + 18], 40, fg=K)
    mx = LW - M - (b[2] + 18) - 18
    dr.text((LW - M, 104), MON[d.month - 1], font=lat(fit(dr, MON[d.month - 1], mx, 40, 24)),
            fill=K, anchor="ra")
    dr.text((LW - M, 160), str(d.year), font=lat(34), fill=R, anchor="ra")
    dr.text((LW - M, 214), WD[d.weekday()], font=lat(fit(dr, WD[d.weekday()], mx, 30, 20)),
            fill=R, anchor="ra")
    y0 = max(b[3] + 66, 372)
    grid(dr, d, y0, fit_grid(d, y0), K, K, today_bg=R, today_fg=W, weekend=R, dot=1)
    return img


def i3(d):
    """三排机械牌：日期 / 星期 / 月份各一张，像老式车站时刻牌。
    只有巨号日期真的切片错位；小字改「缝只画在牌两端」，否则笔画被撕碎。"""
    img, dr = new(d, K)
    y = 30
    for txt, fnt, col, sk in ((str(d.day), lat(168), W, 4), (WD[d.weekday()], lat(46), Y, 0),
                              (f"{MON[d.month - 1]} {d.year}", lat(46), W, 0)):
        bb = dr.textbbox((0, 0), txt, font=fnt, anchor="mm")
        th = int(bb[3] - bb[1]) + 40
        b = flap(img, txt, fnt, col, K, LW // 2, y + th // 2, gap=6, skew=sk, cut=(sk > 0))
        plate(dr, [M + 20, b[1] - 14, LW - M - 20, b[3] + 14], 30, fg=W)
        y = b[3] + 32
    dr.text((M + 24, y + 4), "THIS MONTH", font=lat(24), fill=R)
    col0, dim, rows = cells(d)
    y0 = y + 56
    rh = min(44, (LH - M - y0) / rows)
    w = (LW - 2 * M) / 7
    for i in range(7):
        dr.text((M + i * w + w / 2, y0 - 14), WD1[i][0], font=lat(20, med=True),
                fill=Y if i > 4 else W, anchor="mm")
    for day in range(1, dim + 1):
        c, r = (col0 + day - 1) % 7, (col0 + day - 1) // 7
        cx, cy = M + c * w + w / 2, y0 + r * rh + rh / 2
        if day == d.day:
            dr.rounded_rectangle([cx - 17, cy - 15, cx + 17, cy + 15], 10, fill=R)
            dr.text((cx, cy), str(day), font=lat(22, med=True), fill=W, anchor="mm")
        else:
            dr.text((cx, cy), str(day), font=lat(22, med=True),
                    fill=Y if is_hol(d.replace(day=day)) else W, anchor="mm")
    return img


# ---------- J 打卡卡：31 个孔位本身就是「本月过了多少天」 ----------

def holes(dr, d, y0, bottom, shape="circle", gap=8):
    """状态编码：已过=黑底白字 / 今日=红底白字 / 节假日=黄底黑字 / 未来=黑描边。"""
    col0, dim, rows = cells(d)
    rh = (bottom - y0) / rows
    for day in range(1, dim + 1):
        c, r = (col0 + day - 1) % 7, (col0 + day - 1) // 7
        cx, cy = GX + c * CW + CW // 2, y0 + r * rh + rh / 2
        rad = min(CW, rh) / 2 - gap
        t = day == d.day
        hol = is_hol(d.replace(day=day))
        fill, fg = (R, W) if t else (Y, K) if hol else (K, W) if day < d.day else (W, K)
        box = [cx - rad, cy - rad, cx + rad, cy + rad]
        kw = {"fill": fill} if fill != W else {"outline": K, "width": 3}
        if shape == "circle":
            dr.ellipse(box, **kw)
        else:
            dr.rounded_rectangle(box, rad * 0.62, **kw)
        dr.text((cx, cy), str(day), font=lat(int(rad * 0.86), med=True), fill=fg, anchor="mm")
    return y0 + rows * rh


def legend(dr, y, sz=22):
    x = M + 6
    for txt, bg, fg in (("已过", K, W), ("今天", R, W), ("节假日", Y, K), ("未过", W, K)):
        if bg != W:
            dr.ellipse([x, y - 9, x + 18, y + 9], fill=bg)
        else:
            dr.ellipse([x, y - 9, x + 18, y + 9], outline=K, width=2)
        dr.text((x + 26, y - sz // 2 + 1), txt, font=cj(sz), fill=K)
        x += 26 + dr.textlength(txt, font=cj(sz)) + 26
    return y


def j1(d):
    img, dr = new(d, W)
    plate(dr, [M, 24, LW - M, 116], 28, bg=K)
    dr.text((M + 26, 48), MON[d.month - 1], font=lat(40), fill=W)
    dr.text((LW - M - 30, 66), str(d.year), font=lat(30), fill=Y, anchor="rd")
    dr.text((LW - M - 30, 102), WD[d.weekday()], font=lat(26), fill=W, anchor="rd")
    for i in range(7):
        dr.text((GX + i * CW + CW // 2, 148), WD1[i][0], font=lat(24, med=True),
                fill=R if i > 4 else K, anchor="mm")
    bottom = holes(dr, d, 170, LH - M - 56)
    legend(dr, bottom + 40)
    return img


def j2(d):
    img, dr = new(d, W)
    dr.text((M + 6, 24), f"{MON[d.month - 1]} {d.year}", font=lat(44), fill=K)
    dr.text((M + 8, 84), WD[d.weekday()], font=lat(26), fill=R)
    dr.text((LW - M - 6, 100), str(d.day), font=lat(64), fill=R, anchor="rd")
    for i in range(7):
        dr.text((GX + i * CW + CW // 2, 132), WD1[i][0], font=lat(24, med=True),
                fill=R if i > 4 else K, anchor="mm")
    bottom = holes(dr, d, 156, LH - M - 56, shape="square", gap=4)
    legend(dr, bottom + 40)
    return img


def j3(d):
    """巨字 + 一条 31 孔长带：不按周对齐，纯读「本月走了多远」。"""
    img, dr = new(d, W)
    sz = fit(dr, str(d.day), 236, start=250, floor=140)
    dr.text((M + 4, 16), str(d.day), font=lat(sz), fill=K)
    b = ink(dr, (M + 4, 16), str(d.day), lat(sz))
    tx = b[2] + 22
    _, dim, _ = cells(d)
    dr.text((tx, 60), MON[d.month - 1], font=lat(38), fill=K)
    dr.text((tx, 112), str(d.year), font=lat(32), fill=K)
    dr.text((tx, 168), WD[d.weekday()], font=lat(32), fill=R)
    y = 322
    lab = "本月已过"
    dr.text((M + 4, y - 44), lab, font=cj(26), fill=K)
    dr.text((M + 8 + dr.textlength(lab, font=cj(26)), y - 40),
            f"{d.day} / {dim}", font=lat(26), fill=R)
    per = 8
    cw = (LW - 2 * M) / per
    rows = math.ceil(dim / per)
    rh = (LH - M - 62 - y) / rows
    rad = min(cw, rh) / 2 - 5
    for day in range(1, dim + 1):
        c, r = (day - 1) % per, (day - 1) // per
        cx, cy = M + c * cw + cw / 2, y + r * rh + rh / 2
        t = day == d.day
        hol = is_hol(d.replace(day=day))
        fill, fg = (R, W) if t else (Y, K) if hol else (K, W) if day < d.day else (W, K)
        kw = {"fill": fill} if fill != W else {"outline": K, "width": 3}
        dr.rounded_rectangle([cx - rad, cy - rad, cx + rad, cy + rad], rad * 0.55, **kw)
        dr.text((cx, cy), str(day), font=lat(int(rad * 0.9), med=True), fill=fg, anchor="mm")
    legend(dr, y + rows * rh + 30)
    return img


# ---------- K 看板：反白星期带 + 周次列 + 粗格线 + 节假日名 ----------

def board(dr, d, y0, bottom, lb=True, name=True):
    col0, dim, rows = cells(d)
    x0 = M + (58 if lb else 0)
    cwid = (LW - M - x0) / 7
    rh = (bottom - y0 - 50) / rows
    dr.line([M, y0 + 50, LW - M, y0 + 50], fill=K, width=4)
    for i in range(7):
        dr.text((x0 + i * cwid + cwid / 2, y0 + 26), "一二三四五六日"[i],
                font=cj(28), fill=R if i > 4 else K, anchor="mm")
    for i in range(1, 7):
        dr.line([x0 + i * cwid, y0 + 50, x0 + i * cwid, bottom], fill=K, width=3)
    for r in range(rows):
        dr.line([M, y0 + 50 + (r + 1) * rh, LW - M, y0 + 50 + (r + 1) * rh], fill=K, width=3)
        if lb:
            first = d.replace(day=1) + datetime.timedelta(days=r * 7 - col0)
            dr.text((M + 26, y0 + 50 + r * rh + rh / 2), "W%02d" % first.isocalendar()[1],
                    font=lat(22, med=True), fill=K, anchor="mm")
    if lb:
        dr.line([M + 52, y0 + 50, M + 52, bottom], fill=K, width=3)
    nsz = int(min(cwid, rh) * 0.5)
    ch = min(rh - 10, cwid * 1.18)          # 块不能拉成竖胶囊：高度按列宽封顶
    for day in range(1, dim + 1):
        c, r = (col0 + day - 1) % 7, (col0 + day - 1) // 7
        cx = x0 + c * cwid + cwid / 2
        cy = y0 + 50 + r * rh + rh / 2
        t = day == d.day
        hol = is_hol(d.replace(day=day))
        box = [cx - cwid / 2 + 5, cy - ch / 2, cx + cwid / 2 - 5, cy + ch / 2]
        if t:
            dr.rounded_rectangle(box, 18, fill=R)
        elif hol:
            dr.rounded_rectangle(box, 18, fill=Y)
        dr.text((cx, cy - (11 if (hol or t) and name else 0)), str(day),
                font=lat(nsz, med=True), fill=W if t else K, anchor="mm")
        key = (d.year, d.month, day)
        if name and hol and not t and key in HOLNAME:
            dr.text((cx, cy + nsz * 0.68), HOLNAME[key], font=cj(19), fill=R, anchor="mm")
    return bottom


def k1(d):
    img, dr = new(d, W)
    plate(dr, [M, 20, LW - M, 104], 26, bg=K)
    dr.text((M + 24, 34), f"{d.year}年{d.month}月", font=cj(40), fill=W)
    dr.text((LW - M - 36, 68), WD_CN[d.weekday()], font=cj(30), fill=Y, anchor="rd")
    board(dr, d, 118, LH - M - 6)
    return img


def k2(d):
    """左侧今日竖牌 + 右侧月格：远看先看到数字，走近才看到整月。"""
    img, dr = new(d, W)
    plate(dr, [M, 20, M + 178, LH - M], 44)
    cx = M + 89
    dr.text((cx, 58), "今天", font=cj(28), fill=R, anchor="mm")
    dr.text((cx, 172), str(d.day), font=lat(130), fill=K, anchor="mm")
    dr.text((cx, 250), WD_CN[d.weekday()], font=cj(26), fill=K, anchor="mm")
    hol = sorted([(dd, v) for (yy, mm, dd), v in HOLNAME.items() if yy == d.year and mm == d.month])
    mine = HOLNAME.get((d.year, d.month, d.day))
    if mine:
        dr.rounded_rectangle([cx - 74, 296, cx + 74, 348], 26, fill=R)
        dr.text((cx, 322), mine, font=cj(28), fill=W, anchor="mm")
    elif hol:
        dd, v = hol[0]
        dr.rounded_rectangle([cx - 74, 296, cx + 74, 348], 26, fill=Y)
        dr.text((cx, 322), f"{v} {dd}日", font=cj(24), fill=K, anchor="mm")
    else:
        dr.rounded_rectangle([cx - 74, 296, cx + 74, 348], 26, outline=K, width=3)
        dr.text((cx, 322), "本月无休", font=cj(24), fill=K, anchor="mm")
    dr.text((cx, 424), str(d.year), font=lat(38), fill=K, anchor="mm")
    dr.text((cx, 478), MON[d.month - 1], font=lat(26), fill=K, anchor="mm")
    dr.text((cx, 530), "MONTH", font=lat(20), fill=R, anchor="mm")
    col0, dim, rows = cells(d)
    x0, gw = M + 202, LW - M - (M + 202)
    cwid, rh = gw / 7, (LH - M - 24 - 150) / rows
    for i in range(7):
        dr.text((x0 + i * cwid + cwid / 2, 126), "一二三四五六日"[i],
                font=cj(24), fill=R if i > 4 else K, anchor="mm")
    nsz = int(min(cwid, rh) * 0.52)
    ch = min(rh - 8, cwid * 1.2)
    for day in range(1, dim + 1):
        c, r = (col0 + day - 1) % 7, (col0 + day - 1) // 7
        cx2, cy = x0 + c * cwid + cwid / 2, 150 + r * rh + rh / 2
        t = day == d.day
        h2 = is_hol(d.replace(day=day))
        box = [cx2 - cwid / 2 + 4, cy - ch / 2, cx2 + cwid / 2 - 4, cy + ch / 2]
        if t:
            dr.rounded_rectangle(box, 16, fill=R)
        elif h2:
            dr.rounded_rectangle(box, 16, fill=Y)
        dr.text((cx2, cy), str(day), font=lat(nsz, med=True), fill=W if t else K, anchor="mm")
    return img


def k3(d):
    """甘特看板：一行一周，7 条轨道拉满整幅，节假日与今日在条上直接读出来。"""
    img, dr = new(d, W)
    dr.text((M + 4, 20), f"{d.year}年{d.month}月", font=cj(44), fill=K)
    dr.text((LW - M - 4, 44), WD_CN[d.weekday()], font=cj(28), fill=R, anchor="rd")
    col0, dim, rows = cells(d)
    y = 112
    rh = (LH - M - 8 - y) / rows
    x0 = M + 62
    sw = (LW - M - x0) / 7
    ch = min(rh - 14, sw * 1.25)
    for i in range(1, 7):                       # 竖轨先画：空格子也看得出是「没有这一天」
        dr.line([x0 + i * sw, y, x0 + i * sw, LH - M - 8], fill=K, width=2)
    for r in range(rows):
        top = y + r * rh
        dr.line([M, top + rh - 2, LW - M, top + rh - 2], fill=K, width=3)
        first = d.replace(day=1) + datetime.timedelta(days=r * 7 - col0)
        dr.text((M + 4, top + rh / 2), "W%02d" % first.isocalendar()[1],
                font=lat(22, med=True), fill=K, anchor="lm")
        for c in range(7):
            day = 1 - col0 + r * 7 + c
            if day < 1 or day > dim:
                continue
            box = [x0 + c * sw + 4, top + (rh - ch) / 2, x0 + (c + 1) * sw - 4,
                   top + (rh + ch) / 2]
            t = day == d.day
            hol = is_hol(d.replace(day=day))
            if t:
                dr.rounded_rectangle(box, 16, fill=R)
                col = W
            elif hol:
                dr.rounded_rectangle(box, 16, fill=Y)
                col = K
            else:
                col = R if c > 4 else K
            dr.text(((box[0] + box[2]) / 2, (box[1] + box[3]) / 2), str(day),
                    font=lat(int(min(sw, ch) * 0.46), med=True), fill=col, anchor="mm")
    return img


# ---------- L 竖排历牌：从右往左读，汉字承重 ----------

def l1(d):
    """三竖列从右往左读：月 / 巨号日 / 星期；下半整月。"""
    img, dr = new(d, W)
    vtext(dr, LW - M - 46, 34, MON_CN[d.month - 1], cj(60), K)
    sz = fit(dr, str(d.day), LW - 2 * M - 240, start=250, floor=150)
    b = ink(dr, (LW - M - 124, 30), str(d.day), lat(sz), "ra")
    dr.text((LW - M - 124, 30), str(d.day), font=lat(sz), fill=K, anchor="ra")
    vtext(dr, M + 52, 34, WD_CN[d.weekday()], cj(40), R)
    y = int(max(b[3] + 46, 300))
    dr.line([M, y, LW - M, y], fill=K, width=3)
    y0 = y + 48
    grid(dr, d, y0, fit_grid(d, y0), K, K, today_bg=K, today_fg=W, weekend=R, dot=1,
         hd=list("一二三四五六日"))
    return img


def l2(d):
    """整版一列竖排、从右往左读：月年 / 巨汉字日 / 星期。极简单页，不画月格。"""
    img, dr = new(d, W)
    txt = cn_day(d.day)
    top, band = 40, LH - M - 116 - 40
    sz = int(min(168, band / (len(txt) * 1.18)))
    h = len(txt) * sz * 1.18
    y0 = top + (band - h) / 2
    vtext(dr, LW - M - 48, 56, MON_CN[d.month - 1], cj(52), K)
    vtext(dr, LW - M - 48, 56 + int(2 * 52 * 1.18) + 30, f"{d.year}年", cj(26), R)
    dr.line([LW - M - 104, 44, LW - M - 104, LH - M - 140], fill=K, width=3)
    vtext(dr, LW // 2 - 30, y0, txt, cj(sz), K, sp=1.18)
    vtext(dr, M + 52, 56, WD_CN[d.weekday()], cj(40), R)
    y = LH - M - 116
    plate(dr, [M, y, LW - M, LH - M], 34, bg=K)
    dr.text((M + 30, y + 56), f"{d.day:02d}  {MON[d.month - 1]}  {WD[d.weekday()]}",
            font=lat(28), fill=W, anchor="lm")
    hol = HOLNAME.get((d.year, d.month, d.day))
    if hol:
        dr.text((LW - M - 30, y + 56), hol, font=cj(30), fill=Y, anchor="lm")
    return img


def l3(d):
    """竖排「X日」巨汉字 + 右侧红条节假日名 + 下方整月。"""
    img, dr = new(d, W)
    txt = cn_day(d.day) + "日"
    sz = int(min(146, 340 / (len(txt) * 1.18)))
    vtext(dr, M + 80, 26, txt, cj(sz), K, sp=1.18)
    strip = HOLNAME.get((d.year, d.month, d.day)) or next(
        (v for (yy, mm, dd), v in HOLNAME.items() if yy == d.year and mm == d.month), None)
    dr.text((LW - M - 14, 40), WD_CN[d.weekday()], font=cj(42), fill=R, anchor="ra")
    dr.text((LW - M - 14, 98), f"{d.year} · {MON[d.month - 1]}", font=lat(26), fill=K, anchor="ra")
    if strip:
        h = int(len(strip) * 40 * 1.18) + 44
        dr.rounded_rectangle([LW - M - 78, 134, LW - M, 134 + h], 40, fill=R)
        vtext(dr, LW - M - 39, 158, strip, cj(38), W)
    y0 = 412
    grid(dr, d, y0, fit_grid(d, y0), K, K, today_bg=R, today_fg=W, hol="chip",
         hd=list("一二三四五六日"))
    return img


SCHEMES = {"E": ("黑场", (e1, e2, e3)), "F": ("印章", (f1, f2, f3)),
           "G": ("巨字", (g1, g2, g3)), "H": ("本周条", (h1, h2, h3)),
           "I": ("翻页历", (i1, i2, i3)), "J": ("打卡卡", (j1, j2, j3)),
           "K": ("看板", (k1, k2, k3)), "L": ("竖排历牌", (l1, l2, l3))}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "..", "spark-output", "board", "moink-cal-v2-moodboard")
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    n = 0
    for code, (_, fns) in SCHEMES.items():
        for i, fn in enumerate(fns, 1):
            img = fn(DATES[i - 1])
            name = f"{code.lower()}{i}"
            img.save(os.path.join(out, name + ".png"))
            img.resize((LW * 2, LH * 2), Image.NEAREST).save(os.path.join(out, name + "@2x.png"))
            n += 2
            print(f"{name}  {code} {SCHEMES[code][0]}  {DATES[i-1]}")
    print(f"-> {out}（{n} 个文件）")


if __name__ == "__main__":
    main()
