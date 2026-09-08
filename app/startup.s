.syntax unified
.cpu cortex-m4
.fpu softvfp
.thumb

/* =========================================================
   RESET HANDLER — premier code execute apres le reset
   ========================================================= */
    .section  .text.Reset_Handler
    .weak     Reset_Handler
    .type     Reset_Handler, %function

Reset_Handler:
    /* 1. Charger le stack pointer.
          Le processeur l'a deja fait depuis la vector table ; cette
          ligne garantit un etat correct si l'on arrive ici autrement
          qu'au reset. */
    ldr   sp, =_estack

    /* 2. Copier .data de la FLASH vers la RAM.
          Les valeurs initiales des variables globales sont stockees
          en flash ; leur emplacement d'execution est en RAM. */
    movs  r1, #0
    b     LoopCopyDataInit

CopyDataInit:
    ldr   r3, =_sidata          /* adresse source en flash    */
    ldr   r3, [r3, r1]          /* lire un mot de 32 bits     */
    str   r3, [r0, r1]          /* l'ecrire en RAM            */
    adds  r1, r1, #4

LoopCopyDataInit:
    ldr   r0, =_sdata
    ldr   r3, =_edata
    adds  r2, r0, r1
    cmp   r2, r3
    bcc   CopyDataInit

    /* 3. Mettre .bss a zero.
          Le standard C garantit que les globales non initialisees
          valent zero au demarrage. */
    ldr   r2, =_sbss
    b     LoopFillZerobss

FillZerobss:
    movs  r3, #0
    str   r3, [r2], #4

LoopFillZerobss:
    ldr   r3, =_ebss
    cmp   r2, r3
    bcc   FillZerobss

    /* 4. Appeler main() */
    bl    main

    /* main() ne doit jamais retourner en embarque */
LoopForever:
    b     LoopForever

.size Reset_Handler, .-Reset_Handler


/* =========================================================
   HANDLER PAR DEFAUT

   Toutes les entrees de la table doivent pointer quelque part.
   Celles qui ne sont pas implementees atterrissent ici. En debug,
   un programme bloque dans Infinite_Loop signale une interruption
   non geree — souvent un HardFault.
   ========================================================= */
    .section  .text.Default_Handler,"ax",%progbits

Default_Handler:
Infinite_Loop:
    b  Infinite_Loop

.size Default_Handler, .-Default_Handler


/* =========================================================
   VECTOR TABLE

   Tableau d'adresses place au tout debut de la flash. Le
   processeur y pioche selon l'evenement : au reset il lit les
   deux premieres entrees, a une interruption celle qui
   correspond a son numero.

   Les 16 premieres entrees sont les exceptions systeme definies
   par ARM, communes a tous les Cortex-M. Viennent ensuite les
   interruptions peripheriques, propres au STM32L4.
   ========================================================= */
    .section  .isr_vector,"a",%progbits
    .type     g_pfnVectors, %object

