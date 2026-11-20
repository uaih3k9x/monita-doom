/* qemu virt: RAM 从 0x40000000 起 (run.sh 给 -m 256)。栈 64 KiB 放最后, 前面是堆。 */
ENTRY(_start)
SECTIONS {
    . = 0x40010000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    . = ALIGN(8);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(8);
    __bss_end = .;
    __heap_start = .;
    __heap_end = 0x4FF00000;
    __stack_bottom = 0x4FF00000;
    __stack_top = 0x4FF10000;
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) }
}
