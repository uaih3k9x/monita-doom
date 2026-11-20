#!/bin/sh
# 主机版回放检查: 对一个 IWAD 跑 -timedemo demo1..3, 打印每段的 gametics 和整段逐帧 CRC 的摘要。
#   sh tools/host_check.sh <iwad 路径>
# 共享版 DOOM1.WAD v1.9 (sha1 5b2e249b9c5133ec987b3ea77596381dc0d6bc1d) 的期望值见 README "主机版验证"。
# QEMU 版 (qemu/run.sh) 打印同样格式的摘要, 两边应逐字一致。
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
wad=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
bin=$here/build/host/doomhost
[ -x "$bin" ] || sh "$here/port/host/build.sh"
tmp=$(mktemp -d)
cd "$tmp"
for d in demo1 demo2 demo3; do
  "$bin" -iwad "$wad" -timedemo $d -crc > out.txt 2> err.txt || true
  tics=$(grep -ho "timed [0-9]* gametics" out.txt err.txt | head -1)
  frames=$(grep -c '^CRC' out.txt || true)
  digest=$(grep '^CRC' out.txt | shasum -a 256 | cut -c1-16)
  echo "$d: $tics, $frames 帧, CRC 摘要 $digest"
done
rm -rf "$tmp"