g_pfnVectors:
    /* --- Exceptions systeme ARM --- */
    .word _estack                       /* 0x00  stack pointer initial */
    .word Reset_Handler                 /* 0x04  reset                 */
    .word NMI_Handler                   /* 0x08 */
    .word HardFault_Handler             /* 0x0C */
    .word MemManage_Handler             /* 0x10 */
    .word BusFault_Handler              /* 0x14 */
    .word UsageFault_Handler            /* 0x18 */
    .word 0                             /* 0x1C  reserve */
    .word 0                             /* 0x20  reserve */
    .word 0                             /* 0x24  reserve */
    .word 0                             /* 0x28  reserve */
    .word SVC_Handler                   /* 0x2C */
    .word DebugMon_Handler              /* 0x30 */
    .word 0                             /* 0x34  reserve */
    .word PendSV_Handler                /* 0x38 */
    .word SysTick_Handler               /* 0x3C */

    /* --- Interruptions peripheriques STM32L4 --- */
    .word WWDG_IRQHandler                     /* IRQ  0 */
    .word PVD_PVM_IRQHandler                  /* IRQ  1 */
    .word TAMP_STAMP_IRQHandler               /* IRQ  2 */
    .word RTC_WKUP_IRQHandler                 /* IRQ  3 */
    .word FLASH_IRQHandler                    /* IRQ  4 */
    .word RCC_IRQHandler                      /* IRQ  5 */
    .word EXTI0_IRQHandler                    /* IRQ  6 */
    .word EXTI1_IRQHandler                    /* IRQ  7 */
    .word EXTI2_IRQHandler                    /* IRQ  8 */
    .word EXTI3_IRQHandler                    /* IRQ  9 */
    .word EXTI4_IRQHandler                    /* IRQ 10 */
    .word DMA1_Channel1_IRQHandler            /* IRQ 11 */
    .word DMA1_Channel2_IRQHandler            /* IRQ 12 */
    .word DMA1_Channel3_IRQHandler            /* IRQ 13 */
    .word DMA1_Channel4_IRQHandler            /* IRQ 14 */
    .word DMA1_Channel5_IRQHandler            /* IRQ 15 */
    .word DMA1_Channel6_IRQHandler            /* IRQ 16 */
    .word DMA1_Channel7_IRQHandler            /* IRQ 17 */
    .word ADC1_2_IRQHandler                   /* IRQ 18 */
    .word CAN1_TX_IRQHandler                  /* IRQ 19 */
    .word CAN1_RX0_IRQHandler                 /* IRQ 20 */
    .word CAN1_RX1_IRQHandler                 /* IRQ 21 */
    .word CAN1_SCE_IRQHandler                 /* IRQ 22 */
    .word EXTI9_5_IRQHandler                  /* IRQ 23 */
    .word TIM1_BRK_TIM15_IRQHandler           /* IRQ 24 */
    .word TIM1_UP_TIM16_IRQHandler            /* IRQ 25 */
    .word TIM1_TRG_COM_TIM17_IRQHandler       /* IRQ 26 */
    .word TIM1_CC_IRQHandler                  /* IRQ 27 */
    .word TIM2_IRQHandler                     /* IRQ 28 */
    .word TIM3_IRQHandler                     /* IRQ 29 */
    .word TIM4_IRQHandler                     /* IRQ 30 */
    .word I2C1_EV_IRQHandler                  /* IRQ 31 */
    .word I2C1_ER_IRQHandler                  /* IRQ 32 */
    .word I2C2_EV_IRQHandler                  /* IRQ 33 */
    .word I2C2_ER_IRQHandler                  /* IRQ 34 */
    .word SPI1_IRQHandler                     /* IRQ 35 */
    .word SPI2_IRQHandler                     /* IRQ 36 */
    .word USART1_IRQHandler                   /* IRQ 37 */
    .word USART2_IRQHandler                   /* IRQ 38  <-- notre handler */


