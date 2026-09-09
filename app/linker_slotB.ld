/* =========================================================
 * Application — SLOT B
 *
 * A compiled binary is linked to a fixed address: function calls
 * and global variable references are hard-coded into it. This
 * binary therefore only works at the slot B address.
 *
 * This is why the application is compiled twice, once per slot.
 * See PROTOCOL.md, section on discarded alternatives (PIC,
 * copy-to-a-single-execution-slot, hardware bank swap).
 * ========================================================= */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08080000, LENGTH = 480K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
}

/* Boot request flag -- the last word of RAM.
   The address is defined HERE rather than hard-coded in C, because
   the linker script is the only place that knows where RAM ends. A
   change to LENGTH(RAM) then moves the flag and the stack top
   together, instead of silently placing one inside the other.
   See shared/boot_request.h.

   Eight bytes are subtracted from the stack, not four: AAPCS
   requires the stack pointer to be 8-byte aligned at every public
   interface and GCC generates code assuming it. Reserving only four
   would leave _estack at ...FFC to save four bytes out of 96 KB. */
_boot_request = ORIGIN(RAM) + LENGTH(RAM) - 4;
_estack       = ORIGIN(RAM) + LENGTH(RAM) - 8;

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        . = ALIGN(4);
        _etext = .;
    } >FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT> FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } >RAM
}
