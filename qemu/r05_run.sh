#!/bin/sh
# 在 QEMU 里跑要刷的设备 app 本身 (build/r05/<目标>.bin), 底下是假 R05 内核 (qemu/r05_fake.c)。不连设备。
#   sh qemu/r05_run.sh <WAD> [doom|doom_snd] [bench] [maxms=N] [avail=KiB] [maxblk=KiB]
# WAD 传 - 表示不放 (测找不到 WAD 的路径)。WAD 会以 doom1.wad 的名字放进临时目录 (= 设备上的 EFS:/monita/doom1.wad)。
# 产物在 build/r05q/<目标>/: fb.ppm (结束时的 800×1280 帧缓冲), log.txt, frames.txt (bench 时每帧指令数, 不含 CRC)。
set -eu
cd "$(dirname "$0")/.."
WAD=-; [ "$1" = - ] || WAD=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
T=doom; case "${1:-}" in doom|doom_snd) T=$1; shift;; esac
. tools/armflags.sh
[ -f build/r05/$T.bin ] || sh port/r05/build.sh
Q=$(pwd)/build/r05q; O=$Q/$T
mkdir -p $Q/obj $O
python3 -I - port/r05/$T.imports $Q/obj/fake_tab_$T.c <<'PY'
import hashlib, sys
ents = []
for line in open(sys.argv[1]):
    line = line.split("#", 1)[0].strip()
    if not line: continue
    if "=" in line:
        sym, h = (x.strip() for x in line.split("=", 1)); h = int(h, 16)
    else:
        sym, h = line, int.from_bytes(hashlib.sha256(line.encode()).digest()[:4], "big")
    ents.append((h, "fake_" + sym))
o = ["#include <stdint.h>", "struct fake_ent { uint32_t hash; void *fn; };"]
o += ["extern char %s[];" % f for _, f in ents]
o += ["const struct fake_ent fake_tab[] = {"] + ["    { 0x%08xu, %s }," % e for e in ents] + ["};"]
o += ["const uint32_t fake_tab_n = %d;" % len(ents)]
open(sys.argv[2], "w").write("\n".join(o) + "\n")
PY
$ARM_CC $ARM_CFLAGS_PORT -c qemu/r05_fake.c -o $Q/obj/r05_fake.o
$ARM_CC $ARM_CFLAGS_PORT -c $Q/obj/fake_tab_$T.c -o $Q/obj/fake_tab_$T.o
$ARM_CC $ARM_ARCH -c qemu/start.S -o $Q/obj/start.o
arm-none-eabi-ld -T qemu/r05_fake.ld --no-warn-rwx-segments -o $Q/fake_$T.elf $Q/obj/start.o $Q/obj/r05_fake.o $Q/obj/fake_tab_$T.o "$ARM_LIBGCC"
# 插件: 帧 = DG_DrawFrame 入口; dg_frame_crc .. dg_crc_chain 之间 (测速模式算 CRC) 不算
SO=build/qemu/libinsncount.dylib; [ "$(uname)" = Darwin ] || SO=build/qemu/libinsncount.so
[ -f $SO ] || { echo "先跑一次 qemu/run.sh 编插件"; exit 1; }
sym(){ arm-none-eabi-nm build/r05/$T.elf | awk -v s="$1" '$3 == s { print "0x" $1 }'; }
ARGS=""; for a in "$@"; do ARGS="$ARGS,arg=$a"; done
rm -f $O/doom1.wad; [ "$WAD" = - ] || cp "$WAD" $O/doom1.wad
cd $O && rm -f fb.ppm frames.txt
qemu-system-arm -M virt -cpu cortex-a7 -m 1280 -nographic -monitor none -serial none \
  -semihosting-config enable=on,target=native,arg=fake$ARGS \
  -kernel $Q/fake_$T.elf -device loader,file=$(pwd)/../../r05/$T.bin,addr=0x60270000,force-raw=on \
  -plugin "$(cd ../../.. && pwd)/$SO,frame=$(cd ../../.. && sym DG_DrawFrame),pause=$(cd ../../.. && sym dg_frame_crc),resume=$(cd ../../.. && sym dg_crc_chain),out=$O/frames.txt" \
  > log.txt 2>&1 || true
rm -f doom1.wad
tail -4 log.txt
