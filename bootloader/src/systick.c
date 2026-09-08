#include <stdint.h>
#include "systick.h"

/* ----------------------------------------------------------------
 * SysTick registers — defined by ARM, identical on all Cortex-M.
 * Fixed addresses in the System Control Block space.
 * ---------------------------------------------------------------- */
#define SYST_CSR    (*(volatile uint32_t *)0xE000E010UL)  /* control  */
#define SYST_RVR    (*(volatile uint32_t *)0xE000E014UL)  /* reload   */
#define SYST_CVR    (*(volatile uint32_t *)0xE000E018UL)  /* current  */

#define CSR_ENABLE      (1U << 0)
#define CSR_TICKINT     (1U << 1)
#define CSR_CLKSOURCE   (1U << 2)   /* 1 = processor clock */

/* The counter is only 24 bits wide. */
#define SYST_MAX_RELOAD 0x00FFFFFFUL


static volatile uint32_t tick_count = 0;


void systick_init(uint32_t cpu_hz)
{
    uint32_t reload = (cpu_hz / 1000U) - 1U;

    if (reload > SYST_MAX_RELOAD) {
        /* Above roughly 16 MHz, a one-millisecond period no longer
           fits in 24 bits. Saturate rather than letting the value
           silently truncate: the time base will be too slow, but
           visibly so. */
        reload = SYST_MAX_RELOAD;
    }

    tick_count = 0;

    SYST_RVR = reload;
    SYST_CVR = 0;               /* any write resets the counter to zero */
    SYST_CSR = CSR_CLKSOURCE | CSR_TICKINT | CSR_ENABLE;
}


void SysTick_Handler(void)
{
    tick_count++;
}


uint32_t millis(void)
{
    return tick_count;
}


void delay_ms(uint32_t ms)
{
    uint32_t start = tick_count;

    /* Overflow-safe form: unsigned subtraction gives the elapsed
       duration even across a rollover to zero. */
    while ((tick_count - start) < ms) {
    }
}


void systick_deinit(void)
{
    SYST_CSR = 0;
    SYST_RVR = 0;
    SYST_CVR = 0;
}
