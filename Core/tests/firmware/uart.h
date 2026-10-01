// Minimal polled USART0 output for the SiEDA firmware tests (ATmega328P).
#include <avr/io.h>
#include <stdint.h>
static inline void uart_init(uint16_t ubrr, uint8_t u2x) {
    UBRR0H = (uint8_t)(ubrr >> 8);
    UBRR0L = (uint8_t)ubrr;
    UCSR0A = u2x ? (1 << U2X0) : 0;
    UCSR0B = (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}
static inline void uart_putc(char c) {
    while (!(UCSR0A & (1 << UDRE0))) {}
    UDR0 = (uint8_t)c;
}
static inline void uart_puts(const char* s) { while (*s) uart_putc(*s++); }
static inline void uart_putu32(uint32_t v) {
    char buf[11];
    int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) uart_putc(buf[--i]);
}
static inline void uart_puti32(int32_t v) {
    if (v < 0) { uart_putc('-'); uart_putu32((uint32_t)(-(v + 1)) + 1u); } else uart_putu32((uint32_t)v);
}
static inline void uart_flush(void) {
    while (!(UCSR0A & (1 << TXC0))) {}
}
