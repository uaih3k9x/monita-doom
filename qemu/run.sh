#!/bin/sh
# QEMU 里跑 timedemo, 数每帧的指令数, 估算设备帧率 (粗估, 见 README "QEMU 估算")。
#   sh qemu/run.sh <iwad> [demo3] [额外参数...]
# 依赖: arm-none-eabi-gcc, qemu-system-arm (Homebrew, 带 TCG 插件), cc, pkg-config + glib, python3。
# 产物在 build/qemu/: doom.elf, frames_<demo>.txt (每帧指令数), log_<demo>.txt (控制台, 含逐帧 CRC)。
set -eu
cd "$(dirname "$0")/.."
. tools/armflags.sh
WAD=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
DEMO=${2:-demo3}
[ $# -ge 2 ] && shift 2 || shift 1
B=$(pwd)/build/qemu
mkdir -p $B/obj
QEMU=${QEMU:-qemu-system-arm}
QINC=${QEMU_PLUGIN_INC:-$(dirname "$(dirname "$(realpath "$(command -v "$QEMU")")")")/include}
SO=$B/libinsncount.dylib; LDX="-undefined dynamic_lookup"
[ "$(uname)" = Darwin ] || { SO=$B/libinsncount.so; LDX=; }
cc -shared -fPIC -O2 -Wall -Wextra -I"$QINC" $(pkg-config --cflags glib-2.0) $LDX -o "$SO" qemu/insncount.c
# 头文件或编译选项变了就整个重编 (引擎 .o 平时按源文件时间增量)
if [ -f $B/obj/.stamp ] && [ -z "$(find doomgeneric port tools/armflags.sh -name '*.h' -newer $B/obj/.stamp -o -name armflags.sh -newer $B/obj/.stamp | head -1)" ]; then :; else rm -f $B/obj/*.o; fi
touch $B/obj/.stamp
OBJS=""
for f in doomgeneric/*.c; do
  o=$B/obj/$(basename "$f" .c).o
  [ "$o" -nt "$f" ] || $ARM_CC $ARM_CFLAGS_DOOM -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
for f in port/dg_glue.c port/dg_headless.c port/libc/libc.c qemu/backend.c; do
  o=$B/obj/$(basename "$f" .c).o
  $ARM_CC $ARM_CFLAGS_PORT -Iport/libc -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
$ARM_CC $ARM_ARCH -c qemu/start.S -o $B/obj/start.o
arm-none-eabi-ld -T qemu/link.ld --gc-sections --no-warn-rwx-segments -o $B/doom.elf $B/obj/start.o $OBJS "$ARM_LIBGCC"
if arm-none-eabi-objdump -d $B/doom.elf | awk -F'\t' 'NF>=3 && $3 ~ /^v/' | grep -q .; then echo "有 VFP/NEON 指令!"; exit 1; fi
arm-none-eabi-nm $B/doom.elf > $B/doom.nm
sym(){ awk -v s="$1" '$3 == s { print "0x" $1 }' $B/doom.nm; }
WDIR=$(dirname "$WAD")
cd "$WDIR"
"$QEMU" -M virt -cpu cortex-a7 -m 512 -nographic -monitor none -serial none \
  -semihosting-config enable=on,target=native,arg=doom,arg=-iwad,arg=$(basename "$WAD"),arg=-timedemo,arg=$DEMO,arg=-crc$(for a in "$@"; do printf ',arg=%s' "$a"; done) \
  -kernel "$B/doom.elf" \
  -plugin "$SO,frame=$(sym m_frame),pause=$(sym m_pause),resume=$(sym m_resume),out=$B/frames_$DEMO.txt" \
  > "$B/log_$DEMO.txt" 2>&1 || true
cd "$OLDPWD"
grep -v '^CRC [0-9]' $B/log_$DEMO.txt | tail -6
# 和 tools/host_check.sh 同格式的摘要 (ARM 基准见 README "对拍"; 和主机版不逐字一致, 原因也在那里)
tics=$(grep -ho "timed [0-9]* gametics" $B/log_$DEMO.txt | head -1)
echo "$DEMO: $tics, $(grep -c '^CRC [0-9]' $B/log_$DEMO.txt || true) 帧, CRC 摘要 $(grep "^CRC [0-9]" $B/log_$DEMO.txt | shasum -a 256 | cut -c1-16), $(grep -h "^CRC 链" $B/log_$DEMO.txt | cut -d" " -f1-3)"
python3 -I qemu/report.py $B/frames_$DEMO.txt
