/* monita-doom 的 R05 链接脚本 (改自 Monita fw/sdk/app.ld, 同一维护者)。
     .corestub -> RAM 0x80fe0000 (loader 按哈希把地址填进桩)
     .text/.rodata -> XIP flash 0x60278000 (Monita 是 0x60271280: 那里离文件头只有 4.6 KiB, 放不下 Doom 20 KiB 的 .data;
                      往后挪到 32 KiB 处。loader 只要求 kind4 段 fileoff == addr - 0x60270000, 见 Monita docs/r05-notes.md §3)
     .data -> RAM 0x80fe0640 (loader kind1 拷贝)   .bss -> 紧跟其后 (loader kind3 清零)
   RAM 窗口 0x80fe0000..0x81000000 共 128 KiB, 超了链接时报错。 */
ENTRY(appimg_enter)
EXTERN(appimg_exit)

MEMORY {
  STUB  (rwx) : ORIGIN = 0x80fe0000, LENGTH = 0x00000640
  RAM   (rw)  : ORIGIN = 0x80fe0640, LENGTH = 0x00020000 - 0x640
  FLASH (rx)  : ORIGIN = 0x60278000, LENGTH = 0x000a09a0 - 0x8000
}

SECTIONS {
  .corestub 0x80fe0000 : { KEEP(*(.corestub.*)) } > STUB

  .text 0x60278000 : {
    KEEP(*(.text.appimg_enter))
    KEEP(*(.text.appimg_exit))
    *(.text .text.* .glue_7 .glue_7t)
    *(.rodata .rodata.*)
    *(.ARM.extab*)
  } > FLASH

  .data 0x80fe0640 : { *(.data .data.*) } > RAM
  .bss (NOLOAD) : ALIGN(32) { *(.bss .bss.* COMMON) } > RAM
  __ram_end = .;

  /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) *(.ARM.attributes) }
}
