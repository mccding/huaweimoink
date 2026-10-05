/*
 * 墨印 · MoInk — 日历版面（R1.5.0 功能1，四套样式：暖纸 / 红格 / 翻页牌 / 打卡点阵）
 *
 * 四套都是情绪板方案的 1:1 复刻（见 spark-output/context/board.json 的 device_mapping），
 * 样式名在代码里叫 WARM/GRID/FLAP/PUNCH，因为 "A1" 已经是面板驱动模式名（a1_mode）：
 *   暖纸 CAL_STYLE_WARM = 方案 B1，vibe_images/moinkp-b1_1791121171918_484127fb.png
 *     顶部奶黄块（**大圆角**，块内三行全部黑字：整月名+年 / 大号日期 / 星期）+ 白区月格；
 *     月格**不画格线**，今日 = 黑色实心圆 + 白字，节假日 = 数字下方一个红点。
 *   红格 CAL_STYLE_GRID = 方案 A1，vibe_images/moinkp-a1_1791121137322_2b831f8d.png
 *     白底，顶行小字「年 / 整月名」（黑），超大日期 + 星期（**红**），
 *     下面月格：**外框大圆角描边** + 直角内部分格线，今日 = 圆角红底反白，
 *     节假日 = 圆角黄底黑字（色块与格线之间留白，圆角才读得出来）。
 *   翻页牌 CAL_STYLE_FLAP = 方案 C，vibe_images/moinkp-c1_1791121231686_be3a5195.png
 *     整页 16 px 黑圆角外框 + 底边一条芥末带（带子盖住外框下沿，与参考图一致）；
 *     左上黑圆角牌 = 白色大日期 + 一条横切中缝 + 左右两个铰粒（四色下缝只能画黑，
 *     牌本身是黑的，缝读得出来的唯一地方就是切断白字那一段）；右栏月名（黑）+
 *     年 / 星期（**红**）；下半 = 圆角描边月格，**列 0 = 周日**、列间一条细竖线，
 *     周末与节假日的数字为红，今日 = 贴着数字的描边胶囊，节假日 = 数字下一个红点。
 *   打卡点阵 CAL_STYLE_PUNCH = 方案 D，vibe_images/moinkp-d1_1791121267322_5034f832.png
 *     顶部黑圆角牌（月面白、年芥末）；牌下悬垂一个红色大日期（既不压牌也不压框），
 *     同一水平线左边是星期；下面大圆角框里**每行 5 个圆、按自然日顺序**排
 *     （不按周对齐），状态优先级 今日红 > 已过黑 > 节日芥末 > 未至描边
 *     （已过优先，连休不会把上半月刷成一片黄）；框底一行图例，四颗点与四态对应。
 *   四套共用同一批字模、同一条中线、同一张月格算术与同一份节假日表，
 *   差别只在配色、字号档位和「今日 / 节假日」怎么标。凡带框的地方都是大 R 角。
 *
 * 面板只有 K/W/Y/R 四色、没有灰度，也读不到参考图的奶油白：暖纸的奶黄底用
 * Y/W 斜纹（4 格上 3 格黄）在观看距离上混色（块内一律黑字 —— 白字压在斜纹上
 * 边缘会起毛，而且白对纯黄只有 1.07:1 的对比，根本读不出）；红格的红 / 黄是实色。
 * 参考图里节假日格内那行小字节日名（"Indigenous Peoples' Day"）**不做**：
 * 设备最小字模 32 px ≈ 6 mm，塞进 74 px 宽的格子既放不下也读不清。
 *
 * 竖版画布 552x768 画好后旋进 768x552 的 2bpp 帧，全程只有一块 106 KB 缓冲。
 * 帧字节布局（与 epd_drv.c / 页面 packIdx 同一口径）：
 *   行 y 存在缓冲第 (H-1-y) 行（自下而上），行内字节按 x 倒序 = (RB-1-(x>>2))，
 *   像素 x 在该字节的 ((x&3)<<1) 位；色码 0K 1W 2Y 3R，白底 = 0x55。
 */
#include "cal_face.h"

#include <stdio.h>
#include <string.h>

#include "cal_font.h"

#define PHYS_W        EPD_CAL_W        /* 768 */
#define PHYS_H        EPD_CAL_H        /* 552 */
#define RB            (PHYS_W / 4)     /* 192 字节/行 */
#define FB_LEN        (RB * PHYS_H)

#define LW            PHYS_H           /* 竖版画布宽 552 */
#define LH            PHYS_W           /* 竖版画布高 768 */
#define MID           (LW / 2)         /* 276，所有行按这条中线左右对称 */

#define C_K 0
#define C_W 1
#define C_Y 2
#define C_R 3

/*
 * 竖版贴横屏的两种装法。默认 0 = 把设备**逆时针**转 90°（右边那条边变成顶）读正；
 * 真机装反（字是倒的）就把这里改成 1 重编 —— 只有这一个开关，别处不用动。
 */
#define CAL_ROT_FLIP  0

/* ---------- 版式（逻辑坐标，改这里即可整体挪版） ----------
 * 四套样式的月格共用同一份纵向预算思路：表头贴着一段留白落在块下方，
 * 行高 = 剩余高度 / 实际行数（限幅），所以 5 行的月份不会被拉出半个空页。
 *
 * 暖纸：14 ┌ 奶黄斜纹块 268（**圆角 44**；月+年 70 / 大日期 86..206 / 星期 256）282
 *        ├ 表头 334
 *        └ 月格 354 .. ≤754（5~6 行，无格线）
 * 红格：14 ┌ 顶行「年 / 月」52
 *        ├ 超大红日期 66..246 / 红星期 290
 *        ├ 表头 344
 *        └ 月格 358 .. ≤754（5~6 行，**外框圆角 34** + 直角格线 + 圆角色块）
 * 翻页牌：0 ┌ 黑外框 16（圆角 34）
 *        ├ 黑牌 40,50 237x319（白色大日期 + 中缝 209 + 左右铰粒）
 *        │    右栏 299..510：月名基线 96 / 红年 312 / 红星期 362
 *        ├ 月格框 40,392 473x339（圆角 26）：表头基线 437、行带 450..710、点道 10
 *        └ 底部芥末带 746..767（22 高）
 * 打卡点阵：黑牌 24,26 505x95（圆角 26；月面白基线 90 / 年芥末）
 *        ├ 悬垂红大日期 右边缘 507、盒顶 122（墨迹 128..191，上下各让开 8 / 11）
 *        ├ 星期基线 174 @ u 48
 *        └ 框 30,202 493x539（圆角 30）：5 列 x 7 行圆，图例基线 700
 */

