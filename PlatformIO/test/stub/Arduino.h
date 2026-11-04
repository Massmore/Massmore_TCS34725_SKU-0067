// Minimal Arduino API stub for host-side tests (no hardware needed)
#ifndef ARDUINO_STUB_H
#define ARDUINO_STUB_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define HEX 16
#define DEC 10
#ifndef F
#define F(x) (x)
#endif
typedef uint8_t byte;
unsigned long millis();
unsigned long micros();
void delay(unsigned long);
void delayMicroseconds(unsigned int);
void pinMode(int, int);
void digitalWrite(int, int);
int  digitalRead(int);
class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  size_t print(const char *s) { size_t n = 0; while (*s) n += write((uint8_t)*s++); return n; }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(unsigned long v, int base = DEC) { char b[24]; snprintf(b, sizeof b, base == HEX ? "%lX" : "%lu", v); return print(b); }
  size_t print(long v, int base = DEC) { return base == DEC ? printSigned(v) : print((unsigned long)v, base); }
  size_t print(int v, int base = DEC) { return print((long)v, base); }
  size_t print(unsigned v, int base = DEC) { return print((unsigned long)v, base); }
  size_t print(uint8_t v, int base = DEC) { return print((unsigned long)v, base); }
  size_t print(double v, int d = 2) { char b[32]; snprintf(b, sizeof b, "%.*f", d, v); return print(b); }
  size_t println() { return print("\n"); }
  template <typename T> size_t println(T v) { size_t n = print(v); return n + println(); }
  template <typename T> size_t println(T v, int x) { size_t n = print(v, x); return n + println(); }
private:
  size_t printSigned(long v) { char b[24]; snprintf(b, sizeof b, "%ld", v); return print(b); }
};
#endif
