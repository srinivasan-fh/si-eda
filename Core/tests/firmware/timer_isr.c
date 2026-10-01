// Timer1 CTC compare interrupt at 1 kHz (OCR1A = 249, /64); every 100 ticks toggle PB5 and print the tick count.
#define F_CPU 16000000UL
#include <avr/interrupt.h>
#include "uart.h"
volatile uint16_t ticks;
ISR(TIMER1_COMPA_vect) { ++ticks; }
int main(void) {
    uart_init(8, 0);  // 115200 baud (16 MHz / 16 / 9 = 111111, -3.5 %)
    DDRB |= (1 << DDB5);
    TCCR1A = 0;
    TCCR1B = (1 << WGM12) | (1 << CS11) | (1 << CS10);
    OCR1A = 249;
    TIMSK1 = (1 << OCIE1A);
    sei();
    uint16_t last = 0;
    for (;;) {
        uint16_t t;
        cli(); t = ticks; sei();
        if ((uint16_t)(t - last) >= 100) {
            last += 100;
            PORTB ^= (1 << PORTB5);
            uart_putu32(last);
            uart_putc('\n');
        }
    }
}
