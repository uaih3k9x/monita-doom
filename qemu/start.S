/* qemu-system-arm -M virt 裸机入口 (ARM 状态): 栈、清 bss、栈区刷标记 (测栈深)、取半主机命令行, 跳 qemu_main (Thumb)。
 * 代码按 -mfloat-abi=soft 编, 不开 VFP (同设备: validate 不许有 VFP/NEON 指令)。
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later */
    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    ldr   sp, =__stack_top
    ldr   r0, =__bss_start
    ldr   r1, =__bss_end
    mov   r2, #0
1:  cmp   r0, r1
    strlo r2, [r0], #4
    blo   1b
    ldr   r0, =__stack_bottom          @ 栈区 (除了最上面 256 B) 刷 0xDEADBEEF, 退出时数没被碰过的
    ldr   r1, =__stack_top
    sub   r1, r1, #256
    ldr   r2, =0xDEADBEEF
2:  cmp   r0, r1
    strlo r2, [r0], #4
    blo   2b
    ldr   r0, =qemu_main
    blx   r0
3:  b     3b

/* int sh_call(int op, void *arg): ARM 半主机调用 */
    .global sh_call
    .type sh_call, %function
sh_call:
    svc   0x123456
    bx    lr

/* void call_on_stack(fn, arg, sp_top): 换到 sp_top 上调 fn(arg), 不返回 (qemu/r05_fake.c 跑设备任务用) */
    .global call_on_stack
    .type call_on_stack, %function
call_on_stack:
    mov   sp, r2
    mov   r3, r0
    mov   r0, r1
    blx   r3
4:  b     4b