#define MARGIN        14           /* 四周留白（不画外框） */
#define NUM_H         24           /* d32 数字墨迹高（基线以上），行内垂直居中按它算 */

/* 月格列几何（两套共用）：7 列 x 74 = 518，左右各留 17 */
#define GRID_U        (MARGIN + 3)
#define GRID_CELL_W   74
#define GRID_COLS     7
#define LINE_T        2                      /* 格线宽 ≈ 0.4 mm，1 px 在纸上会断 */
#define BOT_MARGIN    14

/* 暖纸：顶部奶黄块 */
#define WM_BLK_U      MARGIN
#define WM_BLK_V      MARGIN
#define WM_BLK_W      (LW - 2 * MARGIN)      /* 524 */
#define WM_BLK_H      268
#define WM_BLK_R      44                     /* 块圆角半径 ≈ 8.4 mm */
#define WM_DENS       3                      /* 斜纹密度：4 格里上 3 格黄 */
#define WM_L1_BASE    (WM_BLK_V + 56)        /* 70  整月名 + 年 */
#define WM_BIG_V      (WM_L1_BASE + 16)      /* 86  大号日期盒顶（120 高） */
#define WM_L3_BASE    (WM_BIG_V + CAL_B_H + 50)  /* 256 星期 */
#define WM_BLK_BOT    (WM_BLK_V + WM_BLK_H)  /* 282 */
#define WM_WD_BASE    (WM_BLK_BOT + 52)      /* 334 表头 */
#define WM_GRID_V     (WM_WD_BASE + 20)      /* 354 月格顶 */
#define WM_ROW_MAX    92
#define DISC_R        21                     /* 今日黑圆：Ø42 ≈ 8 mm */
#define DOT_R         5                      /* 节假日红点：Ø10 ≈ 1.9 mm */
#define DOT_BELOW     13                     /* 圆心在数字基线下方这么多 */

/* 红格：整页白底 */
#define GD_L1_BASE    52                     /* 顶行「年 / 月」 */
#define GD_BIG_V      (GD_L1_BASE + 14)      /* 66  超大红日期盒顶（180 高） */
#define GD_L3_BASE    (GD_BIG_V + CAL_B180_H + 44)  /* 290 红星期 */
#define GD_WD_BASE    344                    /* 表头 */
#define GD_GRID_V     (GD_WD_BASE + 14)      /* 358 月格顶 */
#define GD_ROW_MAX    84
#define GD_GRID_R     34                     /* 月格外框圆角 ≈ 6.5 mm */
#define GD_CELL_R     18                     /* 今日 / 节假日色块圆角 */
#define CHIP_PAD      5                      /* 色块与格线之间留的白，圆角才看得见 */

/* 翻页牌：整页黑框 + 左上黑牌 + 右栏 + 竖线分列月格 + 底边芥末带 */
#define FL_FRAME_R    34                     /* 外框圆角 ≈ 6.5 mm */
#define FL_FRAME_T    16                     /* 外框描边 ≈ 2.1 mm，参考图就是粗框 */
#define FL_BAND_H     22                     /* 底边芥末带：盖住外框下沿，与参考图一致 */
#define FL_TX         40
#define FL_TY         50
#define FL_TW         237
#define FL_TH         319
#define FL_TR         28                     /* 黑牌圆角 */
#define FL_SEAM       7                      /* 中缝：牌是黑的，只有切断白字才读得出 */
#define FL_PIN_W      26
#define FL_PIN_H      23
#define FL_RX         (FL_TX + FL_TW + 22)   /* 299 右栏左沿，栏宽 211 */
#define FL_MON_BASE   96                     /* 月名（EN 只能 32 档：48 档最长 305 > 211） */
#define FL_YR_BASE    312                    /* 红年 */
#define FL_WD_BASE    362                    /* 红星期，基线几乎压在牌的下沿上 */
#define FL_GX         40                     /* 月格框 */
#define FL_GY         392
#define FL_GW         473
#define FL_GH         339
#define FL_GR         26
#define FL_GT         4
#define FL_PAD        18
#define FL_PITCH      62                     /* 7 列 x 62 = 434，左右各余 18 / 21 */
#define FL_HEAD_BASE  437                    /* 列头基线（周日开头） */
#define FL_ROW0       (FL_GY + 58)           /* 450 首个行带顶 */
#define FL_ROW1       (FL_GY + FL_GH - 20)   /* 711 末个行带底 */
#define FL_RULE_T     2
#define FL_LANE       10                     /* 行带底部留给红点的道，数字不占 */
#define FL_DOT_R      4
#define FL_CHIP_R     8
#define FL_CHIP_T     3
#define FL_CHIP_X     7                      /* 描边胶囊离数字墨迹的间隙 */
#define FL_CHIP_Y     4                      /* 最大安全值：6 行月行距 43，再大会压红点道 */

