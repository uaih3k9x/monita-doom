/*
 * QEMU 里的"假 R05 内核": 把要刷的 build/r05/doom*.bin 原样跑起来 (qemu/r05_run.sh)。
 *   - 装载: doom.bin 由 QEMU 放在 0x60270000 (-device loader); 这里照 2PPA 规则 (Monita docs/r05-notes.md §3) 装:
 *     查 magic / total_len / crc32, kind1 拷贝、kind3 清零、kind2 桩按哈希填成本文件的假函数 (fake_tab.c, 由 r05_run.sh 生成),
 *     哈希不认识就报错退出; 然后调 enter, 再在一块 16 KiB 的"任务栈"上跑它建的任务。
 *   - 假接口: EFS 走半主机读真文件 (ql_fopen 收到非 "rb" 立即报错退出); 内核堆按真机单缓冲时的大小限额 (可用 4.36 MB);
 *     USB 先注入 HELLO, 之后每 250 ms (虚拟时钟) 一条 PING, 设备发来的 LOG 原样打到控制台;
 *     时钟 = 指令时间 (QEMU -icount, CNTVCT) + sleep 累计 (sleep 不真等, 直接往前拨)。
 *     统计最长喂狗间隔 (从 ql_dev_cfg_wdt 起算, 同 monita-x86 的 qemu/r05sim.c)。
 *   - 结束: 日志里出现"测速:" / "exit " / "不启动" / "找不到 WAD" / "zone malloc 失败", 或虚拟时钟到 maxms:
 *     帧缓冲存 fb.ppm (800×1280), 打印任务栈用量 / 堆峰值 / 看门狗次数, 退出 QEMU。
 * 命令行 (半主机): fake [bench] [safe] [maxms=N] [avail=KiB] [maxblk=KiB] [esc=MS] [hold21=A-B]
 *   safe: 上电时 − (GPIO23) 按住; esc=MS: 虚拟时钟 MS 时注入主机的 Esc 按下, 150 ms 后抬起; hold21=A-B: ≡ (GPIO21) 在 A..B ms 按住
 *
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <stddef.h>

extern int sh_call(int op, void *arg);
extern void call_on_stack(void (*fn)(void *), void *arg, void *sp_top) __attribute__((noreturn));
enum { SYS_OPEN = 1, SYS_CLOSE = 2, SYS_WRITE = 5, SYS_READ = 6, SYS_SEEK = 10, SYS_FLEN = 12, SYS_GET_CMDLINE = 0x15, SYS_EXIT = 0x18 };

/* ---- 小工具 ---- */
static uint32_t slen(const char *s){ uint32_t n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b){ while (*a && *a == *b){ a++; b++; } return *a == *b; }
static int sstarts(const char *s, const char *p){ while (*p) if (*s++ != *p++) return 0; return 1; }
static int scontains(const char *h, uint32_t hn, const char *n){
    uint32_t m = slen(n);
    for (uint32_t i = 0; i + m <= hn; i++){ uint32_t k = 0; while (k < m && h[i + k] == n[k]) k++; if (k == m) return 1; }
    return 0;
}
static uint32_t atou(const char *s){ uint32_t v = 0; while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0'); return v; }
static int g_tt = -1;
static void out(const char *s, uint32_t n){
    if (g_tt < 0){ uint32_t a[3] = { (uint32_t)":tt", 4, 3 }; g_tt = sh_call(SYS_OPEN, a); }
    uint32_t a[3] = { (uint32_t)g_tt, (uint32_t)s, n }; sh_call(SYS_WRITE, a);
}
static void puts_(const char *s){ out(s, slen(s)); }
static void putu(uint32_t v){ char b[12]; int i = 11; b[i] = 0; do { b[--i] = (char)('0' + v % 10u); v /= 10u; } while (v); puts_(b + i); }
static void putx(uint32_t v){ char b[9]; for (int i = 0; i < 8; i++) b[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15u]; b[8] = 0; puts_(b); }
static void finish(int code) __attribute__((noreturn));
static void die(const char *why){ puts_("假内核: "); puts_(why); puts_("\n"); finish(2); }

static uint32_t crc32(const uint8_t *p, uint32_t n){
    uint32_t c = 0xFFFFFFFFu;
    while (n--){ c ^= *p++; for (int k = 0; k < 8; k++) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1; }
    return ~c;
}

/* ---- 配置 / 统计 ---- */
static int g_bench, g_safe;
static uint32_t g_esc_ms, g_h21a, g_h21b;
static uint32_t g_maxms = 60000, g_avail = 4360u << 10, g_maxblk = 4350u << 10;
static uint64_t g_us, g_sleep_us;
static uint32_t g_wdt_last_ms, g_wdt_gap_max, g_wdt_cfg;
static uint32_t g_wdt_feeds, g_uart_writes, g_log_bytes, g_opens;
static uint8_t *g_stack_lo, *g_stack_hi;

/* ---- 时钟 / 任务 / 看门狗 ---- */
static uint64_t cntvct(void){ uint32_t lo, hi; __asm__ volatile("isb; mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi)); return ((uint64_t)hi << 32) | lo; }
static uint32_t cntfrq(void){ uint32_t f; __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f)); return f; }
static void tick(void){ g_us = cntvct() * 1000000u / cntfrq() + g_sleep_us; }
static uint32_t now_ms_(void){ tick(); return (uint32_t)(g_us / 1000u); }
int64_t fake_osiUpTimeUS(void);
static void (*g_task_fn)(void *);
static void *g_task_arg;
static uint32_t g_task_stack;
int fake_ql_rtos_task_create(void *p, uint32_t stack, uint8_t prio, const char *name, void (*fn)(void *), void *arg, uint32_t ev){
    (void)name; (void)ev;
    if (g_task_fn) die("建了第二个任务 (预期只有一个)");
    if (prio != 12) die("任务优先级不是 12");
    *(void **)p = (void *)1;
    g_task_fn = fn; g_task_arg = arg; g_task_stack = stack;
    return 0;
}
int fake_ql_rtos_task_sleep_ms(uint32_t ms){
    g_sleep_us += (uint64_t)ms * 1000u;
    tick();
    if (g_us / 1000u > g_maxms){ puts_("假内核: 虚拟时钟到 maxms\n"); finish(0); }
    return 0;
}
int fake_ql_dev_cfg_wdt(uint32_t o){ g_wdt_cfg = o; g_wdt_last_ms = now_ms_(); return 0; }
int fake_ql_dev_feed_wdt(void){
    uint32_t t = now_ms_();
    if (t - g_wdt_last_ms > g_wdt_gap_max) g_wdt_gap_max = t - g_wdt_last_ms;
    g_wdt_last_ms = t; g_wdt_feeds++;
    return 0;
}

/* ---- 引脚 / GPIO / 屏 / PWRKEY: 记一下就行 ---- */
int fake_ql_pin_set_func(uint32_t pin, uint32_t f){ (void)pin; (void)f; return 0; }
int fake_ql_gpio_init(uint32_t g, uint32_t d, uint32_t p, uint32_t l){ (void)g; (void)d; (void)p; (void)l; return 0; }
int fake_ql_gpio_deinit(uint32_t g){ (void)g; return 0; }
int fake_ql_gpio_set_level(uint32_t g, uint32_t l){ (void)g; (void)l; return 0; }
int fake_ql_gpio_get_level(uint32_t g, uint8_t *lv){
    uint32_t ms = now_ms_();
    int down = (g == 21u && (g_bench || (ms >= g_h21a && ms < g_h21b))) || (g == 23u && g_safe);   /* 21 = ≡, 23 = − */
    *lv = down ? 0u : 1u;
    return 0;
}
int fake_ql_mipi_lcd_init(const void *cfg){ const uint32_t *c = cfg; if (c[0] != 800 || c[1] != 1280) die("lcd_init 参数不对"); return 0; }
static uint32_t g_scan;
int fake_ql_mipi_lcd_write_screen(void *fb){ g_scan = (uint32_t)fb; return 0; }
int fake_r05_lcd_pm_lock(void){ return 0; }
typedef void (*cb0_t)(void);
int fake_ql_pwrkey_shutdown_time_set(uint32_t ms){ (void)ms; return 0; }
int fake_ql_pwrkey_callback_register(cb0_t f){ (void)f; return 0; }
int fake_ql_pwrkey_press_cb_register(cb0_t f){ (void)f; return 0; }
int fake_ql_pwrkey_release_cb_register(cb0_t f){ (void)f; return 0; }

/* ---- 内核堆: 只增长, free 只收回最后一块; 总量按 g_avail 限额, 单块按 g_maxblk ---- */
extern char __kheap_start[];
static char *g_brk = __kheap_start, *g_last;
static uint32_t g_used, g_peak;
void *fake_r05_kmalloc(uint32_t n){
    n = (n + 7u) & ~7u;
    if (n > g_maxblk || g_used + n + 8u > g_avail) return 0;
    char *p = g_brk; *(uint32_t *)p = n; g_brk += n + 8u; g_last = p + 8; g_used += n + 8u;
    if (g_used > g_peak) g_peak = g_used;
    return p + 8;
}
void fake_r05_kfree(void *p){
    if (!p) return;
    uint32_t n = *(uint32_t *)((char *)p - 8);
    if (p == g_last){ g_brk = (char *)p - 8; g_last = 0; }
    g_used -= n + 8u;                                   /* 中间的块空间不回收, 只回收额度 */
}
int fake_ql_dev_memory_size_query(uint32_t *q){
    q[0] = 6600u << 10; q[1] = g_avail - g_used;
    q[2] = g_maxblk < q[1] ? g_maxblk : q[1];
    return 0;
}

/* ---- USB ---- */
typedef void (*uart_cb_t)(uint32_t ind, uint32_t port, uint32_t size);
static uart_cb_t g_ucb;
static uint8_t g_rxq[64]; static uint32_t g_rxn;
static uint64_t g_ping_us;
static int g_hello_sent;
static void inject(uint8_t type, const uint8_t *pl, uint32_t n){
    if (g_rxn + 10u + n > sizeof g_rxq) return;
    uint8_t *f = g_rxq + g_rxn;
    f[0] = 0xA5; f[1] = 0x5A; f[2] = type; f[3] = 0; f[4] = (uint8_t)n; f[5] = 0;
    for (uint32_t i = 0; i < n; i++) f[6 + i] = pl[i];
    uint32_t c = crc32(f + 2, 4 + n);
    f[6 + n] = (uint8_t)c; f[7 + n] = (uint8_t)(c >> 8); f[8 + n] = (uint8_t)(c >> 16); f[9 + n] = (uint8_t)(c >> 24);
    g_rxn += 10u + n;
    if (g_ucb) g_ucb(0x82060001u, 4, g_rxn);
}
static int g_esc_state;
int64_t fake_osiUpTimeUS(void){
    tick();
    if (g_ucb && g_us - g_ping_us >= 250000u){
        g_ping_us = g_us;
        inject(g_hello_sent ? 0x7F : 0x01, 0, 0);
        g_hello_sent = 1;
    }
    if (g_ucb && g_esc_ms && g_esc_state < 2 && g_us / 1000u >= g_esc_ms + (g_esc_state ? 150u : 0u)){
        uint8_t k[2] = { 27, (uint8_t)(g_esc_state ? 0 : 1) };
        inject(0xA0, k, 2);
        g_esc_state++;
    }
    return (int64_t)g_us;
}
int fake_ql_uart_open(uint32_t port){ if (port != 4) die("开了 port 4 以外的口"); return 0; }
int fake_ql_uart_register_cb(uint32_t port, uart_cb_t cb){ (void)port; g_ucb = cb; return 0; }
int fake_ql_uart_read(uint32_t port, void *buf, uint32_t n){
    (void)port;
    uint32_t k = g_rxn < n ? g_rxn : n;
    for (uint32_t i = 0; i < k; i++) ((uint8_t *)buf)[i] = g_rxq[i];
    for (uint32_t i = k; i < g_rxn; i++) g_rxq[i - k] = g_rxq[i];
    g_rxn -= k;
    return (int)k;
}
static char g_log[2048]; static uint32_t g_logn;
static void check_log_line(const char *s, uint32_t n){
    if (scontains(s, n, "测速:") || scontains(s, n, "exit ") || scontains(s, n, "不启动") || scontains(s, n, "安全模式") ||
        scontains(s, n, "找不到 WAD") || scontains(s, n, "zone malloc 失败")) finish(0);
}
int fake_ql_uart_write(uint32_t port, const void *data, uint32_t len){
    (void)port;
    const uint8_t *f = data;
    g_uart_writes++;
    if (len < 10 || f[0] != 0xA5 || f[1] != 0x5A) die("设备发了不是帧的东西");
    uint32_t pl = (uint32_t)f[4] | (uint32_t)f[5] << 8;
    if (pl + 10u != len) die("帧长度不对");
    uint32_t c = crc32(f + 2, 4 + pl);
    if ((uint8_t)c != f[6 + pl] || (uint8_t)(c >> 8) != f[7 + pl] || (uint8_t)(c >> 16) != f[8 + pl] || (uint8_t)(c >> 24) != f[9 + pl]) die("帧 CRC 不对");
    if (f[2] == 0x81){ puts_("[HELLO_ACK "); out((const char *)f + 6, 4); puts_(" flags="); putu(f[12]); puts_("]\n"); return (int)len; }
    if (f[2] != 0xA8) die("设备发了未知类型的帧");
    g_log_bytes += pl;
    out((const char *)f + 6, pl);
    for (uint32_t i = 0; i < pl; i++){                       /* 按行看结束标记 */
        if (g_logn < sizeof g_log) g_log[g_logn++] = (char)f[6 + i];
        if (f[6 + i] == '\n'){ uint32_t n = g_logn; g_logn = 0; check_log_line(g_log, n); }
    }
    return (int)len;
}

/* ---- EFS: 只读, 半主机读当前目录下的同名文件 ---- */
static int g_mounted;
int fake_ql_spi6_ext_nor_flash_init(void){ return 0; }
int fake_ql_spi6_ext_nor_flash_sffs_mount(void){ g_mounted = 1; return 0; }
static struct { int h; uint32_t pos; } g_fd[8];
int fake_ql_fopen(const char *path, const char *mode){
    if (!seq(mode, "rb")) die("ql_fopen 收到了不是 \"rb\" 的模式 (设备上会写外挂 flash!)");
    if (!g_mounted) die("没挂载就 ql_fopen");
    if (!sstarts(path, "EFS:/monita/")) return -1;
    const char *name = path + 12;
    uint32_t a[3] = { (uint32_t)name, 1, slen(name) };
    int h = sh_call(SYS_OPEN, a);
    if (h < 0) return -1;
    for (int i = 1; i < 8; i++) if (!g_fd[i].h){ g_fd[i].h = h + 1; g_fd[i].pos = 0; g_opens++; return i; }
    return -1;
}
static int fdh(int fd){ if (fd < 1 || fd >= 8 || !g_fd[fd].h) die("坏句柄"); return g_fd[fd].h - 1; }
int fake_ql_fread(void *buf, uint32_t size, uint32_t count, int fd){
    int h = fdh(fd);
    if (size != 1) die("ql_fread size != 1");
    uint32_t n = count > 16384u ? 16384u : count;          /* 故意短读, 测循环 */
    uint32_t s[2] = { (uint32_t)h, g_fd[fd].pos }; sh_call(SYS_SEEK, s);
    uint32_t r[3] = { (uint32_t)h, (uint32_t)buf, n };
    int left = sh_call(SYS_READ, r);
    uint32_t got = n - (uint32_t)left;
    g_fd[fd].pos += got;
    g_sleep_us += got / 11u + 20u;                         /* 真机读外挂 flash 的耗时: 按随机 4K 实测 11.7 MB/s 取 11 B/us, 每次调用另加 20 us */
    return (int)got;
}
int fake_ql_fseek(int fd, int32_t off, int32_t whence){ fdh(fd); if (whence != 0 || off < 0) return -1; g_fd[fd].pos = (uint32_t)off; return 0; }
int fake_ql_fclose(int fd){ int h = fdh(fd); uint32_t a[1] = { (uint32_t)h }; sh_call(SYS_CLOSE, a); g_fd[fd].h = 0; return 0; }
int fake_ql_fsize(int fd){ uint32_t a[1] = { (uint32_t)fdh(fd) }; return sh_call(SYS_FLEN, a); }

/* ---- 喇叭 (有声版): 只计数 ---- */
static uint32_t g_pcm_bytes, g_pcm_opens;
int fake_ql_set_audio_path_speaker(void){ return 0; }
void *fake_ql_pcm_open(const uint32_t *cfg, uint32_t flags){
    if (cfg[0] != 1 || cfg[1] != 16000 || flags != 10u) die("pcm_open 参数不对");
    g_pcm_opens++; return (void *)0x1234;
}
int fake_ql_pcm_write(void *h, const void *d, uint32_t n){ (void)h; (void)d; if (!n || n > 16384u) die("pcm_write 长度不对"); g_pcm_bytes += n; return (int)n; }
int fake_ql_pcm_close(void *h){ (void)h; return 0; }
void fake_ql_aud_data_done(void){ }

/* ---- 结束: 存帧缓冲, 打统计 ---- */
static void finish(int code){
    static int done;
    if (done){ uint32_t a[2] = { 0x20026, (uint32_t)code }; sh_call(SYS_EXIT, a); for (;;) {} }
    done = 1;
    uint32_t a[3] = { (uint32_t)"fb.ppm", 5 /* wb */, 6 };
    int h = sh_call(SYS_OPEN, a);
    if (h >= 0){
        static const char hdr[] = "P6\n800 1280\n255\n";
        uint32_t w[3] = { (uint32_t)h, (uint32_t)hdr, sizeof hdr - 1 }; sh_call(SYS_WRITE, w);
        static uint8_t row[800 * 3];
        const uint16_t *fb = (const uint16_t *)0x80de0000u;
        for (uint32_t y = 0; y < 1280; y++){
            for (uint32_t x = 0; x < 800; x++){
                uint16_t p = fb[y * 800 + x];
                row[3 * x] = (uint8_t)((p >> 8) & 0xF8u); row[3 * x + 1] = (uint8_t)((p >> 3) & 0xFCu); row[3 * x + 2] = (uint8_t)(p << 3);
            }
            uint32_t r[3] = { (uint32_t)h, (uint32_t)row, sizeof row }; sh_call(SYS_WRITE, r);
        }
        uint32_t c[1] = { (uint32_t)h }; sh_call(SYS_CLOSE, c);
    }
    uint8_t *p = g_stack_lo;
    while (p < g_stack_hi && *(uint32_t *)p == 0xDEADBEEFu) p += 4;
    puts_("\n假内核: 结束 code="); putu((uint32_t)code);
    puts_("; 任务栈用了 "); putu((uint32_t)(g_stack_hi - p)); puts_(" / "); putu(g_task_stack);
    puts_(" B; 内核堆峰值 "); putu(g_peak); puts_(" B (限额 "); putu(g_avail); puts_(")");
    { uint32_t t = now_ms_(); if (g_wdt_cfg && t - g_wdt_last_ms > g_wdt_gap_max) g_wdt_gap_max = t - g_wdt_last_ms; }
    puts_("; 看门狗 "); putu(g_wdt_feeds); puts_(" 次, 最长喂狗间隔 "); putu(g_wdt_gap_max); puts_(" ms; USB 写 "); putu(g_uart_writes); puts_(" 帧, 日志 "); putu(g_log_bytes);
    puts_(" B; 扫描基址 0x"); putx(g_scan); puts_("; 喇叭 open "); putu(g_pcm_opens); puts_(" 次 / "); putu(g_pcm_bytes); puts_(" B");
    puts_("; 虚拟时钟 "); putu((uint32_t)(g_us / 1000u)); puts_(" ms\n");
    uint32_t e[2] = { 0x20026, (uint32_t)code }; sh_call(SYS_EXIT, e);
    for (;;) {}
}

/* ---- 2PPA 装载 ---- */
struct fake_ent { uint32_t hash; void *fn; };
extern const struct fake_ent fake_tab[];
extern const uint32_t fake_tab_n;
#define BASE 0x60270000u

static void load_and_run(void){
    const uint32_t *h = (const uint32_t *)BASE;
    if (h[0] != 0x41505032u) die("magic 不是 2PPA");
    uint32_t total = h[1];
    if (total <= 0x80u || BASE + total > 0x60370000u) die("total_len 不对");
    uint32_t want = h[2];
    ((uint32_t *)BASE)[2] = 0;
    uint32_t got = crc32((const uint8_t *)BASE, total);
    ((uint32_t *)BASE)[2] = want;
    if (got != want) die("crc32@8 不对");
    for (int s = 0; s < 6; s++){
        const uint32_t *d = h + 8 + 4 * s;
        uint32_t k = d[0], off = d[1], sz = d[2], ad = d[3];
        if (!k || !sz) continue;
        if (k >= 1 && k <= 3 && (ad <= 0x80fdffffu || ad + sz > 0x81000000u)) die("RAM 段不在 app 窗口里");
        if (k == 1 || k == 2){ for (uint32_t i = 0; i < sz; i++) ((uint8_t *)ad)[i] = ((const uint8_t *)BASE)[off + i]; }
        if (k == 3){ for (uint32_t i = 0; i < sz; i++) ((uint8_t *)ad)[i] = 0; }
        if (k == 4 && off != ad - BASE) die("kind4 fileoff != addr - base");
        if (k == 2){
            for (uint32_t i = 0; i < sz; i += 8){
                uint32_t *st = (uint32_t *)(ad + i), hash = st[1];
                if (st[0] != 0xf000f8dfu) die("桩格式不对");
                uint32_t j;
                for (j = 0; j < fake_tab_n && fake_tab[j].hash != hash; j++) {}
                if (j == fake_tab_n){ puts_("假内核: 不认识的导入哈希 "); putx(hash); puts_("\n"); finish(2); }
                st[1] = (uint32_t)fake_tab[j].fn;
            }
        }
    }
    __asm__ volatile("dsb\n isb" ::: "memory");
    int (*enter)(void *) = (int (*)(void *))h[4];
    enter(0);
    if (!g_task_fn) die("enter 没建任务");
    if (g_task_stack < 4096u || g_task_stack > 65536u) die("任务栈大小离谱");
    g_stack_lo = fake_r05_kmalloc(g_task_stack);           /* 栈也从内核堆拿 (真机也是) */
    if (!g_stack_lo) die("任务栈 malloc 失败");
    g_stack_hi = g_stack_lo + g_task_stack;
    for (uint32_t *p = (uint32_t *)g_stack_lo; p < (uint32_t *)g_stack_hi; p++) *p = 0xDEADBEEFu;
    call_on_stack(g_task_fn, g_task_arg, g_stack_hi);
}

static char g_cmd[256];
void qemu_main(void){
    uint32_t a[2] = { (uint32_t)g_cmd, sizeof g_cmd - 1 };
    if (sh_call(SYS_GET_CMDLINE, a) == 0){
        char *s = g_cmd;
        while (*s){
            while (*s == ' ') *s++ = 0;
            if (!*s) break;
            char *w = s;
            while (*s && *s != ' ') s++;
            if (*s) *s++ = 0;
            if (seq(w, "bench")) g_bench = 1;
            else if (sstarts(w, "maxms=")) g_maxms = atou(w + 6);
            else if (sstarts(w, "avail=")) g_avail = atou(w + 6) << 10;
            else if (sstarts(w, "maxblk=")) g_maxblk = atou(w + 7) << 10;
            else if (seq(w, "safe")) g_safe = 1;
            else if (sstarts(w, "esc=")) g_esc_ms = atou(w + 4);
            else if (sstarts(w, "hold21=")){ g_h21a = atou(w + 7); const char *d = w + 7; while (*d && *d != '-') d++; if (*d) g_h21b = atou(d + 1); }
        }
    }
    load_and_run();
}
