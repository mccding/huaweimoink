#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
墨印 · MoInk —— 外部设计稿 1:1 复刻试排（真机口径，不参与固件编译）

和 cal_concept_board.py 的分工：那份是"方向探索"（本机画出来当提案，已被判定不适合），
这一份是"照稿复刻"——设计稿由外部 AI 出，本机只负责验证它在 552×768 / 四色 / 固件同款
字库下排不排得开，所以这里的每个尺寸都是固件能落地的尺寸，不是风格提案。

不照抄原图的三处错误（AI 自己画错了）：
  C：原图日期从 1 顺序填满竖列，没跟真实星期对齐（2023-02-01 是周三不是周日），末尾还多画一个 27；
  D：原图第三行是 12 16 14 15 17，跳了 13 且乱序；
  待办：原图 8 条画了 9 个进度块、3/8 配 4 个实心 —— 这里一律按"块数=条目数、实心=完成数"自动算。

字体只用固件真的烧进去的那两款（Helvetica Neue Bold / 冬青黑体 Hiragino Sans GB W6），
不用原图那种窄体（Condensed），否则校样好看、设备上排不出来。

    python3 tools/replicate_templates.py [输出目录]
"""
import calendar
import datetime
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image, ImageDraw  # noqa: E402

from cal_concept_board import (LH, LW, MON, WD, WD_CN, cj, is_hol, lat,  # noqa: E402
                               new, DATES)

# 2bpp 的 index 2 / 3 在面板上是芥末黄与朱红，不是纯黄纯红；按实测色画校样
K, W = (0, 0, 0), (255, 255, 255)
MU, VE = (242, 194, 0), (217, 58, 43)

MON_ABBR = ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
HEAD_EN = ["Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"]   # 周日开头，两字母避免 S/T 撞车


def has_cjk(t):
    return any("\u3000" <= ch <= "\u9fff" for ch in t)


def fnt(t, sz, med=False):
    """混排串一律交给冬青（它自带拉丁），纯拉丁才用 Helvetica —— 与固件同一口径。"""
    return cj(sz) if has_cjk(t) else lat(sz, med=med)


def mon_cn(d):
    return f"{d.month}月" if d.month < 11 else ["十月", "十一月", "十二月"][d.month - 10]


def wd_cn(d):
    return WD_CN[d.weekday()]


def mon_txt(d, lang):
    return mon_cn(d) if lang == "zh" else MON[d.month - 1]


def wd_txt(d, lang):
    return wd_cn(d) if lang == "zh" else WD[d.weekday()]


def date_line(d, lang):
    """待办 A 牌右：10月5日 星期一 / Oct 5, Monday。"""
    if lang == "zh":
        return f"{mon_cn(d)}{d.day}日 {wd_cn(d)}"
    return f"{MON_ABBR[d.month - 1]} {d.day}, {WD[d.weekday()].title()}"


def fit_h(dr, txt, font_of, max_w, max_h, start, floor, step=4):
    """同时受宽和受高约束地缩字号；font_of(sz) 造字体。"""
    sz = start
    while sz > floor:
        b = dr.textbbox((0, 0), txt, font=font_of(sz), anchor="mm")
        if b[2] - b[0] <= max_w and b[3] - b[1] <= max_h:
            break
        sz -= step
    return sz


def tick(dr, cx, cy, s, fg=W):
    """勾：以盒子中心为锚，三段折线。"""
    p = [(cx - s * 0.36, cy + s * 0.02), (cx - s * 0.08, cy + s * 0.30),
         (cx + s * 0.40, cy - s * 0.30)]
    dr.line(p, fill=fg, width=max(3, int(s * 0.15)), joint="curve")


# ---------------------------------------------------------------- C 翻页牌 + 竖列周

def cal_c(d, lang="zh"):
    """外层黑框 + 大翻牌 + 周日开头的竖列月格 + 底部芥末带。"""
    img, dr = new(d, W)
    dr.rounded_rectangle([0, 0, LW - 1, LH - 1], 34, outline=K, width=16)

    # 翻牌块：黑底白巨字，中缝切断数字（四色下缝只能是黑，压在白字上才读得出）
    tx0, ty0, tw, th = 40, 50, 236, 318
    dr.rounded_rectangle([tx0, ty0, tx0 + tw, ty0 + th], 28, fill=K)
    day = str(d.day)
    dsz = fit_h(dr, day, lambda s: lat(s), tw - 40, th - 62, 268, 96, 6)
    dr.text((tx0 + tw // 2, ty0 + th // 2), day, font=lat(dsz), fill=W, anchor="mm")
    mid = ty0 + th // 2
    # 缝只能画成黑：牌是黑的，缝唯一可见的地方就是切断白字的那一段
    dr.rectangle([tx0, mid - 3, tx0 + tw, mid + 3], fill=K)
    for px in ((tx0 - 10, tx0 + 15), (tx0 + tw - 15, tx0 + tw + 10)):
        dr.rounded_rectangle([px[0], mid - 11, px[1], mid + 11], 6, fill=K)

    # 右栏：月份在上，年 + 星期在下（红）。语言由 cal_lang 决定，版式不换
    mt, wt = mon_txt(d, lang), wd_txt(d, lang)
    rx = tx0 + tw + 22
    rw = LW - 42 - rx
    msz = fit_h(dr, mt, lambda s: fnt(mt, s), rw, 999, 60, 28)
    dr.text((rx, ty0 + msz // 2 + 6), mt, font=fnt(mt, msz), fill=K, anchor="lm")
    lsz = fit_h(dr, wt, lambda s: fnt(wt, s), rw, 999, 54, 26)
    dr.text((rx, ty0 + th - 62), str(d.year), font=lat(lsz), fill=VE, anchor="ls")
    dr.text((rx, ty0 + th), wt, font=fnt(wt, lsz), fill=VE, anchor="ls")

    # 月格：7 条竖列，列头周日开头；列间细竖线，整块圆角描边
    # 框往下长到 730（离芥末带留一条 16，与外框描边等宽），行距才够放「数字 + 红点」两层
    gx0, gy0, gx1, gy1 = 40, 392, 512, 730
    dr.rounded_rectangle([gx0, gy0, gx1, gy1], 26, outline=K, width=4)
    col0 = (d.replace(day=1).weekday() + 1) % 7          # 0 = 周日
    dim = calendar.monthrange(d.year, d.month)[1]
    rows = math.ceil((col0 + dim) / 7)
    pad = 18
    pitch = (gx1 - gx0 - 2 * pad) / 7.0
    hdr_y = gy0 + 32
    r0, r1 = hdr_y + 26, gy1 - 20
    rp = (r1 - r0) / rows                          # 行距按行数吃满，红点才压不到下一行
    nsz = int(min(30, rp - 16))
    f_num = lat(nsz, med=True)
    hsz = 26 if lang == "zh" else 24
    for i in range(7):
        cx = gx0 + pad + pitch * (i + 0.5)
        if i:
            lx = gx0 + pad + pitch * i
            dr.line([lx, hdr_y + 20, lx, gy1 - 14], fill=K, width=2)
        lab = "日一二三四五六"[i] if lang == "zh" else HEAD_EN[i]
        dr.text((cx, hdr_y + cj_mid(dr, lab, hsz)), lab, font=fnt(lab, hsz),
                fill=VE if i in (0, 6) else K, anchor="mm")
    for day_n in range(1, dim + 1):
        idx = col0 + day_n - 1
        col, row = idx % 7, idx // 7
        cx = gx0 + pad + pitch * (col + 0.5)
        cy = int(r0 + rp * (row + 0.5)) - 6         # 上移让出底部点道，行高仍按 rp 吃满
        hol = is_hol(d.replace(day=day_n))
        red = hol or col in (0, 6)
        b = dr.textbbox((cx, cy), str(day_n), font=f_num, anchor="mm")
        if day_n == d.day:      # 描边只贴数字墨迹，别把点道吃掉
            dr.rounded_rectangle([b[0] - 6, b[1] - 4, b[2] + 6, b[3] + 4], 7,
                                 outline=K, width=3)
        dr.text((cx, cy), str(day_n), font=f_num, fill=VE if red else K, anchor="mm")
        if hol:
            # 点严格落在这行占位框与下一行之间那条道正中，两侧对称留白
            # b 是绝对坐标，算下一行占位顶要先把墨迹偏移从 cy 里剥出来
            lo, hi = b[3] + 4, gy1 - 12
            if row + 1 < rows:
                hi = int(r0 + rp * (row + 1.5)) - 6 + (b[1] - cy) - 4
            rr = max(2, min(4, (hi - lo) // 2 - 3))
            dy = (lo + hi) // 2
            dr.ellipse([cx - rr, dy - rr, cx + rr, dy + rr], fill=VE)

    dr.rectangle([0, LH - 22, LW, LH], fill=MU)
    return img


# ---------------------------------------------------------------- D 打卡点阵

def cal_d(d, lang="zh"):
    """黑牌月份 + 悬垂红日期 + 每行 5 圆点阵（已过/今天/节日/未至）+ 图例。"""
    img, dr = new(d, W)
    mt = mon_txt(d, lang)
    dr.rounded_rectangle([24, 26, 528, 120], 26, fill=K)
    msz = fit_h(dr, mt, lambda s: fnt(mt, s), 300, 999, 62, 30)
    ysz = msz
    mw = dr.textlength(mt, font=fnt(mt, msz))
    yw = dr.textlength(str(d.year), font=lat(ysz))
    gap = 18
    x0 = (LW - (mw + yw + gap)) / 2
    dr.text((x0, 73 + cj_mid(dr, mt, msz)), mt, font=fnt(mt, msz), fill=W, anchor="lm")
    dr.text((x0 + mw + gap, 73 + cj_mid(dr, str(d.year), ysz)), str(d.year),
            font=lat(ysz), fill=MU, anchor="lm")

    dim = calendar.monthrange(d.year, d.month)[1]
    rows = math.ceil(dim / 5)
    bx0, by0, bx1, by1 = 30, 202, 522, 740
    # 大红色日期悬在牌下但留出间距：原稿压在框的圆角上，读起来像穿模
    dsz = fit_h(dr, str(d.day), lambda s: lat(s), 150, 62, 118, 56)
    dr.text((bx1 - 18, 160), str(d.day), font=lat(dsz), fill=VE, anchor="rm")
    wk = wd_txt(d, lang)
    wsz = fit_h(dr, wk, lambda s: fnt(wk, s), 300, 999, 46, 26)
    dr.text((48, 160 + cj_mid(dr, wk, wsz)), wk, font=fnt(wk, wsz), fill=K, anchor="lm")
    dr.rounded_rectangle([bx0, by0, bx1, by1], 30, outline=K, width=5)

    top, bottom = 240, by1 - 92
    py = min(70.0, (bottom - top) / rows)
    dia = py - 9
    for day_n in range(1, dim + 1):
        r, c = divmod(day_n - 1, 5)
        cx = bx0 + 30 + ((bx1 - bx0 - 60) / 5.0) * (c + 0.5)
        cy = top + py * (r + 0.5)
        box = [cx - dia / 2, cy - dia / 2, cx + dia / 2, cy + dia / 2]
        # 已过优先于节日：国庆连休不会把上半月刷成一片黄
        if day_n == d.day:
            dr.ellipse(box, fill=VE)
            fg = W
        elif day_n < d.day:
            dr.ellipse(box, fill=K)
            fg = W
        elif is_hol(d.replace(day=day_n)):
            dr.ellipse(box, fill=MU)
            fg = K
        else:
            dr.ellipse(box, outline=K, width=3)
            fg = K
        dr.text((cx, cy), str(day_n), font=lat(int(dia * 0.62), med=True), fill=fg, anchor="mm")

    labels = (["已过", "今天", "节日", "未至"] if lang == "zh"
              else ["PAST", "TODAY", "HOLIDAY", "NEXT"])
    styles = [dict(fill=K), dict(fill=VE), dict(fill=MU), dict(outline=K, width=3)]
    fsz, gl = 24, 22

    def legend_w(f):
        return sum(f + 8 + dr.textlength(t, font=fnt(t, f)) for t in labels) + gl * 3

    # 图例必须整条待在圆角框里，不能像原稿那样压到右边框上
    while fsz > 14 and legend_w(fsz) > bx1 - bx0 - 48:
        fsz -= 2
    ly = by1 - 52
    lx = ((bx0 + bx1) - legend_w(fsz)) / 2
    for t, kw in zip(labels, styles):
        dr.ellipse([lx, ly - fsz / 2, lx + fsz, ly + fsz / 2], **kw)
        dr.text((lx + fsz + 8, ly + cj_mid(dr, t, fsz)), t, font=fnt(t, fsz),
                fill=K, anchor="lm")
        lx += fsz + 8 + dr.textlength(t, font=fnt(t, fsz)) + gl
    return img


# ---------------------------------------------------------------- 待办共用的条目模型

ITEMS_A = [("浇绿植", True), ("交房租", True), ("回复王总的邮件", False),
           ("驿站取快递", False), ("写周报", True), ("预约牙医", False),
           ("跑一遍 NAS 备份", False), ("读《置身事内》两章", False)]
ITEMS_B = [("团队周会", True), ("客户电话准备", False), ("更新看板", True),
           ("评审项目排期", False), ("午餐约见", False), ("发版", True),
           ("和干系人过需求", False), ("定稿状态报告", True)]
# 最坏情况：12 条上限 + 一条顶到 40 汉字，用来量"排不排得下"
ITEMS_MAX = [("确认墨水屏驱动时序改动是否影响刷新功耗", True), ("交房租", False),
             ("回复王总的邮件", True), ("驿站取快递", False), ("写周报", False),
             ("预约牙医", True), ("跑一遍 NAS 备份", False), ("读《置身事内》两章", False),
             ("给客厅换灯泡", True), ("整理 Q4 目标", False), ("备份照片到移动硬盘", False),
             ("周五前提交报销单", False)]
# 英文界面对应的条目（真机上条目内容随用户输入，这里只是让校样不中英混排）
ITEMS_A_EN = [("Water the plants", True), ("Pay the rent", True), ("Reply to Mr. Wang", False),
              ("Pick up the parcel", False), ("Write the weekly", True),
              ("Book the dentist", False), ("Run the NAS backup", False),
              ("Read two chapters", False)]
ITEMS_B_EN = [("Team standup", True), ("Prep client call", False), ("Update the board", True),
              ("Review the schedule", False), ("Lunch meeting", False), ("Ship the release", True),
              ("Sync on scope", False), ("Finalize the report", True)]
ITEMS_MAX_EN = [("Check whether the EPD timing change costs refresh power", True),
                ("Pay the rent", False), ("Reply to Mr. Wang", True),
                ("Pick up the parcel", False), ("Write the weekly", False),
                ("Book the dentist", True), ("Run the NAS backup", False),
                ("Read two chapters", False), ("Replace the living-room bulb", True),
                ("Sort out Q4 goals", False), ("Back up photos to the SSD", False),
                ("Submit expenses before Friday", False)]


def row_tsz(dr, tsz, avail, txt):
    """长文自动缩到一行放得下（与页面 measureText 同思路）；缩到 18 号还放不下就截断。
    必须按真实字宽量——拉丁约半个汉字宽，按字数估会把英文缩得过小。"""
    sz = tsz
    while sz > 18 and dr.textlength(txt, font=fnt(txt, sz)) > avail:
        sz -= 2
    return sz


# ---------------------------------------------------------------- 待办 A 横线条目表

def clip(dr, txt, sz, max_w):
    """放不下就砍尾（真机上比缩到 14 号更好读）。"""
    while len(txt) > 1 and dr.textlength(txt, font=fnt(txt, sz)) > max_w:
        txt = txt[:-1]
    return txt


def cj_mid(dr, txt, sz):
    """PIL 的 middle 锚的是 em 盒中点，汉字墨迹偏上；返回把墨迹中心对到 y 的补偿量。
    删除线和行分隔线的"正中间"感全靠它，不补就会看着下坠。拉丁同理，只是偏得少些。"""
    b = dr.textbbox((0, 0), txt, font=fnt(txt, sz), anchor="lm")
    return -int(round((b[1] + b[3]) / 2.0))


def todo_a(d, items=ITEMS_A, lang="zh"):
    img, dr = new(d, W)
    hd = "今天" if lang == "zh" else "Today"
    hsz = fit_h(dr, hd, lambda s: fnt(hd, s), 300, 999, 52, 30)
    dr.rounded_rectangle([24, 22, 528, 106], 26, fill=K)
    dr.text((48, 64 + cj_mid(dr, hd, hsz)), hd, font=fnt(hd, hsz), fill=W, anchor="lm")
    hz = date_line(d, lang)
    zsz = fit_h(dr, hz, lambda s: fnt(hz, s), 300, 999, 30, 20)
    dr.text((504, 64 + cj_mid(dr, hz, zsz)), hz, font=fnt(hz, zsz), fill=MU, anchor="rm")

    n = len(items)
    top, bottom = 122, 672
    pitch = (bottom - top) / n
    cb = int(min(46, pitch * 0.60))
    tsz = int(min(38, pitch * 0.54))
    cur = next((i for i, (_, dn) in enumerate(items) if not dn), None)
    for i, (txt, done) in enumerate(items):
        cy = top + pitch * (i + 0.5)
        bx, by = 46, cy - cb / 2
        tx = bx + cb + 24
        sz = row_tsz(dr, tsz, 504 - tx, txt)
        txt = clip(dr, txt, sz, 504 - tx)
        dy = cj_mid(dr, txt, sz)
        if done:
            dr.rounded_rectangle([bx, by, bx + cb, by + cb], int(cb * 0.24), fill=K)
            tick(dr, bx + cb / 2, cy, cb)
        elif i == cur:
            dr.rounded_rectangle([bx, by, bx + cb, by + cb], int(cb * 0.24), fill=VE)
        else:
            dr.rounded_rectangle([bx, by, bx + cb, by + cb], int(cb * 0.24),
                                 outline=K, width=5)
        dr.text((tx, cy + dy), txt, font=fnt(txt, sz), fill=K, anchor="lm")
        if done:
            tw = dr.textlength(txt, font=fnt(txt, sz))
            dr.line([tx - 6, cy, tx + 10 + tw, cy], fill=K, width=3)
        dr.line([42, top + pitch * (i + 1), 510, top + pitch * (i + 1)], fill=K, width=2)

    dn = sum(1 for _, x in items if x)
    sz = int(min(30, 330 / n - 8))
    px = sz + 9
    for i in range(n):
        x = 46 + i * px
        kw = dict(fill=K) if i < dn else dict(outline=K, width=3)
        dr.rounded_rectangle([x, 700, x + sz, 700 + sz], int(sz * 0.26), **kw)
    dr.text((510, 700 + sz / 2), f"{dn}/{n}", font=lat(46), fill=VE, anchor="rm")
    return img


# ---------------------------------------------------------------- 待办 B 红色时间轴

def todo_b(d, items=ITEMS_B, lang="zh"):
    img, dr = new(d, W)
    if lang == "zh":
        hd = f"{d.year}年{mon_cn(d)}{d.day}日 {wd_cn(d)}"
        tag = "工作日" if d.weekday() < 5 else "休息日"
    else:
        hd = f"{WD[d.weekday()].title()}, {MON_ABBR[d.month - 1]} {d.day} {d.year}"
        tag = "Workday" if d.weekday() < 5 else "Day off"
    hsz = fit_h(dr, hd, lambda s: fnt(hd, s), 470, 999, 34, 20)
    tsz2 = fit_h(dr, tag, lambda s: fnt(tag, s), 470, 999, 28, 18)
    dr.rounded_rectangle([24, 22, 528, 118], 26, fill=K)
    dr.text((LW / 2, 55 + cj_mid(dr, hd, hsz)), hd, font=fnt(hd, hsz), fill=W, anchor="mm")
    dr.text((LW / 2, 92 + cj_mid(dr, tag, tsz2)), tag, font=fnt(tag, tsz2), fill=MU,
            anchor="mm")

    n = len(items)
    top, bottom = 150, 682
    pitch = (bottom - top) / n
    spine_x, spine_w = 68, 16
    dr.rounded_rectangle([spine_x - spine_w / 2, top - 14, spine_x + spine_w / 2, bottom + 14],
                         spine_w / 2, fill=VE)
    cb = int(min(42, pitch * 0.56))
    tsz = int(min(36, pitch * 0.52))
    cur = next((i for i, (_, dn) in enumerate(items) if not dn), None)
    for i, (txt, done) in enumerate(items):
        cy = top + pitch * (i + 0.5)
        if i == cur:
            dr.rounded_rectangle([96, cy - pitch * 0.44, 528, cy + pitch * 0.44],
                                 int(pitch * 0.44), fill=MU)
        dr.line([spine_x - 12, cy, 124, cy], fill=K, width=5)
        if i == cur:
            s = 52
            dr.rounded_rectangle([spine_x - s / 2 - 12, cy - s / 2,
                                  spine_x - s / 2 + 40, cy + s / 2], 14, fill=K)
            dr.text((spine_x - 12 + 14, cy), str(i + 1), font=lat(38), fill=W, anchor="mm")
        else:
            dr.text((30, cy), str(i + 1), font=lat(38), fill=K, anchor="mm")
        bx, by = 138, cy - cb / 2
        tx = bx + cb + 22
        sz = row_tsz(dr, tsz, 504 - tx, txt)
        txt = clip(dr, txt, sz, 504 - tx)
        if done:
            dr.rounded_rectangle([bx, by, bx + cb, by + cb], int(cb * 0.24), fill=K)
            tick(dr, bx + cb / 2, cy, cb)
        else:
            dr.rounded_rectangle([bx, by, bx + cb, by + cb], int(cb * 0.24),
                                 outline=K, width=5)
        dr.text((tx, cy + cj_mid(dr, txt, sz)), txt, font=fnt(txt, sz), fill=K, anchor="lm")
        if done:
            tw = dr.textlength(txt, font=fnt(txt, sz))
            dr.line([tx - 6, cy, tx + 10 + tw, cy], fill=K, width=3)

    dn = sum(1 for _, x in items if x)
    ft = "已完成" if lang == "zh" else "done"
    dr.text((512, 712), f"{dn}/{n}", font=lat(42), fill=K, anchor="rm")
    dr.text((512, 742 + cj_mid(dr, ft, 22)), ft, font=fnt(ft, 22), fill=K, anchor="rm")
    return img


FACES = {"c": ("翻页牌竖列历", cal_c), "d": ("打卡点阵历", cal_d)}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "..", "spark-output", "board", "moink-replicate"))
    os.makedirs(out, exist_ok=True)
    made = []
    for lang, sfx in (("zh", ""), ("en", "e")):
        for code, (_, fn) in FACES.items():
            for i, dt in enumerate(DATES, 1):      # 5 月是 6 行最坏情况，必须一起出
                made += [(fn(dt, lang), f"{code}{i}{sfx}")]
        dt = DATES[0]
        A = ITEMS_A if lang == "zh" else ITEMS_A_EN
        B = ITEMS_B if lang == "zh" else ITEMS_B_EN
        M = ITEMS_MAX if lang == "zh" else ITEMS_MAX_EN
        made += [(todo_a(dt, A, lang), f"ta1{sfx}"), (todo_b(dt, B, lang), f"tb1{sfx}"),
                 (todo_a(dt, M, lang), f"ta2{sfx}"), (todo_b(dt, M, lang), f"tb2{sfx}")]
    for img, name in made:
        img.save(os.path.join(out, name + ".png"))
        img.resize((LW * 2, LH * 2), Image.NEAREST).save(os.path.join(out, name + "@2x.png"))
    print(f"-> {out}（{len(made) * 2} 个文件）")


if __name__ == "__main__":
    main()