/* 打卡点阵：黑牌 + 悬垂红日期 + 每行 5 圆 + 图例 */
#define PD_PL_U       24
#define PD_PL_V       26
#define PD_PL_W       505
#define PD_PL_H       95
#define PD_PL_R       26
#define PD_PL_BASE    90                     /* 牌内一行：月面（白）+ 年（芥末）同一基线 */
#define PD_PL_GAP     18                     /* 月面与年之间空的距离 */
#define PD_DAY_U1     507                    /* 悬垂大日期右边缘（墨迹落在 504） */
#define PD_DAY_V      122                    /* b64 盒顶 76 高，墨迹 128..191 */
#define PD_WD_U       48
#define PD_WD_BASE    174                    /* 与悬垂日期同一水平带 */
#define PD_BX         30                     /* 身体大圆角框 */
#define PD_BY         202
#define PD_BW         493
#define PD_BH         539
#define PD_BR         30
#define PD_BT         5
#define PD_COLS       5
#define PD_INSET      30                     /* 圆心离框边的水平余量 */
#define PD_PITCH      ((PD_BW - 2 * PD_INSET) / PD_COLS)   /* 86：列心距 */
#define PD_TOP        240                    /* 首行圆心的带上沿 */
#define PD_BOT        648                    /* 末行圆心的带下沿：再往下留给图例 */
#define PD_PY_MAX     70
#define PD_LG_BASE    700                    /* 图例基线 */
#define PD_LG_DOT_R   10
#define PD_LG_TEXT_GAP 8
#define PD_LG_ITEM_GAP 16

/* ---------- 像素原语 ---------- */

static inline void px(uint8_t *fb, int u, int v, uint8_t c)
{
    if ((unsigned)u >= LW || (unsigned)v >= LH) return;   /* 画布外直接丢，越界不可能 */
#if CAL_ROT_FLIP
    const int x = v, y = LW - 1 - u;
#else
    const int x = LH - 1 - v, y = u;
#endif
    uint8_t *p = &fb[(PHYS_H - 1 - y) * RB + (RB - 1 - (x >> 2))];
    const int sh = (x & 3) << 1;
    *p = (uint8_t)((*p & ~(3 << sh)) | (c << sh));
}

static void fill_rect(uint8_t *fb, int u, int v, int w, int h, uint8_t c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            px(fb, u + i, v + j, c);
}

/* 落在圆角矩形内的判断：(i,j) 是盒内坐标。四角各按半径 r 的圆弧裁掉。 */
static int in_round_rect(int i, int j, int w, int h, int r)
{
    const int dx = (i < r) ? r - i : ((i >= w - r) ? i - (w - 1 - r) : 0);
    const int dy = (j < r) ? r - j : ((j >= h - r) ? j - (h - 1 - r) : 0);
    return dx * dx + dy * dy <= r * r;
}

/* 斜纹底：每 4 格上 per4 格色，观看距离上混成淡色（面板没有第二种黄，深浅只能靠密度）。
 * Bayer 4x4 阈值表比 1:1 棋盘更匀，r > 0 时整块按圆角裁。 */
static void fill_dither(uint8_t *fb, int u, int v, int w, int h, uint8_t c, int per4, int r)
{
    static const uint8_t BAY[4][4] = {
        {  0,  8,  2, 10 }, { 12,  4, 14,  6 },
        {  3, 11,  1,  9 }, { 15,  7, 13,  5 }
    };
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (in_round_rect(i, j, w, h, r) && BAY[j & 3][i & 3] < per4 * 4)
                px(fb, u + i, v + j, c);
}

static void fill_round_rect(uint8_t *fb, int u, int v, int w, int h, int r, uint8_t c)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (in_round_rect(i, j, w, h, r)) px(fb, u + i, v + j, c);
}

/* 圆角矩形描边：线宽 t 从外沿向内收，内外两条弧同心（r-t 才不会露出缺口）。 */
static void stroke_round_rect(uint8_t *fb, int u, int v, int w, int h, int r, int t, uint8_t c)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (in_round_rect(i, j, w, h, r) &&
                !in_round_rect(i - t, j - t, w - 2 * t, h - 2 * t, r - t))
                px(fb, u + i, v + j, c);
}

static void fill_circle(uint8_t *fb, int uc, int vc, int r, uint8_t c)
{
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r)
                px(fb, uc + i, vc + j, c);
}

/* 圆环：外半径 r、线宽 t，从外沿向内收（D 的「未至」圆圈）。 */
static void stroke_circle(uint8_t *fb, int uc, int vc, int r, int t, uint8_t c)
{
    const int ri = r - t;
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++) {
            const int d2 = i * i + j * j;
            if (d2 <= r * r && (ri <= 0 || d2 > ri * ri)) px(fb, uc + i, vc + j, c);
        }
}

/* ---------- 混排：一行里拉丁 / 汉字各取一档字模，按同一条基线站 ---------- */

enum { T_L48 = 0, T_L32, T_L24, T_D32, T_C32, T_N };

typedef struct {
    int w, h, stride;
    int voff;      /* 盒顶相对基线的偏移（负数）：拉丁 = -BASELINE，汉字按盒高收 3 */
} tbl_t;

static const tbl_t TBL[T_N] = {
    { CAL_L48_W, CAL_L48_H, CAL_L48_STRIDE, -CAL_L48_BASELINE },
    { CAL_L32_W, CAL_L32_H, CAL_L32_STRIDE, -CAL_L32_BASELINE },
    { CAL_L24_W, CAL_L24_H, CAL_L24_STRIDE, -CAL_L24_BASELINE },
    { CAL_D32_W, CAL_D32_H, CAL_D32_STRIDE, -CAL_D32_BASELINE },
    { CAL_C32_W, CAL_C32_H, CAL_C32_STRIDE, -(CAL_C32_H - 3) },
};

typedef struct { uint32_t cp; uint8_t tbl; } seg_t;

#define SEG_CAP 40

static const uint8_t *seg_get(int t, uint32_t cp, int *adv)
{
    switch (t) {
    case T_L48: return cal_font_l48_get(cp, adv);
    case T_L32: return cal_font_l32_get(cp, adv);
    case T_L24: return cal_font_l24_get(cp, adv);
    case T_D32: return cal_font_d32_get(cp, adv);
    default:    *adv = CAL_C32_W; return cal_font_c32_get(cp);
    }
}

