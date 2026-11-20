#!/usr/bin/env python3
"""qemu/run.sh 的后半段: 读插件写的逐帧指令数, 换算设备帧率 (粗估)。

换算系数:
  LZ4 标定  2.76 ns/条  —— Monita 的 RECT_LZ4 解码: QEMU 每帧 5.0 M 条, 真机 DIAG 33 实测 13.8 ms。
                         那是一段能整个放进 cache 的小循环; Doom 的代码 (几百 KB, 在 Flash 上 XIP) 和数据远大于 cache,
                         真机大概率比这个慢, 所以另给两档:
  CPI 1.0 @ 500 MHz = 2.0 ns/条 (乐观) ;  CPI 1.5 @ 400 MHz = 3.75 ns/条 (悲观)
另外单列"写帧缓冲的 PSRAM 带宽": 2× 每帧写 512 000 B, 2.5× 写 800 000 B, 按 Monita 实测的约 64 MB/s 写带宽算。
第 1 帧含启动 (W_Init / R_Init 等), 单独报, 不进统计。
"""
import sys

NS = [("CPI1.0@500MHz", 2.0), ("LZ4 标定", 2.76), ("CPI1.5@400MHz", 3.75)]
rows = [int(l) for l in open(sys.argv[1]) if l.strip() and not l.startswith("#")]
scale_bytes = int(sys.argv[2]) if len(sys.argv) > 2 else 512000
if len(rows) < 3:
    print("帧数太少: %d" % len(rows)); sys.exit(1)
boot, fr = rows[0], sorted(rows[1:])
n = len(fr)
avg = sum(fr) / n
pct = lambda p: fr[min(n - 1, int(p * n))]
print("帧数 %d (不含第 1 帧) ; 第 1 帧 (启动) %.1f M 条" % (n, boot / 1e6))
print("每帧指令: 平均 %.2f M, 中位 %.2f M, p95 %.2f M, 最多 %.2f M" % (avg / 1e6, pct(0.5) / 1e6, pct(0.95) / 1e6, fr[-1] / 1e6))
bw_ms = scale_bytes / 64e6 * 1e3
print("写帧缓冲带宽下限 ≈ %.1f ms/帧 (%d B @ 64 MB/s, 与指令时间部分重叠)" % (bw_ms, scale_bytes))
print("%-14s %10s %10s %10s %12s" % ("换算", "平均 ms", "平均 fps", "p95 fps", "启动 s"))
for name, ns in NS:
    ms = avg * ns / 1e6
    print("%-14s %10.1f %10.1f %10.1f %12.2f" % (name, ms, 1e3 / ms, 1e3 / (pct(0.95) * ns / 1e6), boot * ns / 1e9))
