#include "PskReporter.h"
#include "Common.h"
#include "Ft8Tx.h"
#include "Layout-FT8.h"
#include "Storage.h"

#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <time.h>

#define PSKR_HOST        "report.pskreporter.info"
#define PSKR_PORT        4739
#define PSKR_LOCAL_PORT  14739
#define PSKR_QUEUE       32            // 27 bytes each
#define PSKR_DEDUP       56            // 10 bytes each
#define PSKR_INTERVAL_MS 300000UL      // at most one datagram per 5 minutes
#define PSKR_FULL_GAP_MS 60000UL       // ... or per minute when the queue is full
#define PSKR_REPEAT_S    3600UL        // each call at most once per hour and band
#define PSKR_MAX_DGRAM   1400

typedef struct __attribute__((packed)) {
  char call[12];
  char loc[5];
  int8_t snr;
  uint8_t ft4;
  uint32_t freq;
  uint32_t t;
} PskrRec;

typedef struct __attribute__((packed)) {
  uint32_t hash;
  uint16_t band;
  uint32_t last;
} PskrSeen;

static PskrRec queue[PSKR_QUEUE];
static PskrSeen seen[PSKR_DEDUP];
static uint8_t qCount = 0, seenCount = 0;
static bool enabled = false, loaded = false;
static volatile int8_t webReq = -1;          // -1 none, 0/1 = set enabled (handled in pskrPoll)
static uint32_t nextSendMs = 0, sentRecords = 0, sentPackets = 0, lastSend = 0, lastTpl = 0, obsDomain = 0;
static uint8_t tplLeft = 3;
static char lastError[20] = "";
static WiFiUDP *udp = NULL;

static void pskrLoad() {
  if (loaded) return;
  loaded = true;
  Preferences p;
  if (!p.begin("pskr", true, STORAGE_PARTITION)) return;
  enabled = p.getBool("enabled", false);
  p.end();
}

static void pskrSave() {
  Preferences p;
  if (!p.begin("pskr", false, STORAGE_PARTITION)) return;
  p.putBool("enabled", enabled);
  p.end();
}

static void pskrSetEnabled(bool on) {
  if (on == enabled) return;
  enabled = on;
  pskrSave();
  if (on) {
    tplLeft = 3;
    nextSendMs = millis() + PSKR_INTERVAL_MS + esp_random() % 30000;
  } else {
    qCount = 0;
    if (udp) udp->stop(), delete udp, udp = NULL;
  }
  Serial.printf("[PSKR] %s\r\n", on ? "enabled" : "disabled");
}

static bool isGridTok(const char *s) {
  return strlen(s) == 4 && s[0] >= 'A' && s[0] <= 'R' && s[1] >= 'A' && s[1] <= 'R' &&
         isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) && strcmp(s, "RR73");
}

static bool looksLikeCall(const char *s) {
  size_t n = strlen(s);
  if (n < 3 || n > 11) return false;
  bool d = false, a = false;
  for (size_t i = 0; i < n; i++) {
    if (isdigit((unsigned char)s[i])) d = true;
    else if (s[i] >= 'A' && s[i] <= 'Z') a = true;
    else if (s[i] != '/') return false;
  }
  return d && a;
}

static uint32_t fnv(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
  return h;
}

// Returns true when this call+band was not reported within the last hour (and records it)
static bool dedupAccept(uint32_t hash, uint16_t band, uint32_t now) {
  for (uint8_t i = 0; i < seenCount; i++) {
    if (seen[i].hash == hash && seen[i].band == band) {
      if (now - seen[i].last < PSKR_REPEAT_S) return false;
      seen[i].last = now;
      return true;
    }
  }
  uint8_t slot = seenCount;
  if (seenCount < PSKR_DEDUP) {
    seenCount++;
  } else {
    slot = 0;   // table full: drop the oldest entry
    for (uint8_t i = 1; i < PSKR_DEDUP; i++) if (seen[i].last < seen[slot].last) slot = i;
  }
  seen[slot].hash = hash;
  seen[slot].band = band;
  seen[slot].last = now;
  return true;
}