/* 追加一段 ASCII（拉丁表只烘 ASCII + 空格 / 间隔号 / 斜杠）。 */
static int segs_str(seg_t *s, int cap, int n, int tbl, const char *str)
{
    for (; *str && n < cap; str++, n++) { s[n].cp = (uint32_t)(unsigned char)*str; s[n].tbl = (uint8_t)tbl; }
    return n;
}

static int segs_cps(seg_t *s, int cap, int n, int tbl, const uint32_t *cps, int k)
{
    for (int i = 0; i < k && n < cap; i++, n++) { s[n].cp = cps[i]; s[n].tbl = (uint8_t)tbl; }
    return n;
}

/* 追加一个整数（不补零、不截断） */
static int segs_num(seg_t *s, int cap, int n, int tbl, int v)
{
    char buf[8];
    int k = snprintf(buf, sizeof(buf), "%d", v);
    if (k < 0) k = 0;
    return segs_str(s, cap, n, tbl, buf);
}

static int run_w(const seg_t *s, int n, int gap)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        int adv;
        seg_get(s[i].tbl, s[i].cp, &adv);
        w += adv + ((i + 1 < n) ? gap : 0);
    }
    return w;
}

static void run_draw(uint8_t *fb, const seg_t *s, int n, int u, int base_v, int gap, uint8_t c)
{
    for (int i = 0; i < n; i++) {
        const tbl_t *m = &TBL[s[i].tbl];
        int adv;
        const uint8_t *rows = seg_get(s[i].tbl, s[i].cp, &adv);
        if (rows) {
            const int top = base_v + m->voff;
            for (int y = 0; y < m->h; y++) {
                const uint8_t *r = rows + y * m->stride;
                for (int x = 0; x < m->w; x++)
                    if (r[x >> 3] & (0x80 >> (x & 7))) px(fb, u + x, top + y, c);
            }
        }
        u += adv + gap;
    }
}

/* 居中画一行 */
static void run_center(uint8_t *fb, const seg_t *s, int n, int base_v, int gap, uint8_t c)
{
    run_draw(fb, s, n, MID - run_w(s, n, gap) / 2, base_v, gap, c);
}

/* 定宽大数字：按盒顶对齐、整组从 u0 起排（个位不补零）。 */
static void draw_big_at(uint8_t *fb, int day, int vtop, const uint8_t *digits,
                        int gw, int gh, int gstride, int box_w, int u0, uint8_t c)
{
    const int nd = (day < 10) ? 1 : 2;
    int v = day;
    for (int i = nd - 1; i >= 0; i--) {
        const uint8_t *rows = digits + (v % 10) * gh * gstride;
        const int gu = u0 + i * box_w;
        v /= 10;
        for (int y = 0; y < gh; y++) {
            const uint8_t *r = rows + y * gstride;
            for (int x = 0; x < gw; x++)
                if (r[x >> 3] & (0x80 >> (x & 7))) px(fb, gu + x, vtop + y, c);
        }
    }
}

/* 大号日期：定宽数字表按盒顶对齐、整组水平居中。 */
static void draw_big(uint8_t *fb, int day, int vtop, const uint8_t *digits,
                     int gw, int gh, int gstride, int box_w, uint8_t c)
{
    draw_big_at(fb, day, vtop, digits, gw, gh, gstride, box_w,
                MID - ((day < 10) ? 1 : 2) * box_w / 2, c);
}

/* ---------- 月历算术 ---------- */

static int days_in_month(int y, int m)
{
    static const uint8_t t[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int n = t[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) n = 29;
    return n;
}

/* 本月 1 号落在第几列。列 0 = **周一**（两张参考图都是 M T W T F S S）。 */
static int first_col(int wday, int day)
{
    const int w1 = ((wday - (day - 1)) % 7 + 7) % 7;    /* 1 号是周几（0 = 周日） */
    return (w1 + 6) % 7;
}

/* 列号 -> gmtime 口径的周几（0 = 周日） */
static int col_wday(int col)
{
    return (col + 1) % 7;
}

/* 翻页牌的列 0 = **周日**（参考图是 S M T W T F S），此时列号本身就是周几。 */
static int first_col_sun(int wday, int day)
{
    return ((wday - (day - 1)) % 7 + 7) % 7;
}

/* ---------- 法定节假日（红点 / 黄格） ----------
 * 只有区间表，没有农历算法：设备端算不出春节 / 端午 / 中秋落在公历哪天。
 * 表照 398Inch 那份 2026 年安排移植（元旦 / 春节 / 清明 / 劳动 / 端午 / 中秋 / 国庆），
 * 年底国务院公布下一年安排后往这张表里加几行、重编固件即可；表里没有的年份
 * 就是「没有节假日可标」，周末不自动上色 —— 参考图里也只有一两个红点。
 */
typedef struct { uint16_t y; uint8_t m, d0, d1; } hol_t;

static const hol_t HOLS[] = {
    { 2026,  1,  1,  1 },      /* 元旦 */
    { 2026,  2, 15, 23 },      /* 春节 */
    { 2026,  4,  4,  6 },      /* 清明 */
    { 2026,  5,  1,  5 },      /* 劳动节 */
    { 2026,  6, 19, 21 },      /* 端午 */
    { 2026,  9, 25, 27 },      /* 中秋 */
    { 2026, 10,  1,  7 },      /* 国庆节 */
    { 2027,  1,  1,  1 },      /* 元旦（固定日） */
};

static int is_holiday(int y, int m, int d)
{
    for (size_t i = 0; i < sizeof(HOLS) / sizeof(HOLS[0]); i++)
        if (HOLS[i].y == y && HOLS[i].m == m && d >= HOLS[i].d0 && d <= HOLS[i].d1)
            return 1;
    return 0;
}

/* ---------- 文案表 ---------- */

/* 汉字码点（写死数值，不依赖执行字符集） */
static const uint32_t c_month[] = { 0x6708 };                             /* 月 */
static const uint32_t c_xingqi[] = { 0x661F, 0x671F };                    /* 星期 */
static const uint32_t c_year[] = { 0x5E74 };                              /* 年 */
static const uint32_t c_wd[] = { 0x65E5, 0x4E00, 0x4E8C, 0x4E09,          /* 日 一 二 三 */
                                 0x56DB, 0x4E94, 0x516D };                /* 四 五 六 */
/* D 的图例四项（已过 / 今天 / 节日 / 未至）——汉字码点，缺字会在版面上留空位 */
static const uint32_t c_lg_past[] = { 0x5DF2, 0x8FC7 };                  /* 已过 */
static const uint32_t c_lg_today[] = { 0x4ECA, 0x5929 };                 /* 今天 */
static const uint32_t c_lg_hol[] = { 0x8282, 0x65E5 };                   /* 节日 */
static const uint32_t c_lg_next[] = { 0x672A, 0x81F3 };                  /* 未至 */

static const char *en_monfull[12] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
                                      "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER",
                                      "DECEMBER" };
