#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# 从 3x5 点阵生成 g_font 八进制值; 回算验证位序 (现有 0-9 . E - 必须复现)。
# 位序: g_font[g] & (1 << (14 - (r*3+c))), 行优先, 高位在左上。5 位八进制 = 5 行。

def enc(art):
    rows = art.strip("\n").split("\n")
    assert len(rows) == 5, art
    v = 0
    for r, row in enumerate(rows):
        row = row.replace(" ", "")
        assert len(row) == 3, repr(row)
        for c, ch in enumerate(row):
            if ch == "#":
                v |= 1 << (14 - (r * 3 + c))
    return v

def dec(v):
    out = []
    for r in range(5):
        out.append("".join("#" if v & (1 << (14 - (r * 3 + c))) else "." for c in range(3)))
    return "\n".join(out)

# 现有 13 个值 (dg_r05.c): 验证位序
OLD = {
    "0": 0o75557, "1": 0o26227, "2": 0o71747, "3": 0o71717, "4": 0o55711,
    "5": 0o74717, "6": 0o74757, "7": 0o71111, "8": 0o75757, "9": 0o75717,
    ".": 0o00002, "E": 0o74747, "-": 0o00700,
}

# 我画的点阵 (数字沿用现有, 只是用来回算校验; 字母/符号新画)
ART = {
 "0": "###\n#.#\n#.#\n#.#\n###",
 "1": ".#.\n##.\n.#.\n.#.\n###",
 "2": "###\n..#\n###\n#..\n###",
 "3": "###\n..#\n###\n..#\n###",
 "4": "#.#\n#.#\n###\n..#\n..#",
 "5": "###\n#..\n###\n..#\n###",
 "6": "###\n#..\n###\n#.#\n###",
 "7": "###\n..#\n..#\n..#\n..#",
 "8": "###\n#.#\n###\n#.#\n###",
 "9": "###\n#.#\n###\n..#\n###",
 ".": "...\n...\n...\n...\n.#.",
 "-": "...\n...\n###\n...\n...",
 "E": "###\n#..\n###\n#..\n###",
 # 字母 A-Z
 "A": ".#.\n#.#\n###\n#.#\n#.#",
 "B": "##.\n#.#\n##.\n#.#\n##.",
 "C": ".##\n#..\n#..\n#..\n.##",
 "D": "##.\n#.#\n#.#\n#.#\n##.",
 "F": "###\n#..\n###\n#..\n#..",
 "G": ".##\n#..\n#.#\n#.#\n.##",
 "H": "#.#\n#.#\n###\n#.#\n#.#",
 "I": "###\n.#.\n.#.\n.#.\n###",
 "J": "..#\n..#\n..#\n#.#\n.#.",
 "K": "#.#\n#.#\n##.\n#.#\n#.#",
 "L": "#..\n#..\n#..\n#..\n###",
 "M": "#.#\n###\n###\n#.#\n#.#",
 "N": "#.#\n###\n###\n###\n#.#",
 "O": ".#.\n#.#\n#.#\n#.#\n.#.",   # 圆角, 和方角的 0 区分
 "P": "##.\n#.#\n##.\n#..\n#..",
 "Q": "###\n#.#\n#.#\n###\n..#",
 "R": "##.\n#.#\n##.\n#.#\n#.#",
 "S": ".##\n#..\n.#.\n..#\n##.",
 "T": "###\n.#.\n.#.\n.#.\n.#.",
 "U": "#.#\n#.#\n#.#\n#.#\n###",
 "V": "#.#\n#.#\n#.#\n#.#\n.#.",
 "W": "#.#\n#.#\n###\n###\n#.#",
 "X": "#.#\n#.#\n.#.\n#.#\n#.#",
 "Y": "#.#\n#.#\n.#.\n.#.\n.#.",
 "Z": "###\n..#\n.#.\n#..\n###",
 # 符号
 ":": "...\n.#.\n...\n.#.\n...",
 "#": "#.#\n###\n#.#\n###\n#.#",
 " ": "...\n...\n...\n...\n...",
 "!": ".#.\n.#.\n.#.\n...\n.#.",
 "/": "..#\n..#\n.#.\n#..\n#..",
 ",": "...\n...\n...\n.#.\n#..",
 "(": "..#\n.#.\n.#.\n.#.\n..#",
 ")": "#..\n.#.\n.#.\n.#.\n#..",
 "_": "...\n...\n...\n...\n###",
 "?": "###\n..#\n.##\n...\n.#.",   # 占位符用不到, 但有也无妨
}

# 1) 回算验证: 现有 13 个值的位序必须和我的点阵一致
err = 0
for k, v in OLD.items():
    if enc(ART[k]) != v:
        err += 1
        print("位序不符 %r: 现有 0o%05o, 点阵算出 0o%05o" % (k, v, enc(ART[k])))
        print("现有:\n" + dec(v))
        print("点阵:\n" + dec(enc(ART[k])))
assert err == 0, "位序校验失败"
print("位序校验通过: 现有 13 个值可由点阵复现")

# 2) 生成表 (索引顺序): 0-9, A-Z, 然后符号
order = list("0123456789") + list("ABCDEFGHIJKLMNOPQRSTUVWXYZ") + [".", "-", ":", "#", " ", "!", "/", ",", "(", ")", "_"]
vals = [enc(ART[ch]) for ch in order]

# 3) 两两不同 (空格除外: 空格允许全 0)
seen = {}
for i, ch in enumerate(order):
    if ch == " ":
        continue
    v = vals[i]
    if v in seen:
        print("重复字模! %r 和 %r 都是 0o%05o" % (seen[v], ch, v))
        print(dec(v))
    seen[v] = ch
print("两两不同校验: %d 个非空字模, %d 个唯一值" % (len(order) - 1, len(seen)))

# 4) 打印 C 数组
print("\n--- g_font 数组 (索引: 0-9, A-Z[10-35], 符号) ---")
line = []
for i, ch in enumerate(order):
    line.append("0%05o" % vals[i])
    if len(line) == 13:
        print("    " + ", ".join(line) + ",")
        line = []
if line:
    print("    " + ", ".join(line) + ",")
print("\n索引映射:")
for i, ch in enumerate(order):
    print("  [%2d] %r = 0%05o" % (i, ch, vals[i]))

# 5) 打印所有字形的点阵供肉眼检查
print("\n--- 点阵预览 ---")
for ch in order:
    print("%r:" % ch)
    print(dec(enc(ART[ch])))
    print()