void pskrOnDecode(const char *text, int snr, uint16_t audioHz) {
  pskrLoad();
  if (!enabled || !text) return;
  time_t now = time(NULL);
  if (now < 1700000000) return;
  const char *own = ft8TxCallsign();

  char buf[40];
  strlcpy(buf, text, sizeof(buf));
  char *tok[5];
  uint8_t n = 0;
  for (char *p = strtok(buf, " "); p && n < 5; p = strtok(NULL, " ")) tok[n++] = p;
  if (n < 2) return;

  uint8_t ci;
  if (!strcmp(tok[0], "CQ")) {
    ci = 1;
    if (n > 2 && strlen(tok[1]) >= 2 && strlen(tok[1]) <= 4) {
      bool letters = true;
      for (const char *c = tok[1]; *c; c++) if (*c < 'A' || *c > 'Z') letters = false;
      if (letters) ci = 2;
    }
  } else {
    ci = 1;
  }
  const char *call = tok[ci];
  if (call[0] == '<' || !looksLikeCall(call)) return;
  if (own[0] && !strcmp(call, own)) return;
  const char *loc = "";
  if (n > ci + 1 && isGridTok(tok[n - 1])) loc = tok[n - 1];

  uint32_t rf = (uint32_t)currentFrequency * 1000UL + audioHz;
  bool ft4 = ft8GetDigitalMode() == FT8_DIG_FT4;
  // The decode runs near the end of the slot it decoded, so step back half a period first
  uint32_t half = ft4 ? 15 : 30;                       // period in half-seconds
  uint32_t start2 = ((((uint32_t)now * 2) - half / 2) / half) * half;
  uint32_t slotStart = start2 / 2;

  if (qCount >= PSKR_QUEUE) return;                    // full: pskrPoll() sends soon
  if (!dedupAccept(fnv(call), (uint16_t)(rf / 1000000UL), (uint32_t)now)) return;
  PskrRec &r = queue[qCount++];
  memset(&r, 0, sizeof(r));
  strlcpy(r.call, call, sizeof(r.call));
  strlcpy(r.loc, loc, sizeof(r.loc));
  r.snr = (int8_t)constrain(snr, -127, 127);
  r.ft4 = ft4;
  r.freq = rf;
  r.t = slotStart;
}

static uint8_t *put8(uint8_t *p, uint8_t v) { *p++ = v; return p; }
static uint8_t *put16(uint8_t *p, uint16_t v) { *p++ = v >> 8; *p++ = v; return p; }
static uint8_t *put32(uint8_t *p, uint32_t v) { p = put16(p, v >> 16); return put16(p, v); }
static uint8_t *putStr(uint8_t *p, const char *s) {
  size_t n = strlen(s);
  *p++ = n;
  memcpy(p, s, n);
  return p + n;
}
static uint8_t *pad4(uint8_t *start, uint8_t *p) {
  while ((p - start) & 3) *p++ = 0;
  return p;
}

static const uint8_t tplReceiver[] = {
  0x00,0x03,0x00,0x24,0x99,0x92,0x00,0x03,0x00,0x01,0x80,0x02,0xFF,0xFF,0x00,0x00,0x76,0x8F,
  0x80,0x04,0xFF,0xFF,0x00,0x00,0x76,0x8F,0x80,0x08,0xFF,0xFF,0x00,0x00,0x76,0x8F,0x00,0x00};
static const uint8_t tplSender[] = {
  0x00,0x02,0x00,0x44,0x99,0x93,0x00,0x08,0x80,0x01,0xFF,0xFF,0x00,0x00,0x76,0x8F,
  0x80,0x05,0x00,0x04,0x00,0x00,0x76,0x8F,0x80,0x06,0x00,0x01,0x00,0x00,0x76,0x8F,
  0x80,0x07,0x00,0x01,0x00,0x00,0x76,0x8F,0x80,0x0A,0xFF,0xFF,0x00,0x00,0x76,0x8F,
  0x80,0x0B,0x00,0x01,0x00,0x00,0x76,0x8F,0x80,0x03,0xFF,0xFF,0x00,0x00,0x76,0x8F,
  0x00,0x96,0x00,0x04};

