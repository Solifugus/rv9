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
 *
 * ---- why the base is not zero ----
 *
 * It was, and that quietly made the position-independence rule stricter
 * than it needs to be. At base 0 the linker relaxes the PC-relative pair
 * that medany emits into a single absolute load, because the address fits
 * in a 12-bit immediate:
 *
 *     linked at 0:          linked at 0x4000:
 *       li    s0,96           auipc s0,0x0
 *                             addi  s0,s0,92     # 406c <TREE>
 *
 * The shipped blob was therefore the one holding an absolute address, and
 * build_modules.sh's double-link check was right to refuse it -- but it
 * refused the *shape* rather than the base, so any named `static const`
 * object failed: a string blob, a byte table, a struct array of inline
 * char[N]. None of them holds a pointer. `dump` gave up a hex lookup table
 * over this and blamed the wrong cause in its own comment.
 *
 * Linking above the immediate's reach removes the whole class, and costs
 * nothing: all 84 modules build byte for byte identically either way,
 * 71,732 bytes in total. (--no-relax also works and costs 9.5%.) The check
 * keeps its teeth -- a table of `const char *` and a table of function
 * pointers still fail, because those hold real addresses in rodata.
 *
 * RV9_BASE is overridable so the probe link can pick a different one
 * without a second copy of this file to keep in step.
 */
ENTRY(rv9_module_entry)

SECTIONS
{
    . = DEFINED(RV9_BASE) ? RV9_BASE : 0x4000;

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
