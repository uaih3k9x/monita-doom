#!/bin/sh
# Mac 主机版 (无界面): 同一份引擎 + port/dg_glue.c + port/dg_headless.c, 用系统 cc 编。产物 build/host/doomhost
#   sh port/host/build.sh
set -eu
cd "$(dirname "$0")/../.."
OUT=build/host
mkdir -p $OUT/obj
DEFS="-DDG_HOST -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND -DDG_ZONE_HOOK -DDG_NO_SDL"
CFLAGS="${CFLAGS:--O2 -g} -std=gnu99 -w -Idoomgeneric -Iport $DEFS"
OBJS=""
for f in doomgeneric/*.c port/dg_glue.c port/dg_headless.c; do
  o=$OUT/obj/$(basename "$f" .c).o
  cc $CFLAGS -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
cc -o $OUT/doomhost $OBJS
echo "built $OUT/doomhost"
