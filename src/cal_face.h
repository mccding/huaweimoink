#ifndef MOINK_CAL_FACE_H
#define MOINK_CAL_FACE_H

#include <stdint.h>

/*
 * 日历版面（R1.5.0 功能1，四套样式）
 *
 *   CAL_STYLE_WARM「暖纸」 参考图 vibe_images/moinkp-b1_1791121171918_484127fb.png
 *     顶部奶黄块（块内三行全黑字：整月名+年 / 大号日期 / 星期）+ 白区无格线月格；
 *     今日 = 黑底反白圆，节假日 = 数字下方红点。
 *   CAL_STYLE_GRID「红格」 参考图 vibe_images/moinkp-a1_1791121137322_2b831f8d.png
 *     白底 + 顶行小字「年 / 月」+ 超大红色日期与红色星期 + 细线格月格；
 *     今日 = 整格红底反白，节假日 = 整格黄底黑字。
 *   CAL_STYLE_FLAP「翻页牌」 参考图 vibe_images/moinkp-c1_1791121231686_be3a5195.png
 *     整页 16 px 黑圆角外框 + 底部芥末带；左上黑圆角牌里白色大日期被一条中缝切断
 *     （左右各一个铰粒），右栏 = 月名 + 红色「年 / 星期」；下半圆角框月格，
 *     **列 0 = 周日**、列间细竖线，周末与节假日数字为红，今日 = 贴字描边胶囊，
 *     节假日 = 数字下方一个红点（点有专属道，压不到下一行）。
 *   CAL_STYLE_PUNCH「打卡点阵」 参考图 vibe_images/moinkp-d1_1791121267322_5034f832.png
 *     顶部黑圆角牌（月面白 / 年面芥末）+ 牌下悬垂的红色大日期 + 左侧星期；
 *     下面大圆角框里**每行 5 个圆按自然日排**（不按周对齐），状态优先级
 *     今日红 > 已过黑 > 节日芥末 > 未至描边（已过优先，连休不会把上半月刷黄）；
 *     框底图例 ● 已过 ● 今天 ● 节日 ○ 未至。
 *   四套共用字模、月历算术和节假日表（cal_face.c 里的 HOLS，按年填区间），
 *   差别在配色、字号档位与「今日 / 节假日」怎么标。
 *   样式名取自情绪板的方案 B1 / A1 / C / D（文件名后缀），但 C 标识符刻意不叫 B1/A1 ——
 *   本项目里 A1 已经是墨水屏驱动模式的名字（a1_mode / 「A1 驱动」），撞名会读晕。
 *
 * 画布是**竖版逻辑坐标**（u 向右 0..551、v 向下 0..767），落进设备帧时按 90°
 * 旋转直接写入 768x552 的 2bpp 缓冲 —— 不存在第二块缓冲，也不做整幅旋转。
 * 设备保持横屏帧契约不变（api=2），用户把机器竖过来贴即可。
 *
 * 字全部来自 tools/make_cal_font.py 烘出的 1bpp 点阵（设备端没有字体引擎）。
 * 一张版面只说一种语言：EN 用拉丁档（l48 / l32 / l24），ZH 用 32 px 汉字 + 数字，
 * 不出现「英文月份 + 中文表头」这种混排。
 */

/* 设备帧几何：与传图 / 轮播同一块缓冲、同一种 2bpp 布局。 */
#define EPD_CAL_W   768
#define EPD_CAL_H   552

/* 版面语言：与 NVS 的 cal_lang 同值，页面 / 固件 / 版面三方共用这一个口径。 */
#define CAL_LANG_ZH 0
#define CAL_LANG_EN 1

/* 版面样式：与 NVS 的 cal_style 同值。默认暖纸（升级后画面不变的那一套）。 */
#define CAL_STYLE_WARM   0
#define CAL_STYLE_GRID   1
#define CAL_STYLE_FLAP   2      /* C 翻页牌 + 竖列周 */
#define CAL_STYLE_PUNCH  3      /* D 打卡点阵 */
#define CAL_STYLE_N      4      /* 合法值 = 0 .. CAL_STYLE_N-1，页面与固件同一口径 */

typedef struct {
    int year;    /* 公历年 */
    int month;   /* 1..12 */
    int day;     /* 1..31 */
    int wday;    /* 0 = 周日（gmtime 口径） */
    int lang;    /* CAL_LANG_ZH / CAL_LANG_EN */
    int style;   /* CAL_STYLE_WARM / GRID / FLAP / PUNCH（0 .. CAL_STYLE_N-1） */
} cal_date_t;

/*
 * 把当天日历画进 2bpp 帧缓冲（长度必须为 EPD_CAL_W/4*EPD_CAL_H = 105984 字节）。
 * 内部自行清底，调用方不必预填。user = const cal_date_t *，不可为 NULL。
 * 成功返回 0。
 */
int cal_face_draw(uint8_t *fb, void *user);

#endif /* MOINK_CAL_FACE_H */