static const char *en_wday[7] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY",
                                  "THURSDAY", "FRIDAY", "SATURDAY" };
static const char *en_wday1[7] = { "M", "T", "W", "T", "F", "S", "S" };   /* 按列号，周一打头 */
/* 翻页牌的列头按**周日**打头，且必须两字母 —— 单字母 S/T/F 三列会撞车 */
static const char *en_head_sun[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };

/* 行「整月名 + 年」：EN = OCTOBER 2026；ZH = 2026年10月（暖纸块内首行） */
static int ln_mon_year(seg_t *s, const cal_date_t *d, int en)
{
    int n;
    if (en) {
        char buf[20];
        snprintf(buf, sizeof(buf), "%s %d", en_monfull[d->month - 1], d->year);
        n = segs_str(s, SEG_CAP, 0, T_L48, buf);
    } else {
        n = segs_num(s, SEG_CAP, 0, T_L48, d->year);
        n = segs_cps(s, SEG_CAP, n, T_C32, c_year, 1);
        n = segs_num(s, SEG_CAP, n, T_L48, d->month);
        n = segs_cps(s, SEG_CAP, n, T_C32, c_month, 1);
    }
    return n;
}

/* 行「年 / 整月名」：EN = 2026 / OCTOBER；ZH = 2026 / 10月（红格顶行，小一档） */
static int ln_year_mon(seg_t *s, const cal_date_t *d, int en)
{
    int n;
    if (en) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d / %s", d->year, en_monfull[d->month - 1]);
        n = segs_str(s, SEG_CAP, 0, T_L32, buf);
    } else {
        n = segs_num(s, SEG_CAP, 0, T_L32, d->year);
        n = segs_str(s, SEG_CAP, n, T_L32, " / ");
        n = segs_num(s, SEG_CAP, n, T_L32, d->month);
        n = segs_cps(s, SEG_CAP, n, T_C32, c_month, 1);
    }
    return n;
}

/* 行「月名」：EN = OCTOBER；ZH = 10月（用户定的写法：数字 + 月，不用「十月」）。
 * 档位由调用方给 —— 翻页牌右栏只有 211 px，48 档最长的 SEPTEMBER 是 305，只能降 32。 */
static int ln_month(seg_t *s, const cal_date_t *d, int en, int tbl)
{
    int n;
    if (en) n = segs_str(s, SEG_CAP, 0, tbl, en_monfull[d->month - 1]);
    else {
        n = segs_num(s, SEG_CAP, 0, tbl, d->month);
        n = segs_cps(s, SEG_CAP, n, T_C32, c_month, 1);
    }
    return n;
}

/* 行「星期」：EN = MONDAY（档位同上，汉字只有 32 一档）；ZH = 星期一 */
static int ln_weekday(seg_t *s, int wd, int en, int tbl)
{
    int n;
    if (en) n = segs_str(s, SEG_CAP, 0, tbl, en_wday[wd]);
    else {
        n = segs_cps(s, SEG_CAP, 0, T_C32, c_xingqi, 2);
        n = segs_cps(s, SEG_CAP, n, T_C32, &c_wd[wd], 1);
    }
    return n;
}

/* 表头一列的字：EN 单字母 M T W T F S S；ZH 一 二 … 六 日 */
static int ln_head(seg_t *s, int col, int en)
{
    int n;
    if (en) n = segs_str(s, SEG_CAP, 0, T_L32, en_wday1[col]);
    else    n = segs_cps(s, SEG_CAP, 0, T_C32, &c_wd[col_wday(col)], 1);
    return n;
}

/* 翻页牌的表头：列 0 = 周日，列号本身就是周几；EN 用两字母（单字母 S / T 会撞车） */
static int ln_head_sun(seg_t *s, int col, int en)
{
    int n;
    if (en) n = segs_str(s, SEG_CAP, 0, T_L32, en_head_sun[col]);
    else    n = segs_cps(s, SEG_CAP, 0, T_C32, &c_wd[col], 1);
    return n;
}

/* 行高：剩余高度平分给实际行数（不满 6 行的月份因此更疏朗，不会拖出半页空白）。 */
static int row_h_of(int rows, int avail, int cap)
{
    int h = avail / rows;
    if (h > cap) h = cap;
    if (h < 56) h = 56;
    return h;
}

/* ---------- 版面：暖纸 ---------- */

