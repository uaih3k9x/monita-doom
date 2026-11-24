/* port/dg_font.h 的主机单测: 字模不变量、字符映射、排版不越界。 cc -std=c99 -Wall -Wextra -Werror
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include "../dg_font.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* 1) 原先 13 个字模的位值必须原样保留 (否则 E2/E3/E4、数字、fps 的显示会变) */
static void test_old_unchanged(void){
    CHECK(dg_font[dg_glyph('0')] == 0075557, "0");
    CHECK(dg_font[dg_glyph('1')] == 0026227, "1");
    CHECK(dg_font[dg_glyph('2')] == 0071747, "2");
    CHECK(dg_font[dg_glyph('3')] == 0071717, "3");
    CHECK(dg_font[dg_glyph('4')] == 0055711, "4");
    CHECK(dg_font[dg_glyph('5')] == 0074717, "5");
    CHECK(dg_font[dg_glyph('6')] == 0074757, "6");
    CHECK(dg_font[dg_glyph('7')] == 0071111, "7");
    CHECK(dg_font[dg_glyph('8')] == 0075757, "8");
    CHECK(dg_font[dg_glyph('9')] == 0075717, "9");
    CHECK(dg_font[dg_glyph('.')] == 0000002, ".");
    CHECK(dg_font[dg_glyph('-')] == 0000700, "-");
    CHECK(dg_font[dg_glyph('E')] == 0074747, "E");
}

/* 2) 所有字模两两不同 (空格 index 40 允许全 0, 占位符 47 也要独一) */
static void test_distinct(void){
    for (int i = 0; i < 48; i++)
        for (int j = i + 1; j < 48; j++){
            if (i == 40 || j == 40) continue;               /* 空格 = 全 0, 跳过 */
            CHECK(dg_font[i] != dg_font[j], "字模 %d 和 %d 相同 (0%05o)", i, j, dg_font[i]);
        }
    CHECK(dg_font[40] == 0, "空格应为全 0");
    CHECK(dg_font[DG_GLYPH_BOX] == 0077777, "占位符应为实心块");
}

/* 3) 字符映射: 小写折成大写, 符号到位, 不认识返回 -1 */
static void test_glyph_map(void){
    for (char c = 'a'; c <= 'z'; c++)
        CHECK(dg_glyph(c) == dg_glyph((char)(c - 'a' + 'A')), "小写 %c 未折成大写", c);
    CHECK(dg_glyph('A') == 10 && dg_glyph('Z') == 35, "A-Z 索引");
    CHECK(dg_glyph(':') == 38 && dg_glyph('#') == 39 && dg_glyph(' ') == 40, "符号索引");
    CHECK(dg_glyph('/') == 42 && dg_glyph('_') == 46, "/ 和 _");
    CHECK(dg_glyph('%') == -1 && dg_glyph('\n') == -1 && dg_glyph((char)0xB7) == -1, "不认识的返回 -1");
}

/* 4) 排版: 任意输入都不越界, '\n' 换行, 列满折行 */
#define COLS 31
#define ROWS 30
static int g_grid[COLS * ROWS];
static int g_oob, g_max_col, g_max_row, g_calls;
static void visit(void *ctx, int col, int row, int glyph){
    (void)ctx;
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS){ g_oob++; return; }
    if (glyph < 0 || glyph >= 48){ g_oob++; return; }
    g_grid[row * COLS + col] = glyph;                        /* 不越界才写; ASan 会抓 */
    if (col > g_max_col) g_max_col = col;
    if (row > g_max_row) g_max_row = row;
    g_calls++;
}
static void layout_once(const char *m){
    memset(g_grid, -1, sizeof g_grid);
    g_oob = g_calls = 0; g_max_col = g_max_row = 0;
    dg_text_layout(m, COLS, ROWS, visit, NULL);
}
static void test_layout_bounds(void){
    /* 最坏输入: 511 字符, 混入 \n 和高位字节 (中文 UTF-8 一类) */
    char worst[512];
    for (int i = 0; i < 511; i++){
        int m = i % 23;
        worst[i] = (m == 0) ? '\n' : (m == 1) ? (char)0xE5 : (char)('A' + m);
    }
    worst[511] = 0;
    layout_once(worst);
    CHECK(g_oob == 0, "最坏输入越界 %d 次", g_oob);
    CHECK(g_max_col < COLS && g_max_row < ROWS, "max col/row 越界 %d/%d", g_max_col, g_max_row);

    /* \n 换行: "AB\nC" 中 C 应在第 1 行第 0 列 */
    layout_once("AB\nC");
    CHECK(g_grid[0 * COLS + 0] == dg_glyph('A'), "A@(0,0)");
    CHECK(g_grid[0 * COLS + 1] == dg_glyph('B'), "B@(0,1)");
    CHECK(g_grid[1 * COLS + 0] == dg_glyph('C'), "C 换行后@(1,0)");

    /* 列满自动折行: 第 COLS 个字符落到下一行第 0 列 */
    char line[COLS + 2];
    for (int i = 0; i < COLS + 1; i++) line[i] = 'A';
    line[COLS + 1] = 0;
    layout_once(line);
    CHECK(g_calls == COLS + 1 && g_max_row == 1, "列满折行: calls=%d max_row=%d", g_calls, g_max_row);

    /* 不认识的字节画占位符, 不吞位置 */
    layout_once("A\xE5""B");                                  /* 中间一个高位字节 */
    CHECK(g_grid[0] == dg_glyph('A') && g_grid[1] == DG_GLYPH_BOX && g_grid[2] == dg_glyph('B'), "占位符占一格");

    /* 退化输入不崩 */
    layout_once("");
    CHECK(g_calls == 0, "空串 0 字符");
    CHECK(dg_text_layout("ABC", 0, 10, visit, NULL) == 0, "cols=0 返回 0");
    CHECK(dg_text_layout(NULL, COLS, ROWS, visit, NULL) == 0, "NULL 返回 0");
}

int main(void){
    test_old_unchanged();
    test_distinct();
    test_glyph_map();
    test_layout_bounds();
    if (fails){ printf("test_r05_font: %d 个检查失败\n", fails); return 1; }
    printf("test_r05_font: OK\n");
    return 0;
}
