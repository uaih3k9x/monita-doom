/* port/dg_core.h 的主机单测: 放大 (对照逐像素参考)、按键队列、混音。 cc -std=c99 -Wall -Wextra -Werror */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dg_core.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FBW 800u
#define FBH 1280u
static uint16_t fb[FBW * FBH], ref[FBW * FBH];
static uint8_t src[DGC_W * DGC_H];
static uint16_t lut[256];

static void ref_blit(uint16_t *f, uint32_t x0, uint32_t y0, int num, int den){
    uint32_t w = DGC_W * num / den, h = DGC_H * num / den;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            f[(y0 + y) * FBW + x0 + x] = lut[src[(y * den / num) * DGC_W + x * den / num]];
}

static void test_blit(int mode){
    for (int i = 0; i < 256; i++) lut[i] = (uint16_t)(0x1000 + i * 37);
    srand(1);
    for (size_t i = 0; i < sizeof src; i++) src[i] = (uint8_t)rand();
    for (size_t i = 0; i < FBW * FBH; i++) fb[i] = ref[i] = 0xBEEF;
    dgc_geom_t g = dgc_geom(mode, FBW, FBH);
    if (mode == DGC_SCALE2){ CHECK(g.w == 640 && g.h == 400 && g.x0 == 80 && g.y0 == 120, "geom2 %u %u %u %u", g.w, g.h, g.x0, g.y0); ref_blit(ref, g.x0, g.y0, 2, 1); }
    else { CHECK(g.w == 800 && g.h == 500 && g.x0 == 0 && g.y0 == 70, "geom25 %u %u %u %u", g.w, g.h, g.x0, g.y0); ref_blit(ref, g.x0, g.y0, 5, 2); }
    dgc_blit(fb, FBW, &g, src, lut);
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < FBW * FBH; i++) if (fb[i] != ref[i]){ if (!bad) first = i; bad++; }
    CHECK(bad == 0, "mode %d: %zu 像素不对, 第一个在 (%zu,%zu) got %04x want %04x", mode, bad, first % FBW, first / FBW, fb[first], ref[first]);
}

static void test_lut(void){
    uint8_t rgb[768];
    for (int i = 0; i < 256; i++){ rgb[3*i] = (uint8_t)i; rgb[3*i+1] = (uint8_t)(255 - i); rgb[3*i+2] = (uint8_t)(i * 7); }
    dgc_lut_from_rgb(lut, rgb);
    CHECK(dgc_rgb565(255, 0, 0) == 0xF800, "红 %04x", dgc_rgb565(255, 0, 0));
    CHECK(dgc_rgb565(0, 255, 0) == 0x07E0, "绿");
    CHECK(dgc_rgb565(0, 0, 255) == 0x001F, "蓝");
    for (int i = 0; i < 256; i++) CHECK(lut[i] == dgc_rgb565(rgb[3*i], rgb[3*i+1], rgb[3*i+2]), "lut %d", i);
}

static void test_keys(void){
    dgc_keyq_t q; dgc_keyq_init(&q);
    int down; uint8_t key;
    CHECK(!dgc_key_pop(&q, &down, &key), "空队列");
    CHECK(dgc_key_push(&q, 0xad, 1, DGC_SRC_HOST), "push");
    CHECK(dgc_key_push(&q, 'w', 1, DGC_SRC_HOST), "push");
    CHECK(dgc_key_push(&q, 'w', 0, DGC_SRC_HOST), "push");
    CHECK(dgc_key_push(&q, 0x0d, 1, DGC_SRC_PANEL), "push panel");
    CHECK(dgc_key_pop(&q, &down, &key) && key == 0xad && down == 1, "pop1");
    CHECK(dgc_key_pop(&q, &down, &key) && key == 'w' && down == 1, "pop2");
    CHECK(dgc_key_pop(&q, &down, &key) && key == 'w' && down == 0, "pop3");
    CHECK(dgc_key_pop(&q, &down, &key) && key == 0x0d && down == 1, "pop4");
    /* 主机走了: 只放开主机按住的 (0xad), 面板按住的 (0x0d) 不动 */
    CHECK(dgc_key_release_host(&q) == 1, "release_host 放开 1 个");
    CHECK(dgc_key_pop(&q, &down, &key) && key == 0xad && down == 0, "放开 0xad");
    CHECK(!dgc_key_pop(&q, &down, &key), "之后为空");
    CHECK(dgc_key_release_host(&q) == 0, "再放开 0 个");
    /* 重复按下不重复记; 主机发了抬起才清 */
    dgc_key_push(&q, 'a', 1, DGC_SRC_HOST); dgc_key_push(&q, 'a', 1, DGC_SRC_HOST); dgc_key_push(&q, 'a', 0, DGC_SRC_HOST);
    while (dgc_key_pop(&q, &down, &key)) {}
    CHECK(dgc_key_release_host(&q) == 0, "a 已抬起");
    /* 满了: push 返回 0, 不覆盖 */
    int n = 0;
    while (dgc_key_push(&q, 'x', n & 1, DGC_SRC_HOST)) n++;
    CHECK(n == DGC_KEYQ - 1, "容量 %d", n);
    /* 0 号键 (doom 不用) 被拒 */
    dgc_keyq_init(&q);
    CHECK(!dgc_key_push(&q, 0, 1, DGC_SRC_HOST), "键 0 拒");
}

