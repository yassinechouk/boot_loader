#include <stdint.h>
#include "uart.h"
#include "crc.h"
#include "flash.h"
#include "metadata.h"
#include "metadata_mgr.h"


static int nb_ok = 0, nb_ko = 0;

static void chk(const char *nom, int cond)
{
    uart_puts(cond ? "  ok    " : "  ECHEC ");
    uart_puts(nom);
    uart_puts("\r\n");
    if (cond) nb_ok++; else nb_ko++;
}


int main(void)
{
    uart_init(115200);
    crc32_init();

    uart_puts("\r\n=== TEST UART ===\r\n");

    /* --- Emission et formatage --- */
    uart_puts("\r\n-- formatage --\r\n");
    uart_puts("  hex32 : ");  uart_hex32(0xDEADBEEF);  uart_puts("  attendu 0xDEADBEEF\r\n");
    uart_puts("  hex8  : ");  uart_hex8(0x5A);         uart_puts("        attendu 5A\r\n");
    uart_puts("  dec   : ");  uart_dec(1234567);       uart_puts("     attendu 1234567\r\n");
    uart_puts("  dec 0 : ");  uart_dec(0);             uart_puts("           attendu 0\r\n");

    /* --- Etat initial de la reception --- */
    uart_puts("\r\n-- etat initial --\r\n");
    uart_rx_flush();
    chk("tampon vide", uart_available() == 0);

    uint8_t c;
    chk("getc echoue si vide", uart_getc(&c) == 0);

    uart_reset_counters();
    chk("compteurs a zero", uart_overrun_count() == 0
                         && uart_hw_overrun_count() == 0
                         && uart_framing_error_count() == 0);

    /* --- Diagnostic --- */
    uart_puts("\r\n-- compteurs d'erreur --\r\n");
    uart_puts("  overrun logiciel  : ");  uart_dec(uart_overrun_count());       uart_puts("\r\n");
    uart_puts("  overrun materiel  : ");  uart_dec(uart_hw_overrun_count());    uart_puts("\r\n");
    uart_puts("  erreurs de trame  : ");  uart_dec(uart_framing_error_count()); uart_puts("\r\n");
    uart_puts("  erreurs de bruit  : ");  uart_dec(uart_noise_error_count());   uart_puts("\r\n");

    uart_puts("\r\n=== ");
    uart_dec((uint32_t)nb_ok);
    uart_puts(" reussis, ");
    uart_dec((uint32_t)nb_ko);
    uart_puts(" echecs ===\r\n");

    /* --- Mode interactif --- */
    uart_puts("\r\n=== MODE ECHO ===\r\n");
    uart_puts("Tape des caracteres, ils reviennent en majuscules.\r\n");
    uart_puts("Commandes :\r\n");
    uart_puts("  '?' : afficher les compteurs\r\n");
    uart_puts("  'r' : remettre les compteurs a zero\r\n");
    uart_puts("  'n' : nombre d'octets en attente\r\n\r\n");

    while (1) {
        if (!uart_getc(&c)) {
            continue;
        }

        if (c == '?') {
            uart_puts("\r\n  ovr_sw=");  uart_dec(uart_overrun_count());
            uart_puts(" ovr_hw=");       uart_dec(uart_hw_overrun_count());
            uart_puts(" frame=");        uart_dec(uart_framing_error_count());
            uart_puts(" noise=");        uart_dec(uart_noise_error_count());
            uart_puts("\r\n");
            continue;
        }

        if (c == 'r') {
            uart_reset_counters();
            uart_puts("\r\n  compteurs remis a zero\r\n");
            continue;
        }

        if (c == 'n') {
            uart_puts("\r\n  en attente : ");
            uart_dec(uart_available());
            uart_puts("\r\n");
            continue;
        }

        /* Echo en majuscules : prouve que l'octet a bien transite
           par le tampon plutot que d'etre un artefact du terminal. */
        if (c >= 'a' && c <= 'z') {
            uart_putc((char)(c - 'a' + 'A'));
        } else {
            uart_putc((char)c);
        }
    }
}