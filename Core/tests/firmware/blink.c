// Arduino-style blink without the Arduino core: PB5 (D13) toggles every 250 ms (cycle-exact _delay_ms).
#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>
int main(void) {
    DDRB |= (1 << DDB5);
    for (;;) {
        PORTB ^= (1 << PORTB5);
        _delay_ms(250);
    }
}
