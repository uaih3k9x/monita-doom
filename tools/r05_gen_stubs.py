#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 monita-doom 维护者
#
# R05 导入桩生成器（monita-doom 自带，替换原来从 Monita 仓库 fw/sdk 调用的 gen_stubs.py）。
#
# 读一份 .imports 清单，为每个要从 R05 内核导入的符号生成一段 core_stub 汇编，
# 再附带一份 "哈希 符号名" 文本。清单格式（每行一个，'#' 之后是注释）：
#   纯符号名            —— 用符号名本身算哈希
#   符号名 = 0x哈希     —— 该导出没解出名字，直接用这个符号名绑定给定的哈希
#
# 桩的 ABI（对着 R05 原厂 app 反汇编核对过）：每个桩 8 字节，
#   前 4 字节 df f8 00 f0 = Thumb "ldr.w pc, [pc, #0]"
#   后 4 字节 = 导入哈希（小端）。装载时 R05 loader 拿这 4 字节（按小端读成 u32）去
#   AP 导出表里查，查到就把它改写成解析出来的 Thumb 入口地址。
#
# 哈希约定：把符号名的 sha256 取前 4 字节、按大端拼成一个 u32，即为导出表里的键。
# 汇编里用 `.word 哈希` 写出来正好是那 4 个小端字节，loader 再按小端读回就得到同一个键。

import hashlib
import sys

# ldr.w pc, [pc, #0]  —— Thumb-2 编码，低地址在前
LDR_PC = (0xDF, 0xF8, 0x00, 0xF0)


def name_key(name):
    """符号名 -> 导出表键（sha256 前 4 字节按大端拼成的 u32）。"""
    return int.from_bytes(hashlib.sha256(name.encode()).digest()[:4], "big")


def parse_imports(path):
    """解析 .imports，返回按文件顺序排列的 [(符号名, 哈希), ...]。"""
    entries = []
    with open(path, "r") as fp:
        for raw in fp:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if "=" in line:
                name, hexkey = (part.strip() for part in line.split("=", 1))
                entries.append((name, int(hexkey, 16)))
            else:
                entries.append((line, name_key(line)))
    return entries


def emit_asm(entries):
    """把 entries 渲染成 core_stub 汇编。每个桩各占一个 .corestub.<名字> 段，
    段的先后就是文件顺序；app.ld 用未排序的 KEEP(*(.corestub.*)) 原样拼接。"""
    lines = [
        ".syntax unified",
        ".thumb",
        "",
        "@ monita-doom 自动生成的 core_stub 表 -> .corestub @ 0x80fe0000",
        "",
    ]
    bytes_dir = ".byte " + ", ".join("0x%02x" % b for b in LDR_PC)
    for name, key in entries:
        lines += [
            '.section .corestub.%s,"ax",%%progbits' % name,
            ".align 2",
            ".global %s" % name,
            ".thumb_func",
            "%s:" % name,
            "    %s   @ ldr.w pc, [pc, #0]" % bytes_dir,
            "    .word 0x%08x   @ hash(%s)" % (key, name),
            "",
        ]
    return "\n".join(lines) + "\n"


def emit_hashes(entries):
    """文本清单，每行 "哈希 符号名"，顺序同输入。"""
    return "".join("%08x %s\n" % (key, name) for name, key in entries)


def main(argv):
    if len(argv) != 3:
        sys.exit("用法: r05_gen_stubs.py <app.imports> <out.corestub.S> <out.hashes.txt>")
    src, out_s, out_h = argv
    entries = parse_imports(src)
    with open(out_s, "w") as fp:
        fp.write(emit_asm(entries))
    with open(out_h, "w") as fp:
        fp.write(emit_hashes(entries))
    print("生成 %s：%d 个导入桩" % (out_s, len(entries)))


if __name__ == "__main__":
    main(sys.argv[1:])
