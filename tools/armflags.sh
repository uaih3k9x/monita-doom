# 设备 app 和 QEMU 测速版共用的交叉编译选项 (sh 片段, 被 build.sh / qemu/run.sh source)。
#   Cortex-A5, Thumb-2, 软浮点 ABI: 不生成任何 VFP/NEON 指令 (fw/sdk/validate.py 会查);
#   引擎里零星的浮点 (配置项、timedemo 结束语) 走 libgcc 的软浮点函数 (nofp multilib, 也没有 VFP 指令)。
#   -mlong-calls: 同 Monita 固件 (代码在 Flash 0x6027xxxx, 内核桩在 RAM 0x80fe0000, BL 够不着)。
#   -fno-tree-loop-distribute-patterns: 不许把循环改写成 memcpy/memset 调用 (libc 自己实现 memcpy 时会递归)。
#   -mno-unaligned-access: 引擎读 WAD 结构用了 packed 结构体, 默认会生成非对齐的 LDR。设备上 SCTLR.A 是否为 0 没证实,
#     QEMU 裸机 (MMU 关) 下非对齐访问直接 fault; 统一不生成非对齐访问, 两边行为一致。
ARM_CC=${ARM_CC:-arm-none-eabi-gcc}
ARM_ARCH="-mcpu=cortex-a5 -mthumb -mfloat-abi=soft"
ARM_GCCINC=$($ARM_CC -print-file-name=include)
ARM_LIBGCC=$($ARM_CC $ARM_ARCH -print-libgcc-file-name)
ARM_DEFS="-DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND -DDG_ZONE_HOOK -DDG_NO_SDL"
ARM_COMMON="$ARM_ARCH -mlong-calls -mno-unaligned-access -ffreestanding -fno-common -ffunction-sections -fdata-sections \
  -fno-tree-loop-distribute-patterns -fno-tree-vectorize -nostdinc -isystem $ARM_GCCINC -Iport/libc/include -Idoomgeneric -Iport $ARM_DEFS"
# 引擎 (上游代码): 关掉警告; 性能要紧, -O2
ARM_CFLAGS_DOOM="$ARM_COMMON -O2 -std=gnu99 -w"
# 我们自己的代码: 警告当错误
ARM_CFLAGS_PORT="$ARM_COMMON -O2 -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter"
