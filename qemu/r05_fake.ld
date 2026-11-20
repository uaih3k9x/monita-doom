/* qemu/r05_fake.c 的链接脚本: 假内核在 0x40010000; 设备 app 自己的地址 (0x60270000 / 0x80de0000 帧缓冲 / 0x80fe0000 RAM 窗口)
   都落在 -m 1280 的 RAM (0x40000000..0x90000000) 里。内核堆从 0x48000000 起。 */
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
    __stack_bottom = 0x41000000;
    __stack_top = 0x41010000;
    __kheap_start = 0x48000000;
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) }
}
