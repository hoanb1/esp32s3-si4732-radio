#ifndef PSKREPORTER_H
#define PSKREPORTER_H

#include <Arduino.h>

// PSKReporter receiver-report uploader (IPFIX over UDP, report.pskreporter.info:4739).
// Off by default; the enable flag is saved in NVS namespace "pskr".

void pskrOnDecode(const char *text, int snr, uint16_t audioHz);   // from the live decoder, per message
void pskrPoll();                                                  // from loop(): applies web/serial requests and sends
String pskrStatusJson();
void pskrSerialCommand(const char *args);                         // "E": status, "E1"/"E0": enable/disable
class AsyncWebServer;
void pskrRegisterWeb(AsyncWebServer &server);                     // GET/POST /api/pskr

#endif // PSKREPORTER_H
