// Button on PD2 (INT0, internal pull-up, falling edge) counts presses; the LED on PB5 mirrors the button (on while
// pressed) and each press prints "press N".
#define F_CPU 16000000UL
#include <avr/interrupt.h>
#include "uart.h"
volatile uint8_t presses, pending;
ISR(INT0_vect) { ++presses; pending = 1; }
int main(void) {
    uart_init(8, 0);
    DDRB |= (1 << DDB5);
    PORTD |= (1 << PORTD2);  // pull-up
    EICRA = (1 << ISC01);     // falling edge
    EIMSK = (1 << INT0);
    sei();
    for (;;) {
        if (PIND & (1 << PIND2)) PORTB &= ~(1 << PORTB5);
        else PORTB |= (1 << PORTB5);
        if (pending) {
            pending = 0;
            uart_puts("press ");
            uart_putu32(presses);
            uart_putc('\n');
        }
    }
}