static void setErr(const char *e) { strlcpy(lastError, e, sizeof(lastError)); }

// Build one datagram from the queue into buf; returns its length and how many records it holds
static size_t pskrBuild(uint8_t *buf, uint32_t now, bool tpl, uint8_t *nSent);

static void pskrSend(uint32_t nowMs) {
  time_t now = time(NULL);
  const char *call = ft8TxCallsign(), *grid = ft8TxGrid();
  if (WiFi.status() != WL_CONNECTED) { setErr("no wifi"); return; }
  if (now < 1700000000) { setErr("no clock"); return; }
  if (!call[0] || !grid[0]) { setErr("no call/grid"); return; }
  // DNS blocks loop(), and the FT8 ADC ring only holds 640 ms of audio: resolve rarely and keep the IP
  static IPAddress ip;
  static uint32_t resolvedAt = 0;
  if (!ip || (uint32_t)now - resolvedAt > 6 * 3600UL) {
    IPAddress fresh;
    if (WiFi.hostByName(PSKR_HOST, fresh) && fresh) {
      ip = fresh;
      resolvedAt = (uint32_t)now;
    } else if (!ip) {
      setErr("dns");
      return;
    }
  }
  if (!udp) {
    udp = new WiFiUDP();
    if (!udp->begin(PSKR_LOCAL_PORT)) { delete udp; udp = NULL; setErr("udp"); return; }
  }

  uint8_t buf[PSKR_MAX_DGRAM];
  bool tpl = tplLeft > 0 || (uint32_t)now - lastTpl >= 3600;
  uint8_t sent = 0;
  size_t len = pskrBuild(buf, (uint32_t)now, tpl, &sent);

  bool ok = udp->beginPacket(ip, PSKR_PORT) && udp->write(buf, len) == len && udp->endPacket();
  if (!ok) {
    setErr("send");
    udp->stop();
    delete udp;
    udp = NULL;
    return;
  }
  setErr("");
  if (tpl) { lastTpl = (uint32_t)now; if (tplLeft) tplLeft--; }
  sentRecords += sent;
  sentPackets++;
  lastSend = (uint32_t)now;
  memmove(queue, queue + sent, (qCount - sent) * sizeof(PskrRec));
  qCount -= sent;
  Serial.printf("[PSKR] sent %u records (%u bytes)\r\n", sent, (unsigned)len);
}

static size_t pskrBuild(uint8_t *buf, uint32_t now, bool tpl, uint8_t *nSent) {
  const char *call = ft8TxCallsign(), *grid = ft8TxGrid();
  uint8_t *p = buf;
  p = put16(p, 0x000A);
  p = put16(p, 0);                       // length, patched below
  p = put32(p, (uint32_t)now);
  p = put32(p, sentRecords);
  p = put32(p, obsDomain);
  if (tpl) {
    memcpy(p, tplReceiver, sizeof(tplReceiver)); p += sizeof(tplReceiver);
    memcpy(p, tplSender, sizeof(tplSender)); p += sizeof(tplSender);
  }
  // receiver data set (in every datagram)
  char sw[32];
  snprintf(sw, sizeof(sw), "ATS-Mini ESP32 FT8 v%d", VER_APP);
  uint8_t *set = p;
  p = put16(p, 0x9992);
  p = put16(p, 0);
  p = putStr(p, call);
  p = putStr(p, grid);
  p = putStr(p, sw);
  p = pad4(set, p);
  put16(set + 2, p - set);
  // sender data set
  set = p;
  p = put16(p, 0x9993);
  p = put16(p, 0);
  uint8_t sent = 0;
  while (sent < qCount && (p - buf) + 40 < PSKR_MAX_DGRAM) {
    const PskrRec &r = queue[sent++];
    p = putStr(p, r.call);
    p = put32(p, r.freq);
    p = put8(p, (uint8_t)r.snr);
    p = put8(p, 0);
    p = putStr(p, r.ft4 ? "FT4" : "FT8");
    p = put8(p, 1);
    p = putStr(p, r.loc);
    p = put32(p, r.t);
  }
  p = pad4(set, p);
  put16(set + 2, p - set);
  put16(buf + 2, p - buf);
  *nSent = sent;
  return p - buf;
}

