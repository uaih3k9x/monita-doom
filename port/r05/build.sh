#!/bin/sh
# 编译 + 打包设备 app，可选静态校验。不连设备、不刷机。
#   sh port/r05/build.sh                             # 只编译 + 打包（默认）
#   MONITA_FW=<Monita 仓库>/fw sh port/r05/build.sh    # 另跑 loader 规则 / 导入守卫校验
# 产物: build/r05/doom.bin (无声版)、build/r05/doom_snd.bin (有声版), 2PPA 容器, 写到 NOR 0x60270000。
# 生成导入桩 / 打包用仓库自带的 tools/r05_gen_stubs.py、tools/r05_pack.py (GPL-2.0)。
# 静态校验可选: 设 MONITA_FW 指向 Monita fw/ 才跑 (要 sdk/validate.py + r05_exports.csv, 不在本仓库, 见 port/r05/README.md)。
set -eu
cd "$(dirname "$0")/../.."
MONITA_FW=${MONITA_FW:-}
. tools/armflags.sh
B=$(pwd)/build/r05
mkdir -p $B/obj
# 引擎 .o 和 QEMU 版同样的选项; 头文件或选项变了就整个重编
if [ -f $B/obj/.stamp ] && [ -z "$(find doomgeneric port tools/armflags.sh -name '*.h' -newer $B/obj/.stamp -o -name armflags.sh -newer $B/obj/.stamp | head -1)" ]; then :; else rm -f $B/obj/*.o; fi
touch $B/obj/.stamp
ENG=""
for f in doomgeneric/*.c; do
  o=$B/obj/$(basename "$f" .c).o
  [ "$o" -nt "$f" ] || $ARM_CC $ARM_CFLAGS_DOOM -c "$f" -o "$o"
  ENG="$ENG $o"
done
PORTF="$ARM_CFLAGS_PORT -Wno-unused-function -Iport/libc -Iport/r05"
$ARM_CC $PORTF -c port/dg_glue.c -o $B/obj/dg_glue.o
$ARM_CC $PORTF -c port/libc/libc.c -o $B/obj/libc.o
for T in doom doom_snd; do
  echo "=== $T"
  X=""; [ $T = doom_snd ] && X=-DDG_SOUND_HW
  $ARM_CC $PORTF $X -c port/r05/dg_r05.c -o $B/obj/dg_r05_$T.o
  python3 -I tools/r05_gen_stubs.py port/r05/$T.imports $B/$T.corestub.S $B/$T.hashes.txt
  $ARM_CC $ARM_ARCH -c $B/$T.corestub.S -o $B/obj/$T.corestub.o
  arm-none-eabi-ld -T port/r05/app.ld --gc-sections -Map=$B/$T.map -o $B/$T.elf \
    $B/obj/dg_r05_$T.o $B/obj/dg_glue.o $B/obj/libc.o $ENG $B/obj/$T.corestub.o "$ARM_LIBGCC"
  arm-none-eabi-size -A $B/$T.elf | awk '$1 ~ /^\.(corestub|text|data|bss)$/ { printf "  %-10s %8d B @ 0x%x\n", $1, $2, $3 }'
  end=$(arm-none-eabi-nm $B/$T.elf | awk '$3 == "__ram_end" { print $1 }')
  echo "  RAM 窗口用到 0x$end (上限 0x81000000)"
  python3 -I tools/r05_pack.py $B/$T.elf $B/$T.bin
  if [ -n "$MONITA_FW" ]; then
    MONITA_FW="$MONITA_FW" python3 tools/r05_validate.py $B/$T.bin $B/$T.elf --target $T
  else
    echo "  (未设 MONITA_FW: 跳过 loader 规则 / 导入守卫静态校验; 要校验设 MONITA_FW=<Monita 仓库>/fw)"
  fi
done
(cd $B && shasum -a 256 doom.bin doom_snd.bin)
