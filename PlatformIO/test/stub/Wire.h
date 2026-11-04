// TwoWire stub — implementation (simulated TCS34725) lives in the test file
#ifndef WIRE_STUB_H
#define WIRE_STUB_H
#include "Arduino.h"
class TwoWire {
public:
  void beginTransmission(uint8_t addr);
  size_t write(uint8_t b);
  uint8_t endTransmission(bool stop = true);
  uint8_t requestFrom(uint8_t addr, uint8_t len);
  int read();
  int available();
private:
  uint8_t _txAddr = 0, _tx[32] = {0}, _txLen = 0, _rx[32] = {0}, _rxLen = 0, _rxPos = 0;
};
extern TwoWire Wire;
#endif