static void draw_warm(uint8_t *fb, const cal_date_t *d, int wd, int en)
{
    seg_t ln[SEG_CAP];
    int n;

    /* 顶部奶黄块：大圆角、满宽减留白，Y/W 斜纹 3:4 混出比参考图更实一档的黄 */
    fill_dither(fb, WM_BLK_U, WM_BLK_V, WM_BLK_W, WM_BLK_H, C_Y, WM_DENS, WM_BLK_R);

    n = ln_mon_year(ln, d, en);
    run_center(fb, ln, n, WM_L1_BASE, 0, C_K);

    draw_big(fb, d->day, WM_BIG_V, &cal_font_b_digits[0][0],
             CAL_B_W, CAL_B_H, CAL_B_STRIDE, CAL_B_W, C_K);

    n = ln_weekday(ln, wd, en, T_L48);
    run_center(fb, ln, n, WM_L3_BASE, 0, C_K);

    /* 表头 */
    const int dim = days_in_month(d->year, d->month);
    const int col0 = first_col(wd, d->day);
    const int rows = (col0 + dim + GRID_COLS - 1) / GRID_COLS;
    for (int i = 0; i < GRID_COLS; i++) {
        n = ln_head(ln, i, en);
        const int u = GRID_U + i * GRID_CELL_W + (GRID_CELL_W - run_w(ln, n, 0)) / 2;
        run_draw(fb, ln, n, u, WM_WD_BASE, 0, C_K);
    }

    /* 月格：无格线。今日 = 黑圆反白（数字位置与邻格一致，不上移）；
       节假日 = 数字下方一个红点，今日那格不再补点（圆已经是最强的标记）。 */
    const int row_h = row_h_of(rows, LH - BOT_MARGIN - WM_GRID_V, WM_ROW_MAX);
    for (int day = 1; day <= dim; day++) {
        const int cell = col0 + day - 1;
        const int row = cell / GRID_COLS, col = cell % GRID_COLS;
        if (row >= rows) break;                     /* 数据不自洽时截断，不越界画 */
        const int cu = GRID_U + col * GRID_CELL_W;
        const int cv = WM_GRID_V + row * row_h;
        const int is_today = (day == d->day);

        char buf[4];
        snprintf(buf, sizeof(buf), "%d", day);
        const int dn = segs_str(ln, SEG_CAP, 0, T_D32, buf);
        const int w = run_w(ln, dn, 0);
        const int dbase = cv + (row_h + NUM_H) / 2;
        const int du = cu + (GRID_CELL_W - w) / 2;
        if (is_today) {
            fill_circle(fb, cu + GRID_CELL_W / 2, dbase - NUM_H / 2, DISC_R, C_K);
        } else if (is_holiday(d->year, d->month, day)) {
            fill_circle(fb, cu + GRID_CELL_W / 2, dbase + DOT_BELOW, DOT_R, C_R);
        }
        run_draw(fb, ln, dn, du, dbase, 0, is_today ? C_W : C_K);
    }
}

/* ---------- 版面：红格 ---------- */

static void draw_grid(uint8_t *fb, const cal_date_t *d, int wd, int en)
{
    seg_t ln[SEG_CAP];
    int n;

    n = ln_year_mon(ln, d, en);
    run_center(fb, ln, n, GD_L1_BASE, 0, C_K);

    draw_big(fb, d->day, GD_BIG_V, &cal_font_b180_digits[0][0],
             CAL_B180_W, CAL_B180_H, CAL_B180_STRIDE, CAL_B180_W, C_R);

    n = ln_weekday(ln, wd, en, T_L48);
    run_center(fb, ln, n, GD_L3_BASE, 0, C_R);

    const int dim = days_in_month(d->year, d->month);
    const int col0 = first_col(wd, d->day);
    const int rows = (col0 + dim + GRID_COLS - 1) / GRID_COLS;

    /* 表头 */
    for (int i = 0; i < GRID_COLS; i++) {
        n = ln_head(ln, i, en);
        const int u = GRID_U + i * GRID_CELL_W + (GRID_CELL_W - run_w(ln, n, 0)) / 2;
        run_draw(fb, ln, n, u, GD_WD_BASE, 0, C_K);
    }

    const int row_h = row_h_of(rows, LH - BOT_MARGIN - GD_GRID_V, GD_ROW_MAX);
    const int gw = GRID_COLS * GRID_CELL_W, gh = rows * row_h;

    /* 外框是大圆角描边，格线只画在格与格之间 —— 最近的内部线离角弧还有一整格
       （74 px），不会被弧切到。 */
    stroke_round_rect(fb, GRID_U, GD_GRID_V, gw, gh, GD_GRID_R, LINE_T, C_K);
    for (int i = 1; i < GRID_COLS; i++)
        fill_rect(fb, GRID_U + i * GRID_CELL_W, GD_GRID_V, LINE_T, gh, C_K);
    for (int j = 1; j < rows; j++)
        fill_rect(fb, GRID_U, GD_GRID_V + j * row_h, gw, LINE_T, C_K);

    /* 今日 = 圆角红底反白；节假日 = 圆角黄底黑字（今日优先）。色块让开格线，
       四角才读得出是圆的。 */
    for (int day = 1; day <= dim; day++) {
        const int cell = col0 + day - 1;
        const int row = cell / GRID_COLS, col = cell % GRID_COLS;
        if (row >= rows) break;
        const int cu = GRID_U + col * GRID_CELL_W;
        const int cv = GD_GRID_V + row * row_h;
        const int is_today = (day == d->day);
        const int pad = LINE_T + CHIP_PAD;

        if (is_today)
            fill_round_rect(fb, cu + pad, cv + pad, GRID_CELL_W - 2 * pad,
                            row_h - 2 * pad, GD_CELL_R, C_R);
        else if (is_holiday(d->year, d->month, day))
            fill_round_rect(fb, cu + pad, cv + pad, GRID_CELL_W - 2 * pad,
                            row_h - 2 * pad, GD_CELL_R, C_Y);

        char buf[4];
        snprintf(buf, sizeof(buf), "%d", day);
        const int dn = segs_str(ln, SEG_CAP, 0, T_D32, buf);
        const int w = run_w(ln, dn, 0);
        const int dbase = cv + (row_h + NUM_H) / 2;
        run_draw(fb, ln, dn, cu + (GRID_CELL_W - w) / 2, dbase, 0, is_today ? C_W : C_K);
    }
}

/* ---------- 版面：翻页牌 ---------- */

