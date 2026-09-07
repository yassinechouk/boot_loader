.syntax unified
.cpu cortex-m4
.fpu softvfp
.thumb

/* =========================================================
   RESET HANDLER — premier code exécuté
   ========================================================= */
    .section  .text.Reset_Handler
    .weak     Reset_Handler
    .type     Reset_Handler, %function

Reset_Handler:
    /* 1. Charger le stack pointer */
    ldr   sp, =_estack

    /* 2. Copier .data de la FLASH vers la RAM */
    movs  r1, #0
    b     LoopCopyDataInit

CopyDataInit:
    ldr   r3, =_sidata      /* adresse source en flash */
    ldr   r3, [r3, r1]      /* lire un mot de 32 bits */
    str   r3, [r0, r1]      /* l'écrire en RAM */
    adds  r1, r1, #4        /* avancer de 4 octets */

LoopCopyDataInit:
    ldr   r0, =_sdata       /* début de la destination */
    ldr   r3, =_edata       /* fin de la destination */
    adds  r2, r0, r1        /* position courante */
    cmp   r2, r3            /* a-t-on atteint la fin ? */
    bcc   CopyDataInit      /* non → continuer */

    /* 3. Mettre .bss à zéro */
    ldr   r2, =_sbss
    b     LoopFillZerobss

FillZerobss:
    movs  r3, #0
    str   r3, [r2], #4      /* écrire 0, puis avancer */

LoopFillZerobss:
    ldr   r3, =_ebss
    cmp   r2, r3
    bcc   FillZerobss

    /* 4. Appeler main() */
    bl    main

    /* main() ne devrait jamais retourner en embarqué */
LoopForever:
    b     LoopForever

.size Reset_Handler, .-Reset_Handler


/* =========================================================
   HANDLER PAR DÉFAUT — boucle infinie
   ========================================================= */
    .section  .text.Default_Handler,"ax",%progbits

Default_Handler:
Infinite_Loop:
    b  Infinite_Loop

.size Default_Handler, .-Default_Handler


/* =========================================================
   VECTOR TABLE
   ========================================================= */
    .section  .isr_vector,"a",%progbits
    .type     g_pfnVectors, %object

g_pfnVectors:
    .word _estack                   /* 0x00  stack pointer initial */
    .word Reset_Handler             /* 0x04  reset */
    .word NMI_Handler               /* 0x08 */
    .word HardFault_Handler         /* 0x0C */
    .word MemManage_Handler         /* 0x10 */
    .word BusFault_Handler          /* 0x14 */
    .word UsageFault_Handler        /* 0x18 */
    .word 0                         /* 0x1C  réservé */
    .word 0                         /* 0x20  réservé */
    .word 0                         /* 0x24  réservé */
    .word 0                         /* 0x28  réservé */
    .word SVC_Handler               /* 0x2C */
    .word DebugMon_Handler          /* 0x30 */
    .word 0                         /* 0x34  réservé */
    .word PendSV_Handler            /* 0x38 */
    .word SysTick_Handler           /* 0x3C */


/* =========================================================
   ALIAS — tout pointe vers Default_Handler par défaut
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