// Minimal Arduino API surface, enough to type-check the sketches on a PC.
// It implements nothing: the only question it answers is whether the sketch
// is valid C++ that calls the real API correctly.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

typedef uint8_t byte;
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define SERIAL_8N1 0x800001c

class String {
 public:
  String() {}
  String(const char *s) : v(s ? s : "") {}
  String(const std::string &s) : v(s) {}
  String(char c) : v(1, c) {}
  String(int n) : v(std::to_string(n)) {}
  String(long n) : v(std::to_string(n)) {}
  String(unsigned n) : v(std::to_string(n)) {}
  String(unsigned long n) : v(std::to_string(n)) {}
  String(double n) : v(std::to_string(n)) {}

  const char *c_str() const { return v.c_str(); }
  unsigned length() const { return (unsigned)v.size(); }
  int indexOf(char c) const { return (int)v.find(c); }
  int indexOf(char c, int from) const { return (int)v.find(c, (size_t)from); }
  int indexOf(const String &s) const { return (int)v.find(s.v); }
  String substring(int a) const { return String(v.substr((size_t)a)); }
  String substring(int a, int b) const {
    return String(v.substr((size_t)a, (size_t)(b - a)));
  }
  long toInt() const { return std::atol(v.c_str()); }
  bool startsWith(const String &s) const { return v.rfind(s.v, 0) == 0; }
  bool equals(const char *s) const { return v == (s ? s : ""); }
  bool equals(const String &s) const { return v == s.v; }
  void toLowerCase() {}   // arduino-esp32 returns void, deliberately
  void toUpperCase() {}

  String &operator+=(const String &s) { v += s.v; return *this; }
  String &operator+=(const char *s) { v += s; return *this; }
  String &operator+=(char c) { v += c; return *this; }
  String &operator+=(int n) { v += std::to_string(n); return *this; }
  String &operator+=(unsigned n) { v += std::to_string(n); return *this; }
  String &operator+=(unsigned long n) { v += std::to_string(n); return *this; }

  friend String operator+(const String &a, const String &b) { return String(a.v + b.v); }
  friend String operator+(const String &a, const char *b) { return String(a.v + b); }
  friend String operator+(const char *a, const String &b) { return String(a + b.v); }
  friend bool operator==(const String &a, const String &b) { return a.v == b.v; }
  friend bool operator==(const String &a, const char *b) { return a.v == b; }

  std::string v;
};

class Stream {
 public:
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  void print(const String &) {}
  void print(const char *) {}
  void print(int) {}
  void println(const String &) {}
  void println(const char *) {}
  void println() {}
  void printf(const char *, ...) {}
  void write(uint8_t) {}
  void write(const uint8_t *, size_t) {}
  void begin(unsigned long) {}
  operator bool() const { return true; }
};

class HardwareSerial : public Stream {
 public:
  HardwareSerial(int) {}
  void begin(unsigned long, uint32_t = SERIAL_8N1, int = -1, int = -1) {}
};

extern HardwareSerial Serial;

unsigned long millis();
unsigned long micros();
void delay(unsigned long);
long random(long, long);
void pinMode(int, int);
void digitalWrite(int, int);
int digitalRead(int);
int analogRead(int);
void analogReadResolution(int);
void analogSetAttenuation(int);
#define ADC_11db 3

template <typename T> const T &constrain(const T &v, const T &lo, const T &hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