static void draw_flap(uint8_t *fb, const cal_date_t *d, int wd, int en)
{
    seg_t ln[SEG_CAP];
    int n;

    /* 整页黑外框 + 底边芥末带（带子盖住外框下沿，参考图就是这么画的） */
    stroke_round_rect(fb, 0, 0, LW, LH, FL_FRAME_R, FL_FRAME_T, C_K);
    fill_rect(fb, 0, LH - FL_BAND_H, LW, FL_BAND_H, C_Y);

    /* 左上黑牌：白色大日期。单日用 b180，双日降 b 才放得下（两枚 b180 会叠墨） */
    fill_round_rect(fb, FL_TX, FL_TY, FL_TW, FL_TH, FL_TR, C_K);
    if (d->day < 10)
        draw_big_at(fb, d->day, FL_TY + (FL_TH - CAL_B180_H) / 2,
                    &cal_font_b180_digits[0][0], CAL_B180_W, CAL_B180_H, CAL_B180_STRIDE,
                    CAL_B180_W, FL_TX + (FL_TW - CAL_B180_W) / 2, C_W);
    else
        draw_big_at(fb, d->day, FL_TY + (FL_TH - CAL_B_H) / 2,
                    &cal_font_b_digits[0][0], CAL_B_W, CAL_B_H, CAL_B_STRIDE,
                    CAL_B_W, FL_TX + (FL_TW - 2 * CAL_B_W) / 2, C_W);

    /* 中缝 + 左右铰粒：牌是黑的，缝读得出来的只有切断白字那一段 */
    const int midv = FL_TY + FL_TH / 2;            /* 209 */
    fill_rect(fb, FL_TX, midv - FL_SEAM / 2, FL_TW, FL_SEAM, C_K);
    fill_round_rect(fb, FL_TX - 10, midv - FL_PIN_H / 2, FL_PIN_W, FL_PIN_H, 6, C_K);
    fill_round_rect(fb, FL_TX + FL_TW - FL_PIN_W + 16, midv - FL_PIN_H / 2,
                    FL_PIN_W, FL_PIN_H, 6, C_K);

    /* 右栏：月名（黑，EN 降 32 档）/ 红年 / 红星期，挤在牌的下沿一带 */
    n = ln_month(ln, d, en, en ? T_L32 : T_L48);
    run_draw(fb, ln, n, FL_RX, FL_MON_BASE, 0, C_K);
    n = segs_num(ln, SEG_CAP, 0, T_L32, d->year);
    run_draw(fb, ln, n, FL_RX, FL_YR_BASE, 0, C_R);
    n = ln_weekday(ln, wd, en, T_L32);
    run_draw(fb, ln, n, FL_RX, FL_WD_BASE, 0, C_R);

    /* 月格：列 0 = 周日、列间细竖线。周末与节假日数字红，今日 = 贴数字的
       描边胶囊，节假日 = 行带底一个红点（与参考图一致：今日若是节假日，点照画） */
    const int dim = days_in_month(d->year, d->month);
    const int col0 = first_col_sun(wd, d->day);
    const int rows = (col0 + dim + GRID_COLS - 1) / GRID_COLS;
    const int rp = (FL_ROW1 - FL_ROW0) / rows;     /* 行距按行数吃满，红点不压下一行 */

    stroke_round_rect(fb, FL_GX, FL_GY, FL_GW, FL_GH, FL_GR, FL_GT, C_K);
    for (int i = 1; i < GRID_COLS; i++)
        fill_rect(fb, FL_GX + FL_PAD + FL_PITCH * i - FL_RULE_T / 2, FL_GY + 52,
                  FL_RULE_T, FL_GH - 66, C_K);

    for (int i = 0; i < GRID_COLS; i++) {
        n = ln_head_sun(ln, i, en);
        const int u = FL_GX + FL_PAD + i * FL_PITCH + (FL_PITCH - run_w(ln, n, 0)) / 2;
        run_draw(fb, ln, n, u, FL_HEAD_BASE, 0, (i == 0 || i == 6) ? C_R : C_K);
    }

    for (int day = 1; day <= dim; day++) {
        const int cell = col0 + day - 1;
        const int row = cell / GRID_COLS, col = cell % GRID_COLS;
        if (row >= rows) break;                    /* 数据不自洽时截断，不越界画 */
        const int bt = FL_ROW0 + rp * row;         /* 行带顶 */
        const int nt = bt + (rp - FL_LANE - NUM_H) / 2;   /* 数字盒顶，底部留点道 */
        const int hol = is_holiday(d->year, d->month, day);
        const int red = hol || col == 0 || col == 6;

        char buf[4];
        snprintf(buf, sizeof(buf), "%d", day);
        const int dn = segs_str(ln, SEG_CAP, 0, T_D32, buf);
        const int w = run_w(ln, dn, 0);
        const int cu = FL_GX + FL_PAD + col * FL_PITCH;
        const int du = cu + (FL_PITCH - w) / 2;

        if (day == d->day)
            stroke_round_rect(fb, du - FL_CHIP_X, nt - FL_CHIP_Y, w + 2 * FL_CHIP_X,
                              NUM_H + 2 * FL_CHIP_Y, FL_CHIP_R, FL_CHIP_T, C_K);
        if (hol)
            fill_circle(fb, cu + FL_PITCH / 2, bt + rp - FL_LANE / 2 - 1,
                        FL_DOT_R, C_R);

        run_draw(fb, ln, dn, du, nt + NUM_H, 0, red ? C_R : C_K);
    }
}

/* ---------- 版面：打卡点阵 ---------- */

