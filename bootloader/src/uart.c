#include <stdint.h>
#include "uart.h"

/* ----------------------------------------------------------------
 * Adresses — RM0351 section 2.2.2
 * ---------------------------------------------------------------- */
#define RCC_BASE            0x40021000UL
#define GPIOA_BASE          0x48000000UL
#define USART2_BASE         0x40004400UL
#define NVIC_ISER1          (*(volatile uint32_t *)0xE000E104UL)

#define RCC_AHB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APB1ENR1        (*(volatile uint32_t *)(RCC_BASE + 0x58))

#define GPIOA_MODER         (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_OSPEEDR       (*(volatile uint32_t *)(GPIOA_BASE + 0x08))
#define GPIOA_PUPDR         (*(volatile uint32_t *)(GPIOA_BASE + 0x0C))
#define GPIOA_AFRL          (*(volatile uint32_t *)(GPIOA_BASE + 0x20))

#define USART2_CR1          (*(volatile uint32_t *)(USART2_BASE + 0x00))
#define USART2_CR3          (*(volatile uint32_t *)(USART2_BASE + 0x08))
#define USART2_BRR          (*(volatile uint32_t *)(USART2_BASE + 0x0C))
#define USART2_ISR_REG      (*(volatile uint32_t *)(USART2_BASE + 0x1C))
#define USART2_ICR          (*(volatile uint32_t *)(USART2_BASE + 0x20))
#define USART2_RDR          (*(volatile uint32_t *)(USART2_BASE + 0x24))
#define USART2_TDR          (*(volatile uint32_t *)(USART2_BASE + 0x28))

/* USART_CR1 — RM0351 section 40.8.1 */
#define CR1_UE              (1U << 0)
#define CR1_RE              (1U << 2)
#define CR1_TE              (1U << 3)
#define CR1_RXNEIE          (1U << 5)

/* USART_ISR — RM0351 section 40.8.8 */
#define ISR_PE              (1U << 0)
#define ISR_FE              (1U << 1)
#define ISR_NE              (1U << 2)
#define ISR_ORE             (1U << 3)
#define ISR_RXNE            (1U << 5)
#define ISR_TC              (1U << 6)
#define ISR_TXE             (1U << 7)

/* USART_ICR — effacement des drapeaux, ecriture de 1 */
#define ICR_PECF            (1U << 0)
#define ICR_FECF            (1U << 1)
#define ICR_NECF            (1U << 2)
#define ICR_ORECF           (1U << 3)

/* Horloge systeme par defaut : MSI a 4 MHz au reset */
#define SYSTEM_CLOCK_HZ     4000000UL

/* USART2 occupe la position 38 dans la table des vecteurs.
   NVIC_ISER1 couvre les interruptions 32 a 63, d'ou le decalage. */
#define USART2_IRQ_NUMBER   38U


/* ----------------------------------------------------------------
 * Tampon de reception
 *
 * head n'est ecrit que par l'ISR, tail que par le contexte
 * principal. Aucune variable n'est ecrite par les deux, ce qui rend
 * toute section critique inutile : sur Cortex-M, l'ecriture d'un
 * uint32_t aligne est atomique.
 * ---------------------------------------------------------------- */
static volatile uint8_t  rx_buffer[UART_RX_BUFFER_SIZE];
static volatile uint32_t rx_head = 0;   /* ecrit par l'ISR          */
static volatile uint32_t rx_tail = 0;   /* ecrit par le principal   */

static volatile uint32_t cnt_overrun_sw = 0;
static volatile uint32_t cnt_overrun_hw = 0;
static volatile uint32_t cnt_framing    = 0;
static volatile uint32_t cnt_noise      = 0;

#define RX_MASK             (UART_RX_BUFFER_SIZE - 1U)


/* ----------------------------------------------------------------
 * Initialisation
 * ---------------------------------------------------------------- */

