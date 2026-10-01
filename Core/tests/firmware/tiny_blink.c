// ATtiny85 @ 8 MHz: PB0 blinks every 100 ms; PB1 = OC0B fast PWM at 50 % duty.
#define F_CPU 8000000UL
#include <avr/io.h>
#include <util/delay.h>
int main(void) {
    DDRB |= (1 << DDB0) | (1 << DDB1);
    TCCR0A = (1 << COM0B1) | (1 << WGM01) | (1 << WGM00);
    TCCR0B = (1 << CS00);
    OCR0B = 128;
    for (;;) {
        PORTB ^= (1 << PORTB0);
        _delay_ms(100);
    }
}