static void draw_punch(uint8_t *fb, const cal_date_t *d, int wd, int en)
{
    seg_t ln[SEG_CAP];
    int n;

    /* 顶部黑牌：月（白）+ 年（芥末）同一基线、整组居中 */
    fill_round_rect(fb, PD_PL_U, PD_PL_V, PD_PL_W, PD_PL_H, PD_PL_R, C_K);
    seg_t yr[SEG_CAP];
    const int mtbl = en ? T_L32 : T_L48;
    n = ln_month(ln, d, en, mtbl);
    const int mw = run_w(ln, n, 0);
    const int yn = segs_num(yr, SEG_CAP, 0, mtbl, d->year);
    const int yw = run_w(yr, yn, 0);
    const int x0 = MID - (mw + PD_PL_GAP + yw) / 2;
    run_draw(fb, ln, n, x0, PD_PL_BASE, 0, C_W);
    run_draw(fb, yr, yn, x0 + mw + PD_PL_GAP, PD_PL_BASE, 0, C_Y);

    /* 悬垂红大日期：右缘对齐 507，盒顶 122，墨迹 128..191，上下各让开牌 / 框 */
    draw_big_at(fb, d->day, PD_DAY_V, &cal_font_b64_digits[0][0],
                CAL_B64_W, CAL_B64_H, CAL_B64_STRIDE, CAL_B64_W,
                PD_DAY_U1 - ((d->day < 10) ? 1 : 2) * CAL_B64_W, C_R);

    /* 星期：与大日期同一水平带，左边 */
    n = ln_weekday(ln, wd, en, T_L48);
    run_draw(fb, ln, n, PD_WD_U, PD_WD_BASE, 0, C_K);

    /* 身体大圆角框 + 每行 5 圆按自然日顺序（不按周对齐） */
    stroke_round_rect(fb, PD_BX, PD_BY, PD_BW, PD_BH, PD_BR, PD_BT, C_K);

    const int dim = days_in_month(d->year, d->month);
    const int nrows = (dim + PD_COLS - 1) / PD_COLS;
    const int span = (PD_BOT - PD_TOP) / nrows;
    const int py = (span < PD_PY_MAX) ? span : PD_PY_MAX;   /* 行距 */
    const int cr = (py - 9) / 2;                            /* 圆半径（dia = py - 9） */

    for (int day = 1; day <= dim; day++) {
        const int r = (day - 1) / PD_COLS, c = (day - 1) % PD_COLS;
        const int cx = PD_BX + PD_INSET + PD_PITCH * c + PD_PITCH / 2;
        const int cy = PD_TOP + py * r + py / 2;
        const int hol = is_holiday(d->year, d->month, day);

        uint8_t fg;
        if (day == d->day)     { fill_circle(fb, cx, cy, cr, C_R); fg = C_W; }
        else if (day < d->day) { fill_circle(fb, cx, cy, cr, C_K); fg = C_W; }
        else if (hol)          { fill_circle(fb, cx, cy, cr, C_Y); fg = C_K; }
        else                   { stroke_circle(fb, cx, cy, cr, 3, C_K); fg = C_K; }

        char buf[4];
        snprintf(buf, sizeof(buf), "%d", day);
        const int dn2 = segs_str(ln, SEG_CAP, 0, T_D32, buf);
        run_draw(fb, ln, dn2, cx - run_w(ln, dn2, 0) / 2, cy + NUM_H / 2, 0, fg);
    }

    /* 图例：四颗点与四态对应，整条居中落在框底。EN 用 24 档（32 档会顶穿框） */
    {
        const int ltbl = en ? T_L24 : T_C32;
        const int dr = en ? 8 : 12;
        const uint32_t *zh[4] = { c_lg_past, c_lg_today, c_lg_hol, c_lg_next };
        const char *ea[4] = { "PAST", "TODAY", "HOLIDAY", "NEXT" };
        int tw[4], wsum = 0;

        for (int i = 0; i < 4; i++) {
            const int k = en ? segs_str(ln, SEG_CAP, 0, ltbl, ea[i])
                             : segs_cps(ln, SEG_CAP, 0, ltbl, zh[i], 2);
            tw[i] = run_w(ln, k, 0);
            wsum += 2 * dr + PD_LG_TEXT_GAP + tw[i] + PD_LG_ITEM_GAP;
        }
        wsum -= PD_LG_ITEM_GAP;

        int lu = PD_BX + (PD_BW - wsum) / 2;
        const int dcy = PD_LG_BASE - 12;           /* 点心与文字视觉中心对齐 */
        for (int i = 0; i < 4; i++) {
            if (i == 0)      fill_circle(fb, lu + dr, dcy, dr, C_K);
            else if (i == 1) fill_circle(fb, lu + dr, dcy, dr, C_R);
            else if (i == 2) fill_circle(fb, lu + dr, dcy, dr, C_Y);
            else             stroke_circle(fb, lu + dr, dcy, dr, 3, C_K);

            const int k = en ? segs_str(ln, SEG_CAP, 0, ltbl, ea[i])
                             : segs_cps(ln, SEG_CAP, 0, ltbl, zh[i], 2);
            run_draw(fb, ln, k, lu + 2 * dr + PD_LG_TEXT_GAP, PD_LG_BASE, 0, C_K);
            lu += 2 * dr + PD_LG_TEXT_GAP + tw[i] + PD_LG_ITEM_GAP;
        }
    }
}

/* ---------- 入口 ---------- */

int cal_face_draw(uint8_t *fb, void *user)
{
    if (!fb || !user) return -1;
    const cal_date_t *d = (const cal_date_t *)user;
    if (d->month < 1 || d->month > 12 || d->day < 1 || d->day > 31) return -1;
    const int wd = (d->wday >= 0 && d->wday <= 6) ? d->wday : 0;
    const int en = (d->lang == CAL_LANG_EN);

    memset(fb, 0x55, FB_LEN);                       /* 全白底，不画外框 */

    switch (d->style) {
    case CAL_STYLE_GRID:  draw_grid(fb, d, wd, en);  break;
    case CAL_STYLE_FLAP:  draw_flap(fb, d, wd, en);  break;
    case CAL_STYLE_PUNCH: draw_punch(fb, d, wd, en); break;
    default:              draw_warm(fb, d, wd, en);  break;
    }
    return 0;
}
