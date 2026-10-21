#include "Common.h"
#include "Storage.h"
#include "Layout-FT8.h"
#include "Options.h"

#include <Preferences.h>
#include <ESPAsyncWebServer.h>

static bool optLog = true, optHiRes = true, optWebWf = true;
static volatile int8_t reqLog = -1, reqHiRes = -1, reqWebWf = -1;   // -1 none, else the new value

static void optionsSave() {
  Preferences p;
  if (!p.begin("options", false, STORAGE_PARTITION)) return;
  p.putBool("log", optLog);
  p.putBool("hires", optHiRes);
  p.putBool("webwf", optWebWf);
  p.end();
}

void optionsLoad() {
  Preferences p;
  if (p.begin("options", true, STORAGE_PARTITION)) {
    optLog = p.getBool("log", true);
    optHiRes = p.getBool("hires", true);
    optWebWf = p.getBool("webwf", true);
    p.end();
  }
  webLogSetEnabled(optLog);
  ft8SetHiResEnabled(optHiRes && optWebWf);
}

void optionsApply() {
  bool changed = false;
  int8_t r;
  if ((r = reqLog) >= 0) { reqLog = -1; optLog = r; changed = true; }
  if ((r = reqHiRes) >= 0) { reqHiRes = -1; optHiRes = r; changed = true; }
  if ((r = reqWebWf) >= 0) { reqWebWf = -1; optWebWf = r; changed = true; }
  if (!changed) return;
  webLogSetEnabled(optLog);
  ft8SetHiResEnabled(optHiRes && optWebWf);
  optionsSave();
  Serial.printf("[OPT] log %s, hi-res waterfall %s, web waterfall %s (8-bit heap %u)\r\n", optLog ? "on" : "off",
                optHiRes ? "on" : "off", optWebWf ? "on" : "off", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

bool optionWebWaterfall() {
  return optWebWf;
}

static String optionsJson() {
  char b[200];
  snprintf(b, sizeof(b),
           "{\"log\":%s,\"hires\":%s,\"webwf\":%s,\"heap\":%u,\"largest\":%u,\"heap_min\":%u}",
           (reqLog >= 0 ? reqLog : optLog) ? "true" : "false", (reqHiRes >= 0 ? reqHiRes : optHiRes) ? "true" : "false",
           (reqWebWf >= 0 ? reqWebWf : optWebWf) ? "true" : "false",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  return String(b);
}

void optionsRegisterWeb(AsyncWebServer &server) {
  // POST action=set&key=log|hires|webwf&on=0|1 ; GET returns the options and the heap
  server.on("/api/options", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if (request->hasParam("key", true) && request->hasParam("on", true)) {
      String key = request->getParam("key", true)->value();
      int8_t on = request->getParam("on", true)->value() == "1" ? 1 : 0;
      if (key == "log") reqLog = on;
      else if (key == "hires") reqHiRes = on;
      else if (key == "webwf") reqWebWf = on;
    }
    request->send(200, "application/json", optionsJson());
  });
}