void pskrPoll() {
  static bool init = false;
  pskrLoad();
  if (!init) {
    init = true;
    obsDomain = esp_random();
    nextSendMs = millis() + PSKR_INTERVAL_MS + esp_random() % 30000;
  }
  int8_t req = webReq;
  if (req >= 0) { webReq = -1; pskrSetEnabled(req == 1); }
  if (!enabled) return;
  uint32_t nowMs = millis();
  bool due = (int32_t)(nowMs - nextSendMs) >= 0;
  static uint32_t lastFullMs = 0;
  bool full = qCount >= PSKR_QUEUE && nowMs - lastFullMs >= PSKR_FULL_GAP_MS;
  if (!qCount || (!due && !full)) {
    if (due && !qCount) nextSendMs = nowMs + 60000;   // nothing to send: collect for a minute (spacing stays >= 5 min)
    return;
  }
  if (full) lastFullMs = nowMs;
  nextSendMs = nowMs + PSKR_INTERVAL_MS + esp_random() % 30000;
  pskrSend(nowMs);
}

String pskrStatusJson() {
  pskrLoad();
  int32_t next = enabled ? (int32_t)(nextSendMs - millis()) / 1000 : 0;
  if (next < 0) next = 0;
  char b[190];
  snprintf(b, sizeof(b),
           "{\"enabled\":%s,\"queued\":%u,\"sent_records\":%lu,\"sent_packets\":%lu,\"last_send\":%lu,\"last_error\":\"%s\",\"next_send_in\":%ld}",
           enabled ? "true" : "false", qCount, (unsigned long)sentRecords, (unsigned long)sentPackets,
           (unsigned long)lastSend, lastError, (long)next);
  return String(b);
}

void pskrSerialCommand(const char *args) {
  pskrLoad();
  if (*args == 'd') {
    // Debug: hex dump of the datagram the next send would build (with templates); nothing is sent.
    // With an empty queue, one sample record is used and removed again.
    bool sample = qCount == 0;
    if (sample) {
      memset(&queue[0], 0, sizeof(PskrRec));
      strlcpy(queue[0].call, "N1DQ", sizeof(queue[0].call));
      strlcpy(queue[0].loc, "FN42", sizeof(queue[0].loc));
      queue[0].snr = -12;
      queue[0].freq = 21074000UL + 1500;
      queue[0].t = (uint32_t)time(NULL) / 15 * 15;
      qCount = 1;
    }
    uint8_t buf[PSKR_MAX_DGRAM];   // on the loop() stack (8 KB), like pskrSend()
    uint8_t n = 0;
    size_t len = pskrBuild(buf, (uint32_t)time(NULL), true, &n);
    if (sample) qCount = 0;
    Serial.printf("[PSKR] dump %u bytes, %u records:", (unsigned)len, n);
    for (size_t i = 0; i < len; i++) Serial.printf("%s%02X", (i % 32) ? " " : "\r\n[PSKR] ", buf[i]);
    Serial.println();
    return;
  }
  if (*args == '1') pskrSetEnabled(true);
  else if (*args == '0') pskrSetEnabled(false);
  Serial.printf("[PSKR] %s\r\n", pskrStatusJson().c_str());
}

void pskrRegisterWeb(AsyncWebServer &server) {
  server.on("/api/pskr", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    String action;
    if (request->hasParam("action", true)) action = request->getParam("action", true)->value();
    else if (request->hasParam("action")) action = request->getParam("action")->value();
    if (action == "enable") {
      String on;
      if (request->hasParam("on", true)) on = request->getParam("on", true)->value();
      else if (request->hasParam("on")) on = request->getParam("on")->value();
      webReq = on == "1" ? 1 : 0;
    }
    request->send(200, "application/json", pskrStatusJson());
  });
}
