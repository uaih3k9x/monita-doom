/*
 * 屏上自绘用的 3x5 点阵字库 + 字符映射 + 纯逻辑排版 (设备 port/r05/dg_r05.c 用; 主机单测 port/tests/test_r05_font.c)。
 *   - 每字 15 位, 3 宽 5 高, 行优先、高位在左上: 位 (r,c) = dg_font[g] & (1 << (14 - (r*3+c)))。
 *   - 覆盖 0-9、大写 A-Z、空格和常见符号 (. - : # ! / , ( ) _); 小写折成大写。
 *   - 原先 dg_r05.c 只有 "0-9 . E -" 13 个字模, 只够画状态码 (E2/E3/E4/fps); 扩成能画 I_Error 的英文消息。
 *   - 画的动作 (写帧缓冲、放大、cache) 还在 dg_r05.c (碰硬件); 这里只有能在主机上测的纯逻辑。
 *
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MONITA_DOOM_DG_FONT_H
#define MONITA_DOOM_DG_FONT_H
#include <stdint.h>

/* 索引: 0-9 数字, 10-35 A-Z, 36.. 符号, 47 = 占位符 (实心块, 画字库里没有的字节)。由 tools fontgen.py 生成。 */
#define DG_GLYPH_BOX 47
static const uint16_t dg_font[48] = {
    /* 0-9 */            0075557, 0026227, 0071747, 0071717, 0055711, 0074717, 0074757, 0071111, 0075757, 0075717,
    /* A-M */            0025755, 0065656, 0034443, 0065556, 0074747, 0074744, 0034553, 0055755, 0072227, 0011152, 0055655, 0044447, 0057755,
    /* N-Z */            0057775, 0025552, 0065644, 0075571, 0065655, 0034216, 0072222, 0055557, 0055552, 0055775, 0055255, 0055222, 0071247,
    /* . - : # SP ! / , ( ) _ */ 0000002, 0000700, 0002020, 0057575, 0000000, 0022202, 0011244, 0000024, 0012221, 0042224, 0000007,
    /* 占位符实心块 */   0077777,
};

/* 字符 -> 字模索引; 不认识返回 -1 (空格返回空白字模索引, 占一格)。小写折成大写。 */
static inline int dg_glyph(char ch){
    unsigned char c = (unsigned char)ch;
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
    switch (c){
        case '.': return 36;
        case '-': return 37;
        case ':': return 38;
        case '#': return 39;
        case ' ': return 40;
        case '!': return 41;
        case '/': return 42;
        case ',': return 43;
        case '(': return 44;
        case ')': return 45;
        case '_': return 46;
        default:  return -1;
    }
}

/* 把 msg 按 cols 列 rows 行铺开: 对每个要画的字符回调 fn(ctx, col, row, glyph)。
 *   '\n' 换行; 一行满了自动折行; 行数用满就停 (截断, 不翻页)。
 *   字库里没有的字节 (比如中文 UTF-8) 画占位符 DG_GLYPH_BOX, 不吞掉位置。
 * 返回画出的字符数。保证每次回调 0 <= col < cols 且 0 <= row < rows。 */
typedef void (*dg_cell_fn)(void *ctx, int col, int row, int glyph);
static inline int dg_text_layout(const char *msg, int cols, int rows, dg_cell_fn fn, void *ctx){
    if (cols < 1 || rows < 1 || msg == 0) return 0;
    int col = 0, row = 0, drawn = 0;
    for (const char *p = msg; *p; p++){
        unsigned char c = (unsigned char)*p;
        if (c == '\n'){ col = 0; row++; if (row >= rows) break; continue; }
        if (col >= cols){ col = 0; row++; }
        if (row >= rows) break;
        int g = dg_glyph((char)c);
        if (g < 0) g = DG_GLYPH_BOX;
        if (fn) fn(ctx, col, row, g);
        col++; drawn++;
    }
    return drawn;
}

#endif