static void test_mix(void){
    /* DMX 头: u16 3, u16 rate, u32 n (含前后各 16 字节填充), 然后 n 个 u8 */
    static uint8_t lump[8 + 16 + 100 + 16];
    lump[0] = 3; lump[2] = 0x11; lump[3] = 0x2b;           /* 11025 */
    uint32_t n = 16 + 100 + 16; lump[4] = (uint8_t)n;
    for (int i = 0; i < 132; i++) lump[8 + i] = 128;
    for (int i = 0; i < 100; i++) lump[8 + 16 + i] = 255;  /* 满幅正 */
    dgc_mixer_t m; dgc_mix_init(&m);
    CHECK(dgc_mix_start(&m, 2, lump, sizeof lump, 127) == 0, "start");
    CHECK(dgc_mix_playing(&m, 2), "playing");
    CHECK(dgc_mix_start(&m, DGC_MIX_CH, lump, sizeof lump, 127) != 0, "越界通道");
    CHECK(dgc_mix_start(&m, 1, lump, 7, 127) != 0, "太短的 lump 拒");
    int16_t out[400];
    dgc_mix(&m, out, 400);
    /* 100 个 11025 Hz 样本 ≈ 145 个 16 kHz 样本; 之后静音, 通道结束 */
    int nz = 0; for (int i = 0; i < 400; i++) if (out[i]) nz++;
    CHECK(nz >= 144 && nz <= 146, "非零样本 %d", nz);
    CHECK(out[0] > 10000 && out[0] < 16384, "幅度 %d", out[0]);
    CHECK(!dgc_mix_playing(&m, 2), "播完了");
    /* 两路满幅叠加: 钳位在 int16 */
    dgc_mix_start(&m, 0, lump, sizeof lump, 127); dgc_mix_start(&m, 1, lump, sizeof lump, 127);
    dgc_mix_start(&m, 2, lump, sizeof lump, 127); dgc_mix_start(&m, 3, lump, sizeof lump, 127);
    dgc_mix(&m, out, 10);
    CHECK(out[0] == 32767, "钳位 %d", out[0]);
    dgc_mix_stop(&m, 0); dgc_mix_stop(&m, 1); dgc_mix_stop(&m, 2);
    CHECK(!dgc_mix_playing(&m, 0) && dgc_mix_playing(&m, 3), "stop");
    dgc_mix_set_vol(&m, 3, 0);
    dgc_mix(&m, out, 10);
    CHECK(out[0] == 0, "音量 0 -> 静音 %d", out[0]);
    /* 22050 Hz 的 lump: 步长是 11025 的两倍 */
    lump[2] = 0x22; lump[3] = 0x56;
    dgc_mix_init(&m); dgc_mix_start(&m, 0, lump, sizeof lump, 127);
    dgc_mix(&m, out, 400);
    nz = 0; for (int i = 0; i < 400; i++) if (out[i]) nz++;
    CHECK(nz >= 72 && nz <= 74, "22050: 非零样本 %d", nz);
}

int main(void){
    test_lut();
    test_blit(DGC_SCALE2);
    test_blit(DGC_SCALE25);
    test_keys();
    test_mix();
    printf(fails ? "test_core: %d FAIL\n" : "test_core: OK\n", fails);
    return fails != 0;
}
