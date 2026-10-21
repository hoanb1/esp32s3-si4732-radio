#ifndef WEBLOG_H
#define WEBLOG_H

#include <Arduino.h>

//
// Everything the sketch prints to Serial also goes into a small line ring
// that the web UI streams over the /ws WebSocket (live log card).
// Common.h redefines Serial as LogSerial, so existing Serial.printf() calls are captured.
//
class LogSerialClass : public Stream
{
  public:
    void begin(unsigned long baud) { Serial0.begin(baud); }
    void updateBaudRate(unsigned long baud) { Serial0.updateBaudRate(baud); }
    int available() override { return Serial0.available(); }
    int read() override { return Serial0.read(); }
    int peek() override { return Serial0.peek(); }
    void flush() override { Serial0.flush(); }
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *buf, size_t size) override;
    using Print::write;

    // Off while raw binary audio is streamed over the UART
    void setCapture(bool on) { capture = on; }

  private:
    volatile bool capture = true;
};

extern LogSerialClass LogSerial;

#define WEBLOG_LINES    20
#define WEBLOG_LINE_LEN 100

// The line ring (WEBLOG_LINES x WEBLOG_LINE_LEN bytes of heap) exists only while the log is on;
// off frees it, and Serial output then goes to the UART only. Call from one task at a time.
void webLogSetEnabled(bool on);
bool webLogEnabled();

// Absolute number of the next line to be written (lines head-WEBLOG_LINES .. head-1 are kept)
uint32_t webLogHead();
// Copy line `seq` into dst; false if it is no longer (or not yet) in the ring
bool webLogGet(uint32_t seq, char *dst, size_t maxLen);

#endif // WEBLOG_H
