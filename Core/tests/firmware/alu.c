// CPU self-check: integer (8/16/32-bit, signed/unsigned, multiply, divide, shifts), float, strings, function
// pointers and recursion. Prints one result per line at 1 Mbaud (U2X, UBRR 1) and ends with "END".
#define F_CPU 16000000UL
#include <string.h>
#include <stdlib.h>
#include "uart.h"
volatile uint32_t seed = 12345;
static uint32_t lcg(void) { seed = seed * 1103515245u + 12345u; return seed; }
static int32_t fib(int8_t n) { return n < 2 ? n : fib((int8_t)(n - 1)) + fib((int8_t)(n - 2)); }
static int16_t sq(int16_t x) { return (int16_t)(x * x); }
static int16_t neg(int16_t x) { return (int16_t)-x; }
static int16_t (*const ops[2])(int16_t) = {sq, neg};
static int cmp(const void* a, const void* b) { return (int)(*(const int16_t*)a - *(const int16_t*)b); }
#define OUT(v) do { uart_puti32((int32_t)(v)); uart_putc('\n'); } while (0)
#define OUTU(v) do { uart_putu32((uint32_t)(v)); uart_putc('\n'); } while (0)
int main(void) {
    uart_init(1, 1);
    volatile int8_t a8 = -100, b8 = 77;
    volatile uint8_t u8a = 200, u8b = 99;
    volatile int16_t a16 = -12345, b16 = 321;
    volatile uint16_t u16a = 60000, u16b = 7;
    volatile int32_t a32 = -987654321, b32 = 12345;
    volatile uint32_t u32a = 4000000000u, u32b = 65537u;
    OUT((int8_t)(a8 + b8)); OUT((int8_t)(a8 - b8)); OUT((int16_t)(a8 * b8)); OUT(a8 / 7); OUT(a8 % 7);
    OUTU((uint8_t)(u8a + u8b)); OUTU((uint8_t)(u8a * u8b)); OUTU(u8a / u8b); OUTU(u8a % u8b);
    OUTU((uint8_t)(u8a << 3)); OUTU(u8a >> 3); OUT((int8_t)a8 >> 2);
    OUT((int16_t)(a16 + b16)); OUT((int16_t)(a16 * b16)); OUT(a16 / b16); OUT(a16 % b16); OUT(a16 >> 3);
    OUTU((uint16_t)(u16a * u16b)); OUTU(u16a / u16b); OUTU(u16a % u16b); OUTU((uint16_t)(u16a << 5));
    OUT(a32 + b32); OUT(a32 * b32); OUT(a32 / b32); OUT(a32 % b32); OUT(a32 >> 7);
    OUTU(u32a + u32b); OUTU(u32a * u32b); OUTU(u32a / u32b); OUTU(u32a % u32b); OUTU(u32a >> 13);
    OUT((int32_t)a16 * (int32_t)b16); OUTU((uint32_t)u16a * u16a);
    for (uint8_t i = 0; i < 5; ++i) OUTU(lcg());
    OUT(fib(15));
    OUT(ops[0](-123)); OUT(ops[1](456));
    volatile float f1 = 3.14159f, f2 = -2.5f;
    OUT((int32_t)(f1 * 1000.0f)); OUT((int32_t)((f1 + f2) * 1e5f)); OUT((int32_t)(f1 / f2 * 1e6f));
    OUT((int32_t)(f1 * f1 * f1 * 1000.0f)); OUT((int32_t)(f2 < f1)); OUT((int32_t)((float)a32 / 1000.0f));
    char buf[32];
    strcpy(buf, "SiEDA");
    strcat(buf, "-AVR");
    OUT((int32_t)strlen(buf)); OUT(strcmp(buf, "SiEDA-AVR")); uart_puts(buf); uart_putc('\n');
    int16_t arr[8] = {5, -3, 9, 0, 22, -17, 4, 4};
    qsort(arr, 8, sizeof arr[0], cmp);
    for (uint8_t i = 0; i < 8; ++i) OUT(arr[i]);
    uint8_t sreg = 0;
    __asm__ volatile("sec\n\tclz\n\tin %0, __SREG__" : "=r"(sreg));
    OUTU(sreg & 0x03);
    uart_puts("END\n");
    uart_flush();
    for (;;) {}
}
