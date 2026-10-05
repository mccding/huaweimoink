/*
 * 主机端日历版面预览（不参与固件编译，见 tools/cal_preview.py）。
 *
 *   cc -Isrc -o /tmp/cal_preview tools/cal_preview.c src/cal_face.c src/cal_font.c
 *   /tmp/cal_preview 2026 10 5 1 /tmp/cal.raw 0 1   # 1 = 周一（0 = 周日）
 *                                                   # 语言 0=中 1=英，样式 0=暖纸 1=红格
 */
#include <stdio.h>
#include <stdlib.h>

#include "cal_face.h"

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "用法: %s 年 月 日 周几(0=周日) 输出.raw [语言 0=中 1=英] [样式 0=暖纸 1=红格]\n",
                argv[0]);
        return 2;
    }
    cal_date_t d = { atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                     (argc > 6) ? atoi(argv[6]) : CAL_LANG_ZH,
                     (argc > 7) ? atoi(argv[7]) : CAL_STYLE_WARM };
    size_t len = (size_t)EPD_CAL_W / 4 * EPD_CAL_H;

    uint8_t *fb = malloc(len);
    if (!fb) return 1;
    if (cal_face_draw(fb, &d) != 0) { fprintf(stderr, "cal_face_draw 拒收该日期\n"); return 1; }

    FILE *f = fopen(argv[5], "wb");
    if (!f) { perror(argv[5]); return 1; }
    fwrite(fb, 1, len, f);
    fclose(f);
    free(fb);
    printf("%04d-%02d-%02d wd=%d lang=%d style=%d -> %s (%zu 字节)\n", d.year, d.month, d.day,
           d.wday, d.lang, d.style, argv[5], len);
    return 0;
}
