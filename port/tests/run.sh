#!/bin/sh
# 平台层纯逻辑单测 (主机 cc, ASan + UBSan)
set -eu
cd "$(dirname "$0")"
out=$(mktemp)
for t in test_core.c test_libc.c test_r05_link.c test_r05_efs.c; do
  [ -f "$t" ] || continue
  cc -std=c99 -Wall -Wextra -Werror -Wno-unused-function -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o "$out" "$t"
  "$out"
done
rm -f "$out"