/* =========================================================
   ALIAS FAIBLES

   Chaque handler pointe vers Default_Handler par defaut. Une
   definition FORTE du meme nom ailleurs dans le projet — par
   exemple USART2_IRQHandler dans uart.c — remplace
   automatiquement l'alias : le linker retient toujours la
   definition forte.

   C'est ce qui permet d'implementer un handler sans jamais
   modifier cette table.
   ========================================================= */

    .weak NMI_Handler
    .thumb_set NMI_Handler,Default_Handler

    .weak HardFault_Handler
    .thumb_set HardFault_Handler,Default_Handler

    .weak MemManage_Handler
    .thumb_set MemManage_Handler,Default_Handler

    .weak BusFault_Handler
    .thumb_set BusFault_Handler,Default_Handler

    .weak UsageFault_Handler
    .thumb_set UsageFault_Handler,Default_Handler

    .weak SVC_Handler
    .thumb_set SVC_Handler,Default_Handler

    .weak DebugMon_Handler
    .thumb_set DebugMon_Handler,Default_Handler

    .weak PendSV_Handler
    .thumb_set PendSV_Handler,Default_Handler

    .weak SysTick_Handler
    .thumb_set SysTick_Handler,Default_Handler

    .weak WWDG_IRQHandler
    .thumb_set WWDG_IRQHandler,Default_Handler

    .weak PVD_PVM_IRQHandler
    .thumb_set PVD_PVM_IRQHandler,Default_Handler

    .weak TAMP_STAMP_IRQHandler
    .thumb_set TAMP_STAMP_IRQHandler,Default_Handler

    .weak RTC_WKUP_IRQHandler
    .thumb_set RTC_WKUP_IRQHandler,Default_Handler

    .weak FLASH_IRQHandler
    .thumb_set FLASH_IRQHandler,Default_Handler

    .weak RCC_IRQHandler
    .thumb_set RCC_IRQHandler,Default_Handler

    .weak EXTI0_IRQHandler
    .thumb_set EXTI0_IRQHandler,Default_Handler

    .weak EXTI1_IRQHandler
    .thumb_set EXTI1_IRQHandler,Default_Handler

    .weak EXTI2_IRQHandler
    .thumb_set EXTI2_IRQHandler,Default_Handler

    .weak EXTI3_IRQHandler
    .thumb_set EXTI3_IRQHandler,Default_Handler

    .weak EXTI4_IRQHandler
    .thumb_set EXTI4_IRQHandler,Default_Handler

    .weak DMA1_Channel1_IRQHandler
    .thumb_set DMA1_Channel1_IRQHandler,Default_Handler

    .weak DMA1_Channel2_IRQHandler
    .thumb_set DMA1_Channel2_IRQHandler,Default_Handler

    .weak DMA1_Channel3_IRQHandler
    .thumb_set DMA1_Channel3_IRQHandler,Default_Handler

    .weak DMA1_Channel4_IRQHandler
    .thumb_set DMA1_Channel4_IRQHandler,Default_Handler

    .weak DMA1_Channel5_IRQHandler
    .thumb_set DMA1_Channel5_IRQHandler,Default_Handler

    .weak DMA1_Channel6_IRQHandler
    .thumb_set DMA1_Channel6_IRQHandler,Default_Handler

    .weak DMA1_Channel7_IRQHandler
    .thumb_set DMA1_Channel7_IRQHandler,Default_Handler

    .weak ADC1_2_IRQHandler
    .thumb_set ADC1_2_IRQHandler,Default_Handler

    .weak CAN1_TX_IRQHandler
    .thumb_set CAN1_TX_IRQHandler,Default_Handler

    .weak CAN1_RX0_IRQHandler
    .thumb_set CAN1_RX0_IRQHandler,Default_Handler

    .weak CAN1_RX1_IRQHandler
    .thumb_set CAN1_RX1_IRQHandler,Default_Handler

    .weak CAN1_SCE_IRQHandler
    .thumb_set CAN1_SCE_IRQHandler,Default_Handler

    .weak EXTI9_5_IRQHandler
    .thumb_set EXTI9_5_IRQHandler,Default_Handler

    .weak TIM1_BRK_TIM15_IRQHandler
    .thumb_set TIM1_BRK_TIM15_IRQHandler,Default_Handler

    .weak TIM1_UP_TIM16_IRQHandler
    .thumb_set TIM1_UP_TIM16_IRQHandler,Default_Handler

    .weak TIM1_TRG_COM_TIM17_IRQHandler
    .thumb_set TIM1_TRG_COM_TIM17_IRQHandler,Default_Handler

    .weak TIM1_CC_IRQHandler
    .thumb_set TIM1_CC_IRQHandler,Default_Handler

    .weak TIM2_IRQHandler
    .thumb_set TIM2_IRQHandler,Default_Handler

    .weak TIM3_IRQHandler
    .thumb_set TIM3_IRQHandler,Default_Handler

    .weak TIM4_IRQHandler
    .thumb_set TIM4_IRQHandler,Default_Handler

    .weak I2C1_EV_IRQHandler
    .thumb_set I2C1_EV_IRQHandler,Default_Handler

    .weak I2C1_ER_IRQHandler
    .thumb_set I2C1_ER_IRQHandler,Default_Handler

    .weak I2C2_EV_IRQHandler
    .thumb_set I2C2_EV_IRQHandler,Default_Handler

    .weak I2C2_ER_IRQHandler
    .thumb_set I2C2_ER_IRQHandler,Default_Handler

    .weak SPI1_IRQHandler
    .thumb_set SPI1_IRQHandler,Default_Handler

    .weak SPI2_IRQHandler
    .thumb_set SPI2_IRQHandler,Default_Handler

    .weak USART1_IRQHandler
    .thumb_set USART1_IRQHandler,Default_Handler

    .weak USART2_IRQHandler
    .thumb_set USART2_IRQHandler,Default_Handler
