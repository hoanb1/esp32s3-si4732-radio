#include "WebLog.h"

LogSerialClass LogSerial;

static char (*logLines)[WEBLOG_LINE_LEN] = NULL;   // heap, only while the log is on
static char logCur[WEBLOG_LINE_LEN];
static uint16_t logCurLen = 0;
static uint32_t logHead = 0;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;   // Serial is printed from several tasks

size_t LogSerialClass::write(const uint8_t *buf, size_t size)
{
  size_t n = Serial0.write(buf, size);
  if(!capture) return n;

  portENTER_CRITICAL(&logMux);
  for(size_t i = 0; i < size; i++)
  {
    char c = (char)buf[i];
    if(c == '\n')
    {
      if(logCurLen && logLines)
      {
        char *dst = logLines[logHead % WEBLOG_LINES];
        memcpy(dst, logCur, logCurLen);
        dst[logCurLen] = '\0';
        logHead++;
        logCurLen = 0;
      }
    }
    else if(c >= 32 && c < 127 && logCurLen < WEBLOG_LINE_LEN - 1)
      logCur[logCurLen++] = c;
  }
  portEXIT_CRITICAL(&logMux);
  return n;
}

void webLogSetEnabled(bool on)
{
  if(on == (logLines != NULL)) return;
  // malloc/free stay outside the critical section; writers check the pointer inside it
  char (*fresh)[WEBLOG_LINE_LEN] = on ? (char (*)[WEBLOG_LINE_LEN])calloc(WEBLOG_LINES, WEBLOG_LINE_LEN) : NULL;
  if(on && !fresh) return;
  portENTER_CRITICAL(&logMux);
  char (*old)[WEBLOG_LINE_LEN] = logLines;
  logLines = fresh;
  logHead += WEBLOG_LINES;   // readers skip whatever the old ring held
  logCurLen = 0;
  portEXIT_CRITICAL(&logMux);
  free(old);
}

bool webLogEnabled()
{
  return logLines != NULL;
}

uint32_t webLogHead()
{
  return logHead;
}

bool webLogGet(uint32_t seq, char *dst, size_t maxLen)
{
  bool ok = false;
  portENTER_CRITICAL(&logMux);
  if(logLines && (int32_t)(logHead - seq) > 0 && logHead - seq <= WEBLOG_LINES)
  {
    strncpy(dst, logLines[seq % WEBLOG_LINES], maxLen - 1);
    dst[maxLen - 1] = '\0';
    ok = dst[0] != '\0';   // slots of a freshly enabled ring are empty
  }
  portEXIT_CRITICAL(&logMux);
  return ok;
}
