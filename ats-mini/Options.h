#ifndef OPTIONS_H
#define OPTIONS_H

#include <Arduino.h>

//
// Optional features that cost heap while on, switchable from the web UI and saved in NVS
// (namespace "options"). The web handler only records a request; optionsApply() makes the
// change from the loop task, where the log ring and the hi-res rows are used.
//
//   log    radio log ring for the web page (WEBLOG_LINES x WEBLOG_LINE_LEN bytes)
//   hires  full-resolution web waterfall rows (~1.8 KB) and their 'H' WebSocket frames
//   webwf  waterfall rows to web pages at all (the LCD waterfall is not affected)
//
void optionsLoad();      // setup(), after the settings NVS partition is up
void optionsApply();     // loop task
bool optionWebWaterfall();
class AsyncWebServer;
void optionsRegisterWeb(AsyncWebServer &server);   // GET/POST /api/options

#endif // OPTIONS_H
