// UART at 9600 baud (UBRR 103 @ 16 MHz): a greeting, then a counter every 20 ms.
#define F_CPU 16000000UL
#include <util/delay.h>
#include "uart.h"
int main(void) {
    uart_init(103, 0);
    uart_puts("Hello, SiEDA!\r\n");
    for (uint16_t n = 0;; ++n) {
        uart_puts("n=");
        uart_putu32(n);
        uart_puts("\r\n");
        _delay_ms(20);
    }
}
