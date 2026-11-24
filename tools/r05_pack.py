#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 monita-doom 维护者
#
# R05 2PPA 打包器（monita-doom 自带，替换原来从 Monita 仓库 fw/sdk 调用的 pack.py）。
#
# 把链接好的 app.elf 打成 R05 app 槽的 2PPA 容器（基址 0x60270000）。只做静态打包，
# 不连设备、不刷机。ELF 直接按 ELF32/小端自己解析（不调 readelf/objcopy，也不落临时文件）。
#
# 容器布局（对着 R05 原厂 app 容器逐字节核对过，原厂容器不在本仓库）：
#   0x00 u32 magic 0x41505032 "2PPA" | 0x04 u32 total_len | 0x08 u32 crc32(整文件@8置0)
#   0x0c u32 version | 0x10 u32 enter | 0x14 u32 exit | 0x18 u32 0 | 0x1c u32 0
#   0x20.. 若干 16 字节装载描述符 (kind, fileoff, size, addr)，kind=0 为终止符：
#     kind=1 RAM 拷贝、kind=2 core_stub 表、kind=3 bss 清零、
#     kind=4 XIP flash 段（要求 fileoff == addr-0x60270000），另有一个 size=0 的 XIP 末尾标记。
#
# 文件内部布局：头 0x80 | corestub@0x80 | data | text@(text_addr-base)。

import struct
import sys
import zlib

BASE = 0x60270000          # app 槽基址
HDR = 0x80                  # 头 + 描述符表共占 0x80 字节
MAGIC = 0x41505032         # "2PPA"
VERSION = 0x00010000       # hi16=1 同原厂内核，lo16=0

SHT_SYMTAB = 2
SHT_NOBITS = 8             # .bss：有大小无文件内容


def align(n, a):
    return (n + a - 1) & ~(a - 1)


class Elf:
    """够用就好的 ELF32（小端）读取器：取出段的 addr/offset/size/内容，以及符号地址。"""

    def __init__(self, path):
        self.blob = open(path, "rb").read()
        if self.blob[:4] != b"\x7fELF":
            raise ValueError("不是 ELF 文件: %s" % path)
        if self.blob[4] != 1 or self.blob[5] != 1:
            raise ValueError("只支持 ELF32 小端")
        (e_shoff,) = struct.unpack_from("<I", self.blob, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", self.blob, 0x2E)
        self._secs = []            # [(name, type, addr, offset, size, link)]
        raw = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            (sh_name, sh_type, _flags, sh_addr, sh_off, sh_size,
             sh_link) = struct.unpack_from("<IIIIIII", self.blob, off)
            raw.append((sh_name, sh_type, sh_addr, sh_off, sh_size, sh_link))
        strtab_off = raw[e_shstrndx][3]
        for sh_name, sh_type, sh_addr, sh_off, sh_size, sh_link in raw:
            name = self._cstr(strtab_off + sh_name)
            self._secs.append((name, sh_type, sh_addr, sh_off, sh_size, sh_link))

    def _cstr(self, off):
        end = self.blob.index(b"\x00", off)
        return self.blob[off:end].decode()

    def section(self, name):
        for sec in self._secs:
            if sec[0] == name:
                return sec            # (name, type, addr, offset, size, link)
        return None

    def content(self, name):
        """段的文件字节；段不存在或为空返回 b""，NOBITS 段也视为无内容。"""
        sec = self.section(name)
        if not sec or sec[4] == 0 or sec[1] == SHT_NOBITS:
            return b""
        _n, _t, _a, off, size, _l = sec
        return self.blob[off:off + size]

    def sym_value(self, want):
        """符号的 st_value（Thumb 函数自带最低位 =1）。"""
        for _n, sh_type, _a, sh_off, sh_size, sh_link in self._secs:
            if sh_type != SHT_SYMTAB:
                continue
            str_off = self._secs[sh_link][3]
            for p in range(sh_off, sh_off + sh_size, 16):
                st_name, st_value = struct.unpack_from("<II", self.blob, p)
                if self._cstr(str_off + st_name) == want:
                    return st_value
        raise KeyError(want)


def pack(elf_path, out_path):
    elf = Elf(elf_path)

    corestub = elf.content(".corestub")
    text = elf.content(".text")
    data = elf.content(".data")
    bss = elf.section(".bss")
    bss_addr = bss[2] if bss else 0
    bss_size = bss[4] if bss else 0

    text_sec = elf.section(".text")
    text_addr = text_sec[2]
    stub_addr = elf.section(".corestub")[2] if corestub else 0x80FE0000
    data_addr = elf.section(".data")[2] if data else 0

    enter = elf.sym_value("appimg_enter")
    exit_ = elf.sym_value("appimg_exit")

    off_stub = HDR
    off_data = align(off_stub + len(corestub), 0x10)
    off_text = text_addr - BASE
    if off_data + len(data) > off_text:
        raise ValueError("corestub+data 挤进了 text 槽，检查 app.ld")

    # 原厂约定：kind4 的 size 不强制对齐，但 total_len / XIP 末尾标记对齐到 0x20。
    total = align(off_text + len(text), 0x20)
    buf = bytearray(total)
    buf[off_stub:off_stub + len(corestub)] = corestub
    if data:
        buf[off_data:off_data + len(data)] = data
    buf[off_text:off_text + len(text)] = text

    descs = []
    if corestub:
        descs.append((2, off_stub, len(corestub), stub_addr))
    descs.append((4, off_text, len(text), text_addr))
    descs.append((4, total, 0, BASE + total))          # XIP 末尾标记
    if data:
        descs.append((1, off_data, len(data), data_addr))
    if bss_size:
        descs.append((3, 0, bss_size, bss_addr))
    descs.append((0, 0, 0, 0))                          # 终止符

    struct.pack_into("<8I", buf, 0,
                     MAGIC, total, 0, VERSION, enter, exit_, 0, 0)
    pos = 0x20
    for desc in descs:
        struct.pack_into("<4I", buf, pos, *desc)
        pos += 16
    if pos > HDR:
        raise ValueError("描述符表超出 0x80 头部")

    crc = zlib.crc32(bytes(buf)) & 0xFFFFFFFF           # @8 此刻为 0
    struct.pack_into("<I", buf, 8, crc)

    with open(out_path, "wb") as fp:
        fp.write(buf)
    print("打包 %s  %d B (%#x)  enter=%#x exit=%#x crc32@8=%08x"
          % (out_path, total, total, enter, exit_, crc))


def main(argv):
    if len(argv) != 2:
        sys.exit("用法: r05_pack.py <app.elf> <out.bin>")
    pack(argv[0], argv[1])


if __name__ == "__main__":
    main(sys.argv[1:])
