/* Entry point */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
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
    /* Vector table, at the very start of flash */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    /* Executable code */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        . = ALIGN(4);
        _etext = .;
    } >FLASH

    /* Constants */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    /* Initialised global variables:
       addresses in RAM, initial values stored in FLASH */
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

    /* Uninitialised global variables: zeroed at startup */
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