void uart_init(uint32_t baudrate)
{
    RCC_AHB2ENR  |= (1U << 0);      /* horloge GPIOA */
    RCC_APB1ENR1 |= (1U << 17);     /* horloge USART2 */

    /* PA2 et PA3 en fonction alternative (MODER = 10) */
    GPIOA_MODER &= ~((3U << (2 * 2)) | (3U << (3 * 2)));
    GPIOA_MODER |=  ((2U << (2 * 2)) | (2U << (3 * 2)));

    /* AF7 = USART2 (datasheet STM32L476, table de mapping des AF) */
    GPIOA_AFRL &= ~((0xFU << (2 * 4)) | (0xFU << (3 * 4)));
    GPIOA_AFRL |=  ((7U   << (2 * 4)) | (7U   << (3 * 4)));

    /* Pull-up sur RX : maintient la ligne au repos si l'emetteur est
       debranche, ce qui evite un flot d'octets parasites. */
    GPIOA_PUPDR &= ~(3U << (3 * 2));
    GPIOA_PUPDR |=  (1U << (3 * 2));

    /* Vitesse elevee : a 115200 bauds ce n'est pas critique, mais
       cela reduit le temps de montee et donc la sensibilite au bruit. */
    GPIOA_OSPEEDR |= (3U << (2 * 2)) | (3U << (3 * 2));

    /* L'USART doit etre desactive pour reconfigurer BRR. */
    USART2_CR1 = 0;
    USART2_CR3 = 0;

    /* Mode oversampling par 16 (defaut) : BRR = f_ck / baudrate.
       A 4 MHz et 115200 bauds, cela donne 34, soit 117647 bauds
       reels — 2,1 % d'ecart, dans la tolerance de l'UART. */
    USART2_BRR = SYSTEM_CLOCK_HZ / baudrate;

    rx_head = 0;
    rx_tail = 0;

    /* Effacer les drapeaux d'erreur eventuellement herites. */
    USART2_ICR = ICR_PECF | ICR_FECF | ICR_NECF | ICR_ORECF;

    /* Activer l'interruption USART2 dans le NVIC avant d'armer
       RXNEIE, pour ne pas manquer un octet arrivant aussitot. */
    NVIC_ISER1 = (1U << (USART2_IRQ_NUMBER - 32U));

    USART2_CR1 = CR1_TE | CR1_RE | CR1_RXNEIE | CR1_UE;
}


/* ----------------------------------------------------------------
 * Gestionnaire d'interruption
 *
 * Le nom correspond a l'entree de la table des vecteurs dans
 * startup.s. Le symbole faible defini la-bas est remplace
 * automatiquement par cette definition forte.
 * ---------------------------------------------------------------- */

void USART2_IRQHandler(void)
{
    uint32_t status = USART2_ISR_REG;

    /* Les erreurs sont traitees en premier.
     *
     * ORE est le cas le plus grave : tant qu'il n'est pas efface
     * explicitement, l'USART CESSE de recevoir. Un pilote qui
     * l'ignore devient sourd apres le premier debordement, sans
     * aucun symptome visible cote firmware — le PC voit simplement
     * ses trames rester sans reponse. */
    if (status & (ISR_ORE | ISR_FE | ISR_NE | ISR_PE)) {

        if (status & ISR_ORE) {
            cnt_overrun_hw++;
            USART2_ICR = ICR_ORECF;
        }
        if (status & ISR_FE) {
            cnt_framing++;          /* bit de stop absent : debit errone */
            USART2_ICR = ICR_FECF;
        }
        if (status & ISR_NE) {
            cnt_noise++;            /* echantillonnage incoherent */
            USART2_ICR = ICR_NECF;
        }
        if (status & ISR_PE) {
            USART2_ICR = ICR_PECF;
        }
    }

    if (status & ISR_RXNE) {
        /* La lecture de RDR efface RXNE. Elle doit avoir lieu meme
           si le tampon est plein, sans quoi l'interruption se
           redeclencherait indefiniment. */
        uint8_t byte = (uint8_t)(USART2_RDR & 0xFFU);

        uint32_t next = (rx_head + 1U) & RX_MASK;

        if (next == rx_tail) {
            /* Tampon plein : on jette le nouvel octet plutot que
               d'ecraser le plus ancien. La trame en cours sera
               invalidee par son CRC et retransmise ; ecraser
               corromprait une trame deja complete. */
            cnt_overrun_sw++;
        } else {
            rx_buffer[rx_head] = byte;
            rx_head = next;
        }
    }
}


