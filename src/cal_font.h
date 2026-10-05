/* 由 tools/make_cal_font.py 生成，勿手改（改字号 / 字集 = 重跑脚本 + 重编固件）。
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

#define CAL_B_W      80
#define CAL_B_H      120
#define CAL_B_STRIDE 10
extern const uint8_t cal_font_b_digits[10][1200];
#define CAL_B180_W      124
#define CAL_B180_H      180
#define CAL_B180_STRIDE 16
extern const uint8_t cal_font_b180_digits[10][2880];
#define CAL_B64_W      52
#define CAL_B64_H      76
#define CAL_B64_STRIDE 7
extern const uint8_t cal_font_b64_digits[10][532];
#define CAL_C32_W      32
#define CAL_C32_H      32
#define CAL_C32_STRIDE 4
/* 按码点查表，缺字返回 NULL（调用方留空位，不会画花屏）。 */
const uint8_t *cal_font_c32_get(uint32_t cp);
#define CAL_L48_W      48
#define CAL_L48_H      48
#define CAL_L48_STRIDE 6
#define CAL_L48_BASELINE 36
/* 取字形点阵（盒内已按基线摆好）并给出本字推进宽度；缺字返回 NULL。 */
const uint8_t *cal_font_l48_get(uint32_t cp, int *adv);
#define CAL_L32_W      32
#define CAL_L32_H      32
#define CAL_L32_STRIDE 4
#define CAL_L32_BASELINE 24
/* 取字形点阵（盒内已按基线摆好）并给出本字推进宽度；缺字返回 NULL。 */
const uint8_t *cal_font_l32_get(uint32_t cp, int *adv);
#define CAL_L24_W      24
#define CAL_L24_H      24
#define CAL_L24_STRIDE 3
#define CAL_L24_BASELINE 18
/* 取字形点阵（盒内已按基线摆好）并给出本字推进宽度；缺字返回 NULL。 */
const uint8_t *cal_font_l24_get(uint32_t cp, int *adv);
#define CAL_D32_W      32
#define CAL_D32_H      32
#define CAL_D32_STRIDE 4
#define CAL_D32_BASELINE 24
/* 取字形点阵（盒内已按基线摆好）并给出本字推进宽度；缺字返回 NULL。 */
const uint8_t *cal_font_d32_get(uint32_t cp, int *adv);

#endif /* MOINK_CAL_FONT_H */
