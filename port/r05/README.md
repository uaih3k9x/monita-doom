# port/r05：设备平台层（R05 app）

## 复用清单

`monita/` 下的文件**原样**拷自 Monita-CS80-lakala 仓库 `fw-audio` 分支提交
`16070935162c0c6b4d3d285db06400651c01381c`（fw v13）。这几个文件全部由 Monita-CS80 维护者本人编写
（`git log` 只有维护者一个作者），由作者本人以 GPL-2.0-or-later 授权放进本仓库；Monita 仓库里它们仍按原协议。
不含任何原厂（拉卡拉 / 移远 / 紫光展锐）代码，只有维护者对照原厂调用点整理出的函数原型和常量。

| 本仓库 | Monita 原路径 | 用途 |
|---|---|---|
| `monita/r05_api.h` | `fw/sdk/r05_api.h` | R05 导入函数原型 |
| `monita/lcd.h` | `fw/lcd/lcd.h` | 大屏上电、背光、D-cache clean |
| `monita/lcdc.h` | `fw/lcd/lcdc.h` | LCDC 寄存器（本程序没用到等帧） |
| `monita/proto.h` | `fw/link/proto.h` | 帧格式、CRC、流式拆帧 |
| `monita/keys.h` | `fw/link/keys.h` | 按键去抖 / 长按 / PWRKEY |
| `monita/keys_hw.h` | `fw/io/keys_hw.h` | 前面板 4 键硬件层 |
| `monita/audio.h` | `fw/link/audio.h` | 喇叭 PCM 环形缓冲和内核管道水位估计 |
| `monita/audio_hw.h` | `fw/io/audio_hw.h` | 功放脚 + 内核 PCM 播放 |

`dg_efs.h` 里 `ql_f*` 的用法（`"rb"`、返回 > 0 有效）照维护者的 extdump v2（真机跑过）。

构建用的脚本（生成导入桩、打包 2PPA、静态校验）和 R05 导出表**不拷**：导出表是从原厂固件里提取的数据。
`build.sh` 通过环境变量 `MONITA_FW` 指向一份 Monita 的 `fw/` 目录来调用它们。

## 文件

| 文件 | 内容 |
|---|---|
| `dg_link.h` | USB 链路（HELLO / PING / KEY / LOG）、前面板键位、zone 预算（纯逻辑，主机单测） |
| `dg_efs.h` | EFS 只读文件（纯逻辑，主机单测） |
| `dg_r05.c` | 设备平台层：入口、任务、画面、计时、输入、喇叭、libc 后端 |
| `app.ld` | 链接脚本 |
| `doom.imports` / `doom_snd.imports` | 无声版 / 有声版导入清单 |
| `build.sh` | 编译 + 打包 + 校验，产物 `build/r05/doom.bin`、`doom_snd.bin` |