/* ----------------------------------------------------------------
 * Emission
 * ---------------------------------------------------------------- */

void uart_putc(char c)
{
    while (!(USART2_ISR_REG & ISR_TXE)) {
        /* attente que le registre d'emission se libere */
    }
    USART2_TDR = (uint32_t)(uint8_t)c;
}


void uart_puts(const char *s)
{
    if (s == 0) {
        return;
    }
    while (*s) {
        uart_putc(*s++);
    }
}


void uart_write(const uint8_t *data, uint32_t len)
{
    if (data == 0) {
        return;
    }
    while (len--) {
        uart_putc((char)*data++);
    }
}


void uart_flush(void)
{
    /* TXE indique que le registre est libre, pas que l'octet est
       parti. TC signale la fin reelle de la transmission. */
    while (!(USART2_ISR_REG & ISR_TC)) {
    }
}


/* ----------------------------------------------------------------
 * Formatage
 * ---------------------------------------------------------------- */

static const char HEX_DIGITS[] = "0123456789ABCDEF";

void uart_hex32(uint32_t v)
{
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(HEX_DIGITS[(v >> i) & 0xFU]);
    }
}


void uart_hex8(uint8_t v)
{
    uart_putc(HEX_DIGITS[(v >> 4) & 0xFU]);
    uart_putc(HEX_DIGITS[v & 0xFU]);
}


void uart_dec(uint32_t v)
{
    char tmp[10];
    int n = 0;

    if (v == 0U) {
        uart_putc('0');
        return;
    }
    while (v > 0U) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (n-- > 0) {
        uart_putc(tmp[n]);
    }
}


/* ----------------------------------------------------------------
 * Reception
 * ---------------------------------------------------------------- */

uint32_t uart_available(void)
{
    /* Lecture unique de chaque indice : head peut changer sous nos
       pieds, mais la valeur lue reste coherente et sous-estime au
       pire le nombre d'octets disponibles. */
    uint32_t head = rx_head;
    uint32_t tail = rx_tail;

    return (head - tail) & RX_MASK;
}


int uart_getc(uint8_t *out)
{
    if (out == 0) {
        return 0;
    }

    uint32_t tail = rx_tail;

    if (rx_head == tail) {
        return 0;                   /* tampon vide */
    }

    *out = rx_buffer[tail];
    rx_tail = (tail + 1U) & RX_MASK;

    return 1;
}


uint32_t uart_read(uint8_t *dest, uint32_t max)
{
    if (dest == 0) {
        return 0;
    }

    uint32_t n = 0;
    while (n < max && uart_getc(&dest[n])) {
        n++;
    }
    return n;
}


void uart_rx_flush(void)
{
    rx_tail = rx_head;
}


/* ----------------------------------------------------------------
 * Diagnostic
 * ---------------------------------------------------------------- */

uint32_t uart_overrun_count(void)       { return cnt_overrun_sw; }
uint32_t uart_hw_overrun_count(void)    { return cnt_overrun_hw; }
uint32_t uart_framing_error_count(void) { return cnt_framing;    }
uint32_t uart_noise_error_count(void)   { return cnt_noise;      }

void uart_reset_counters(void)
{
    cnt_overrun_sw = 0;
    cnt_overrun_hw = 0;
    cnt_framing    = 0;
    cnt_noise      = 0;
}
