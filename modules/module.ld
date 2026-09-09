/*
 * Linker script for RV-9 modules.
 *
 * Produces one blob: entry first, then text, then rodata, all moving as a
 * unit so that -mcmodel=medany PC-relative references stay valid wherever
 * the loader puts it.
 *
 * The assertions at the bottom enforce the reentrancy rule at link time.
 * A module with writable static data would appear to work and then corrupt
 * itself the moment two processes shared it -- far better to refuse to link.
 */
ENTRY(rv9_module_entry)

SECTIONS
{
    . = 0;

    .text : {
        KEEP(*(.text.entry))     /* entry point must be at offset 0 */
        *(.text .text.*)
        . = ALIGN(4);
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(4);
    }

    .data : { *(.data .data.* .sdata .sdata.*) }
    .bss  : { *(.bss .bss.* .sbss .sbss.* COMMON) }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.riscv.attributes)
    }
}

ASSERT(SIZEOF(.data) == 0,
       "RV-9 modules must not have writable static data -- use env->statics")
ASSERT(SIZEOF(.bss) == 0,
       "RV-9 modules must not have BSS -- use env->statics")
