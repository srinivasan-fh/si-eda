// Reads ADC0 (PC0, AVCC reference) and prints the 10-bit value every 10 ms; drives OC0A (PD6) fast PWM with the
// value / 4 and OC1A (PB1) phase-correct PWM at a fixed 25 % duty.
#define F_CPU 16000000UL
#include <util/delay.h>
#include "uart.h"
static uint16_t adc_read(uint8_t ch) {
    ADMUX = (1 << REFS0) | (ch & 0x0F);
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC)) {}
    return ADC;
}
int main(void) {
    uart_init(8, 0);
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
    DDRD |= (1 << DDD6);
    TCCR0A = (1 << COM0A1) | (1 << WGM01) | (1 << WGM00);
    TCCR0B = (1 << CS01);
    DDRB |= (1 << DDB1);
    TCCR1A = (1 << COM1A1) | (1 << WGM10);  // phase-correct 8-bit
    TCCR1B = (1 << CS10);
    OCR1A = 64;
    for (;;) {
        uint16_t v = adc_read(0);
        OCR0A = (uint8_t)(v >> 2);
        uart_putu32(v);
        uart_putc('\n');
        _delay_ms(10);
    }
}
