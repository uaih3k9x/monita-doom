# monita-doom

把 Doom（[doomgeneric](https://github.com/ozkl/doomgeneric)）原生移植到 Monita-CS80：拉卡拉 CS80 收款机副屏，
EC600U（UIS8910DM，Cortex-A5 约 500 MHz，单核，无浮点可用），原厂 R05 基带，自制 app 跑在 app 槽 `0x60270000`。

原厂收款功能在刷入自制固件后已经不存在，本项目与支付、交易无关。

## 许可证与出处

- 整个仓库按 **GPL-2.0**（见 `LICENSE`）。
- `doomgeneric/`：上游 doomgeneric commit `e257a09`（GPL-2.0），导入提交 `4d69656` 未改动；
  之后的改动都标了 `MONITA-DOOM` 注释（`grep -rn MONITA-DOOM doomgeneric`）。
- `port/`、`qemu/`、`tools/`：本项目新写，GPL-2.0-or-later。
- 从 Monita-CS80-lakala（PolyForm Noncommercial）复用的代码**只限维护者本人写的那部分**，
  由作者本人以 GPL-2.0-or-later 重新授权放进本仓库，逐个文件列在下面的"复用清单"里。
  拉卡拉 / 移远 / 紫光展锐的任何代码和素材都不在本仓库。
- **WAD 不进仓库**（`.gitignore` 拦了 `*.wad`）。用原版共享版 `DOOM1.WAD` 或 Freedoom，自己准备。

复用清单（从 Monita-CS80-lakala 拷入的文件）：见 `port/r05/README.md`。

## 目录

| 路径 | 内容 |
|---|---|
| `doomgeneric/` | 引擎（上游 + 少量改动：大数组改启动时分配、zone 由平台层给、存档失败不退出） |
| `port/dg_core.h` | 平台无关的纯逻辑：调色板→RGB565、2× / 2.5× 放大、按键队列、音效混音（主机单测 `port/tests`） |
| `port/dg_glue.c` | 依赖引擎头文件但和平台无关：大数组分配、画面输出、音效模块 |
| `port/dg_headless.c` | 无界面平台层（Mac 主机版和 QEMU 测速版共用）：虚拟时钟、逐帧 CRC |
| `port/libc/` | 精简 libc（设备和 QEMU 用）：文件只读，printf 进日志 |
| `port/r05/` | 设备平台层（R05 app），见 `port/r05/README.md` |
| `qemu/` | QEMU 裸机测速（`run.sh`）；假 R05 内核跑要刷的 bin 本身（`r05_run.sh`） |
| `host/doomkeys/` | Mac 键盘转发器（Swift，USB 接口 7 直连），顺便显示设备日志 |
| `tools/put_wad.py` | 把 WAD 写进外挂 flash `EFS:/monita/`（要设备上跑着 extdump v2） |

## 构建和测试

```sh
sh port/tests/run.sh                     # 纯逻辑单测 (ASan + UBSan)
sh tools/host_check.sh <DOOM1.WAD>       # Mac 主机版跑 demo1..3, 打印 gametics 和逐帧 CRC 摘要
sh qemu/run.sh <DOOM1.WAD> demo3         # QEMU 里跑 timedemo, 估帧率, 打印同格式的摘要
```

依赖：`cc`、`arm-none-eabi-gcc`、`qemu-system-arm`（Homebrew，带 TCG 插件头文件）、`pkg-config` + `glib`、`python3`。

设备版：

```sh
MONITA_FW=~/code/Monita-CS80-lakala-wt-audio/fw sh port/r05/build.sh   # -> build/r05/doom.bin（无声）、doom_snd.bin（有声），带静态校验
sh qemu/r05_run.sh <DOOM1.WAD> doom bench        # 假 R05 内核跑 doom.bin 本身：测速模式，打印 CRC 链，存 fb.ppm
sh qemu/r05_run.sh <DOOM1.WAD> doom_snd maxms=20000   # 正常模式跑 20 s（虚拟时钟），看喇叭有没有被喂
sh qemu/r05_run.sh - doom                        # 没有 WAD：应停在 E3
sh qemu/r05_run.sh <DOOM1.WAD> doom avail=3600   # 堆不够：应停在 E2
sh host/doomkeys/build.sh && build/doomkeys --selftest
EXTFLASH_HOST=... python3 tools/tests/test_put_wad.py [DOOM1.WAD]  # 上传脚本对着 extdump v2 的内存假设备跑
```

`MONITA_FW` 指向一份 Monita 仓库的 `fw/`，用它的生成导入桩、打包、loader 规则校验脚本和 R05 导出表（不拷进本仓库）。

## 对拍基准

共享版 `DOOM1.WAD` v1.9（sha1 `5b2e249b9c5133ec987b3ea77596381dc0d6bc1d`），zone 2560 KiB（默认值）。
"摘要"= 全部 `CRC <帧> <crc>` 行的 sha256 前 16 位；"链"= 逐帧 CRC 再算一次 CRC-32（`-crc` 退出时打印，设备测速模式也打这一行）。

| demo | gametics | 帧数 | ARM（QEMU）链 | 能否跨平台对拍 |
|---|---|---|---|---|
| demo3 | 2134 | 2173 | `2e1acd90`（摘要 `9a854b489ef804d4`） | **能**：与内存地址无关 |
| demo1 | 5026 | 5065 | `0c8f65aa` | 不能：随 zone 地址变 |
| demo2 | 3836 | 3875 | `b548f425` | 不能：随 zone 地址变 |

Mac 主机版 demo3 链 `45b72299`（摘要 `1a0f6588802377ee`），和 ARM 不同，原因见下。

**为什么只有 demo3 能对**：

- 游戏逻辑在各平台逐 tic 一致（gametics、帧数相同，差异只在个别帧的几个像素，不累积）。
- Doom 原版画纹理时会读过列尾，读到 zone 里相邻的内存，其中有 zone 块头 `memblock_t`，块头里存着指针。
  于是有两种依赖：
  1. **依赖布局**：64 位上块头和关卡结构体更大、对齐不同。实验：把 ARM 端块头垫成 64 位布局后，
     demo3 全部 2173 帧和 Mac 版逐字一致（实验代码未保留）。
  2. **依赖地址值**：读到的是指针本身的字节。实验：QEMU 里在 zone 前面垫 8 / 4096 / 65544 字节，
     demo1、demo2 的链会变，demo3 三次都是 `2e1acd90`；Mac 上同一个二进制连跑 5 次，demo2 出现 3 种结果
     （系统每次运行地址随机），demo1、demo3 5 次都相同。
- 设备版和 QEMU 版编译器、选项、libc、引擎都一样（都是 32 位布局），zone 地址不同。所以真机测速模式
  （`-timedemo demo3`）的链应该是 `2e1acd90`。**真机核对前不算证实。**

定位差异用 `-rowcrc N`：第 N 帧打印调色板 CRC 和每行 CRC，两边 diff 即可。

## 内存预算

真机数据（Monita `docs/r05-notes.md`）：内核堆可用约 4.36 MB，最大连续块 4.35 MB；Monita 日用固件会从堆里拿两块
2 MB 帧缓冲做三缓冲。Doom 模式只用原厂那块帧缓冲（`0x80de0000`，不在堆里），这两块不拿。

| 项 | 大小 | 依据 |
|---|---|---|
| zone | 2560 KiB | ARM 布局下 1200 KiB 已能加载 E1M1–E1M9 并各跑 300 帧（QEMU），取约 2 倍余量 |
| zone 以外的 malloc | 约 136 KiB | QEMU 堆峰值减去 zone 和模拟帧缓冲 |
| 引擎 .data + .bss | 约 65 KiB | 放在 app 的 128 KiB RAM 窗口里，不占内核堆 |
| Doom 合计（堆） | 约 2.76 MB | |
| 留给基带和其它任务 | 约 1.6 MB | 4.36 MB − 2.76 MB |

QEMU 的堆是只增长的简易实现，峰值是**估算**。测到的栈最深约 1.6 KiB 只是 timedemo 的路径，设备任务栈给 32 KiB。

## QEMU 帧率估算

方法：`qemu/run.sh` 用 TCG 插件按帧数指令（含游戏逻辑、渲染、调色板查表放大），再按三档每条指令耗时换算。
QEMU 不模拟 cache 和 XIP flash 取指，所以这只是**粗估**，真机测出来才算数。

timedemo（不限速，每 tic 一帧），每帧指令平均 1.65–1.70 M，p95 约 2.0–2.1 M：

| 换算档 | 每帧指令时间 | + 写帧缓冲 | 合计 | 折合 |
|---|---|---|---|---|
| 乐观（CPI 1.0 @ 500 MHz） | 3.4 ms | 8.0 ms | 约 11.4 ms | 约 88 fps |
| LZ4 标定（2.76 ns/条） | 4.7 ms | 8.0 ms | 约 12.7 ms | 约 79 fps |
| 悲观（CPI 1.5 @ 400 MHz） | 6.4 ms | 8.0 ms | 约 14.4 ms | 约 69 fps |

写帧缓冲一项按 2× 放大每帧写 512 000 B、PSRAM 写带宽约 64 MB/s 算，和指令时间有部分重叠，这里直接相加取保守值。
Doom 游戏逻辑本身封顶 35 fps，所以估算结论是**能跑满 35 fps，余量约一倍**。没计入 XIP 取指和 cache 未命中，
Doom 的代码和数据都远大于 cache，真机可能明显更慢。

## 设备版

- 单独的 app，刷进 app 槽 `0x60270000`，替换掉日用的 Monita 固件；玩完刷回去。
- 无声版 `doom.bin` 不导入任何喇叭接口；有声版 `doom_snd.bin` 用 Monita fw v13 的喇叭路径，**那条路径本身还没上过真机**，先刷无声版。
- 导入清单只有读类接口：外挂 flash 只挂载、只用 `"rb"` 打开；写 / 擦 / 格式化 / 写 NV 的接口一个都没有（构建时逐个点名校验）。
  存档和改设置都存不了，菜单里存档会提示 `CANNOT SAVE: READ-ONLY STORAGE`。
- WAD 依次找 `EFS:/monita/doom1.wad`、`EFS:/monita/freedoom1.wad`。共享版 `DOOM1.WAD` 4.0 MB 放得下（EFS 剩约 14 MiB）。
  Freedoom 据我所知 20 MB 以上，大概率放不下（没核实；上传脚本写前会查空间）。

屏幕下方的大字是状态：

| 显示 | 意思 |
|---|---|
| `-`（白） | 开机先等 3 s：万一后面崩了反复重启，刷机工具还有窗口进下载模式 |
| `E2`（红） | 内核堆不够：最大块放不下 2560 KiB 的 zone，或分完剩下不到 1.25 MiB |
| `E3`（红） | 找不到 WAD |
| `E4`（红） | Doom 出错退出（原因在 USB 日志里） |
| 数字（绿） | 测速模式的结果（fps） |

按键：

| | Mac（`build/doomkeys` 窗口在前台） | 前面板 |
|---|---|---|
| 前进 / 后退 | ↑ / ↓ | `+` / `−` |
| 转身 | ← / → | — |
| 开火 | Ctrl | 查询键 |
| 开门 / 确认 | 空格 / 回车 | `≡`（同时发两个） |
| 菜单 | Esc | 长按 `≡` |
| 跑 / 平移 | Shift / Option | — |

测速模式：开机时按住 `≡`，直到中间的 `-` 消失。跑 `-timedemo demo3`（不限速），结束时屏上显示帧率，
USB 日志里打 `CRC 链 ...` 和帧数、耗时。`CRC 链` 应该是 `2e1acd90 2173`。帧率里包含每帧多算的一次 64 KB CRC
（约 0.3 M 条指令，1 ms 上下），所以实际游戏会比读数略快。

QEMU 假内核跑 `doom.bin` 本身（测速模式）的结果：CRC 链 `2e1acd90`（和 QEMU 测速版一致），任务栈最深 1700 / 16384 B，
内核堆峰值 2.78 MB（含 16 KiB 任务栈），每帧平均 1.78 M 条指令（比测速版多 0.08 M：D-cache clean 和 USB 轮询）。

## 上真机

由维护者亲手执行。三次刷写都只写 app 槽 `0x60270000`；EFS 只写 `EFS:/monita/`。

1. 刷 extdump v2（维护者私有工具目录里 staged 的 `extdump_v2.bin`，sha256 `16264e24…`）。
2. 上传 WAD：`EXTFLASH_HOST=<extdump v2 工具>/host python3 tools/put_wad.py --port COM5 DOOM1.WAD`（ThinkPad）。写速实测 183 KB/s，4 MB 约 25–60 s；
   读回校验约 74 KB/s，再约 1 分钟。
3. 刷 `build/r05/doom.bin`（无声版）。
4. 上电：`-` 3 s → 标题画面。Mac 上先停日用上位机（它占着 USB 接口 7），再开 `build/doomkeys`。
5. 测速：断电重上电时按住 `≡`，记下屏上帧率和日志里的 `CRC 链`。
6. 想听声音再刷 `doom_snd.bin`；玩完刷回日用固件。

万一刷进去以后开机就反复重启：3 s 窗口里刷机工具应该还能让模组进下载模式；再不行用整片备份
`full_flash_0x60000000_8MiB_9c81936b.bin` 写回（见 Monita `docs/r05-notes.md`）。

## 还没做

- **撕裂**：单缓冲、不等帧头，写 2× 画面约 8 ms，屏幕扫一帧 26.6 ms，会看到撕裂。可以做"追光写入"（等帧事件后从扫描线后面写）。
- 音乐（OPL / MIDI 合成在这颗 CPU 上不划算）。
- 存档（EFS 只读）。
- 2.5× 放大（`dg_core.h` 有，设备上没接）。
- 段码屏和状态灯没用上。
- Mac 转发器只在没有设备的情况下编译、自检过；Windows / Linux 没有转发器。
