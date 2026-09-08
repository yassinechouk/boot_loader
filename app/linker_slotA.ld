/* =========================================================
 * Application — SLOT A
 *
 * Un binaire compile est lie a une adresse fixe : les appels
 * de fonction et les references aux variables globales y sont
 * graves en dur. Ce binaire ne fonctionne donc qu'a l'adresse
 * du slot A.
 *
 * C'est pourquoi l'application est compilee deux fois, une
 * fois par emplacement. Voir PROTOCOL.md, section sur les
 * alternatives ecartees (code position-independent, recopie
 * vers un slot d'execution unique, bank swap materiel).
 * ========================================================= */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 480K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

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
