#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "Layout-FT8.h"
#include "WebUI.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <NTPClient.h>
#include <ESPmDNS.h>

#define CONNECT_TIME    1500  // Delay before connecting WiFi after request
#define MAX_WIFI_SLOTS  8     // Support saving and scanning up to 8 WiFi networks

// Default WiFi networks requested by user (HH-1301 primary on 192.168.3.65, HH-1301_IoT secondary)
static const char *DEFAULT_WIFI_SSID_1 = "HH-1301";
static const char *DEFAULT_WIFI_PASS_1 = "tutulala";
static const char *DEFAULT_WIFI_SSID_2 = "HH-1301_IoT";
static const char *DEFAULT_WIFI_PASS_2 = "tutulala";

//
// Access Point (AP) mode settings
//
static const char *apSSID    = "ATS-Mini-Radio";
static const char *apPWD     = 0;       // Open AP for easy access
static const int   apChannel = 6;       // WiFi channel number (1..13)
static const bool  apHideMe  = false;   // TRUE: disable SSID broadcast
static const int   apClients = 4;       // Maximum simultaneous connected clients

static uint16_t ajaxInterval = 1500;

static bool itIsTimeToWiFi = false;
static uint32_t connectTime = millis();
static bool webServerStarted = false;
static bool doScanRequested = false;

struct ScannedAP {
  char ssid[33];
  int32_t rssi;
  int32_t ch;
  bool enc;
};
static ScannedAP lastScannedAPs[16];
static int lastScannedCount = 0;

// Settings
String loginUsername = "";
String loginPassword = "";

// AsyncWebServer object on port 80
AsyncWebServer server(80);

// NTP Client to get UTC time
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org");

static bool wifiInitAP();
static bool wifiConnect();
static void webInit();
static void ensureDefaultWifiPrefs();
static void performWifiScan();

static void webSetConfig(AsyncWebServerRequest *request);
static const String webInputField(const String &name, const String &value, bool pass = false);
static const String webStyleSheet();
static const String webPage(const String &body);
static const String webUtcOffsetSelector();
static const String webThemeSelector();
static const String webRadioPage();
static const String webMemoryPage();
static const String webConfigPage();

// Ensure HH-1301 and HH-1301_IoT (pass: tutulala) are present in NVS WiFi slots 1 & 2
static void ensureDefaultWifiPrefs()
{
  prefs.begin("network", false, STORAGE_PARTITION);

  String ssids[MAX_WIFI_SLOTS];
  String passes[MAX_WIFI_SLOTS];

  for (int i = 0; i < MAX_WIFI_SLOTS; i++) {
    char kS[16], kP[16];
    snprintf(kS, sizeof(kS), "wifissid%d", i + 1);
    snprintf(kP, sizeof(kP), "wifipass%d", i + 1);
    ssids[i] = prefs.getString(kS, "");
    passes[i] = prefs.getString(kP, "");
  }

  if (ssids[0] != DEFAULT_WIFI_SSID_1 || passes[0] != DEFAULT_WIFI_PASS_1 ||
      ssids[1] != DEFAULT_WIFI_SSID_2 || passes[1] != DEFAULT_WIFI_PASS_2) {
    // Place HH-1301 in slot 1 and HH-1301_IoT in slot 2, preserving any other custom SSIDs in slots 3..8
    String newSsids[MAX_WIFI_SLOTS];
    String newPasses[MAX_WIFI_SLOTS];
    newSsids[0] = DEFAULT_WIFI_SSID_1;
    newPasses[0] = DEFAULT_WIFI_PASS_1;
    newSsids[1] = DEFAULT_WIFI_SSID_2;
    newPasses[1] = DEFAULT_WIFI_PASS_2;
    int dst = 2;
    for (int i = 0; i < MAX_WIFI_SLOTS && dst < MAX_WIFI_SLOTS; i++) {
      if (ssids[i] != "" && ssids[i] != DEFAULT_WIFI_SSID_1 && ssids[i] != DEFAULT_WIFI_SSID_2) {
        newSsids[dst] = ssids[i];
        newPasses[dst] = passes[i];
        dst++;
      }
    }
    for (int i = 0; i < MAX_WIFI_SLOTS; i++) {
      char kS[16], kP[16];
      snprintf(kS, sizeof(kS), "wifissid%d", i + 1);
      snprintf(kP, sizeof(kP), "wifipass%d", i + 1);
      prefs.putString(kS, newSsids[i]);
      prefs.putString(kP, newPasses[i]);
    }
  }
  prefs.end();
}

static void performWifiScan()
{
  int n = WiFi.scanNetworks(false, false, false, 120);
  lastScannedCount = 0;
  Serial.printf("[WiFi] Scanned %d 2.4GHz networks:\r\n", n);
  for (int i = 0; i < n && lastScannedCount < 16; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    // Deduplicate same SSID keeping strongest RSSI
    bool exists = false;
    for (int j = 0; j < lastScannedCount; j++) {
      if (strcmp(lastScannedAPs[j].ssid, s.c_str()) == 0) {
        if (WiFi.RSSI(i) > lastScannedAPs[j].rssi) {
          lastScannedAPs[j].rssi = WiFi.RSSI(i);
          lastScannedAPs[j].ch = WiFi.channel(i);
        }
        exists = true;
        break;
      }
    }
    if (!exists) {
      strncpy(lastScannedAPs[lastScannedCount].ssid, s.c_str(), 32);
      lastScannedAPs[lastScannedCount].ssid[32] = '\0';
      lastScannedAPs[lastScannedCount].rssi = WiFi.RSSI(i);
      lastScannedAPs[lastScannedCount].ch = WiFi.channel(i);
      lastScannedAPs[lastScannedCount].enc = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      Serial.printf("  - '%s' (%d dBm, CH%d)\r\n", lastScannedAPs[lastScannedCount].ssid, (int)WiFi.RSSI(i), (int)WiFi.channel(i));
      lastScannedCount++;
    }
  }
  WiFi.scanDelete();
}

//
// Delayed WiFi connection & scan handling
//
void netRequestConnect()
{
  connectTime = millis();
  itIsTimeToWiFi = true;
}

void netTickTime()
{
  if (doScanRequested) {
    doScanRequested = false;
    performWifiScan();
  }

  // Connect to WiFi if requested
  if(itIsTimeToWiFi && ((millis() - connectTime) > CONNECT_TIME))
  {
    netInit(wifiModeIdx, false);
    connectTime = millis();
    itIsTimeToWiFi = false;
    return;
  }

  // Auto-reconnect watchdog if STA drops connection temporarily (never block or drop into AP)
  static uint32_t lastWifiCheckMs = 0;
  static uint8_t disconnectTicks = 0;
  if (millis() - lastWifiCheckMs >= 6000) {
    lastWifiCheckMs = millis();
    if (wifiModeIdx > NET_AP_ONLY) {
      if (WiFi.status() != WL_CONNECTED) {
        disconnectTicks++;
        Serial.printf("[WiFi] STA disconnected (tick %u), non-blocking reconnect...\r\n", disconnectTicks);
        if ((WiFi.getMode() & WIFI_STA) == 0) {
          WiFi.mode(WIFI_STA);
        }
        WiFi.setSleep(false);
        if (disconnectTicks % 3 == 0) {
          prefs.begin("network", true, STORAGE_PARTITION);
          String s1 = prefs.getString("wifissid1", "HH-1301");
          String p1 = prefs.getString("wifipass1", "tutulala");
          prefs.end();
          WiFi.begin(s1.c_str(), p1.c_str());
        } else {
          WiFi.reconnect();
        }
      } else {
        if (disconnectTicks > 0) {
          Serial.printf("[WiFi] Reconnected! IP: %s | RSSI: %d dBm\r\n",
                        WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
          WiFi.setSleep(false);
          disconnectTicks = 0;
        }
      }
    }
  }
}

//
// Get current connection status
// (-1 - not connected, 0 - disabled, 1 - connected to AP, 2 - connected to WiFi network)
//
int8_t getWiFiStatus()
{
  wifi_mode_t mode = WiFi.getMode();

  switch(mode)
  {
    case WIFI_MODE_NULL:
      return(0);
    case WIFI_AP:
      return(WiFi.softAPgetStationNum()? 1 : -1);
    case WIFI_STA:
      return(WiFi.status()==WL_CONNECTED? 2 : -1);
    case WIFI_AP_STA:
      return((WiFi.status()==WL_CONNECTED)? 2 : WiFi.softAPgetStationNum()? 1 : -1);
    default:
      return(-1);
  }
}

char *getWiFiIPAddress()
{
  static char ip[16];
  return strcpy(ip, WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str() : "");
}

//
// Stop WiFi hardware
//
void netStop()
{
  wifi_mode_t mode = WiFi.getMode();

  if((mode==WIFI_STA) || (mode==WIFI_AP_STA))
    WiFi.disconnect(true);

  if((mode==WIFI_AP) || (mode==WIFI_AP_STA))
    WiFi.softAPdisconnect(true);

  WiFi.mode(WIFI_MODE_NULL);
}

//
// Initialize WiFi network and services
//
void netInit(uint8_t netMode, bool showStatus)
{
  ensureDefaultWifiPrefs();
  netStop();

  switch(netMode)
  {
    case NET_OFF:
      return;
    case NET_AP_ONLY:
      WiFi.mode(WIFI_AP);
      if(wifiInitAP() && showStatus) delay(800);
      break;
    case NET_AP_CONNECT:
    default:
      // Connect in pure WIFI_STA first (saves ~16KB RAM and avoids AP/STA channel conflict);
      // if STA fails in NET_AP_CONNECT mode, wifiConnect() automatically falls back to Hotspot AP!
      WiFi.mode(WIFI_STA);
      break;
  }

  // Initialize WiFi and try connecting to saved networks (HH-1301, HH-1301_IoT, etc.)
  if(netMode > NET_AP_ONLY && wifiConnect())
  {
    if(netMode != NET_SYNC)
    {
      webInit();
    }

    if(netMode != NET_SYNC && showStatus) delay(200);

    ntpClient.begin();
    ntpClient.setUpdateInterval(5*60*1000);
  }
  else if(netMode != NET_SYNC)
  {
    webInit();
  }

  if(netMode == NET_SYNC)
  {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
  }
}

bool ntpIsAvailable()
{
  return(ntpClient.isTimeSet());
}

bool ntpSyncTime()
{
  // Never block loop() with DNS/UDP timeouts once UTC clock is already set
  if(clockAvailable()) return(false);
  return(false);
}

static bool wifiInitAP()
{
  IPAddress ip(10, 1, 1, 1);
  IPAddress gateway(10, 1, 1, 1);
  IPAddress subnet(255, 255, 255, 0);

  WiFi.softAPConfig(ip, gateway, subnet);
  WiFi.softAP(apSSID, apPWD, apChannel, apHideMe, apClients);

  drawScreen(
    ("Hotspot AP: " + String(apSSID)).c_str(),
    ("IP: " + WiFi.softAPIP().toString()).c_str()
  );

  ajaxInterval = 1500;
  return(true);
}

//
// Connect to saved WiFi networks (HH-1301 primary, HH-1301_IoT secondary, up to 8 slots)
//
static bool wifiConnect()
{
  ensureDefaultWifiPrefs();

  prefs.begin("network", true, STORAGE_PARTITION);
  loginUsername = prefs.getString("loginusername", "");
  loginPassword = prefs.getString("loginpassword", "");

  String savedSsid[MAX_WIFI_SLOTS];
  String savedPass[MAX_WIFI_SLOTS];
  for (int i = 0; i < MAX_WIFI_SLOTS; i++) {
    char nameSSID[16], namePASS[16];
    snprintf(nameSSID, sizeof(nameSSID), "wifissid%d", i + 1);
    snprintf(namePASS, sizeof(namePASS), "wifipass%d", i + 1);
    savedSsid[i] = prefs.getString(nameSSID, "");
    savedPass[i] = prefs.getString(namePASS, "");
  }
  prefs.end();

  // First try Slot #1 (HH-1301) directly for fast boot without scan delay
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  if (savedSsid[0] != "") {
    Serial.printf("[WiFi] Fast-connecting to primary Slot #1: '%s'...\r\n", savedSsid[0].c_str());
    ft8AddLog("[INFO] Đang kết nối Wi-Fi ưu tiên '%s'...", savedSsid[0].c_str());
    drawScreen(("Đang kết nối: " + savedSsid[0]).c_str(), "Nhấn nút Encoder để bỏ qua");
    WiFi.begin(savedSsid[0].c_str(), savedPass[0].c_str());
    for (int t = 0; t < 16 && WiFi.status() != WL_CONNECTED; t++) {
      delay(500);
      if (digitalRead(ENCODER_PUSH_BUTTON) == LOW) {
        WiFi.disconnect();
        break;
      }
    }
  }

  // If primary Slot #1 did not connect, scan 2.4GHz and try remaining visible saved networks
  if (WiFi.status() != WL_CONNECTED) {
    drawScreen("Đang quét mạng Wi-Fi 2.4GHz...", "Tìm mạng Wi-Fi đã lưu...");
    performWifiScan();

    for (int s = 0; s < lastScannedCount && WiFi.status() != WL_CONNECTED; s++) {
      for (int i = 0; i < MAX_WIFI_SLOTS; i++) {
        if (savedSsid[i] != "" && savedSsid[i] == lastScannedAPs[s].ssid) {
          Serial.printf("[WiFi] Trying slot #%d: SSID='%s' (%d dBm)...\r\n",
                        i + 1, savedSsid[i].c_str(), (int)lastScannedAPs[s].rssi);
          ft8AddLog("[INFO] Đang kết nối Wi-Fi '%s'...", savedSsid[i].c_str());
          drawScreen(("Đang kết nối: " + savedSsid[i]).c_str(), "Nhấn nút Encoder để bỏ qua");

          WiFi.disconnect(false);
          delay(50);
          WiFi.setSleep(false);
          WiFi.begin(savedSsid[i].c_str(), savedPass[i].c_str());

          for (int t = 0; t < 14 && WiFi.status() != WL_CONNECTED; t++) {
            delay(500);
            if (digitalRead(ENCODER_PUSH_BUTTON) == LOW) {
              WiFi.disconnect();
              break;
            }
          }
          break;
        }
      }
    }
  }

  if(WiFi.status() != WL_CONNECTED)
  {
    WiFi.mode(WIFI_AP_STA);
    wifiInitAP();
    if (savedSsid[0] != "") {
      WiFi.begin(savedSsid[0].c_str(), savedPass[0].c_str());
    }
    Serial.printf("[WiFi] Could not connect to STA yet. Running AP+STA '%s' at %s\r\n",
                  apSSID, WiFi.softAPIP().toString().c_str());
    ft8AddLog("[WARN] Chưa kết nối được Wi-Fi, phát AP+STA: %s (%s)",
              apSSID, WiFi.softAPIP().toString().c_str());
    drawScreen(("AP: " + String(apSSID)).c_str(), ("Web UI: " + WiFi.softAPIP().toString()).c_str());
    return(false);
  }
  else
  {
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    Serial.printf("[WiFi] CONNECTED to '%s' | IP: %s | RSSI: %d dBm\r\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    ft8AddLog("[INFO] Đã kết nối Wi-Fi '%s' - IP: %s (%d dBm)",
              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    drawScreen(
      ("WiFi: " + WiFi.SSID()).c_str(),
      ("IP: " + WiFi.localIP().toString()).c_str()
    );
    ajaxInterval = 1000;
    return(true);
  }
}

//
// Initialize internal web server with full FT8 + Multi-WiFi APIs
//
static void webInit()
{
  if (webServerStarted) return;
  webServerStarted = true;

  // Main FT8 Web UI page - stream directly from Flash/PROGMEM (zero heap copy)
  server.on("/", HTTP_GET, [] (AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(
      200, "text/html; charset=utf-8",
      (const uint8_t *)FT8_WEBUI_HTML, sizeof(FT8_WEBUI_HTML) - 1
    );
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // Legacy status / memory / config pages
  server.on("/status", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html; charset=utf-8", webRadioPage());
  });

  server.on("/memory", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html; charset=utf-8", webMemoryPage());
  });

  server.on("/config", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(loginUsername != "" && loginPassword != "")
      if(!request->authenticate(loginUsername.c_str(), loginPassword.c_str()))
        return request->requestAuthentication();
    request->send(200, "text/html; charset=utf-8", webConfigPage());
  });

  server.on("/setconfig", HTTP_ANY, webSetConfig);

  // /api/status - Real-time radio, FT8 slot, and WiFi status
  server.on("/api/status", HTTP_GET, [] (AsyncWebServerRequest *request) {
    char freqStr[24];
    const char *unit = "kHz";
    if (currentMode == FM) {
      snprintf(freqStr, sizeof(freqStr), "%.2f", currentFrequency / 100.0f);
      unit = "MHz";
    } else {
      double fKhz = (double)currentFrequency + (double)currentBFO / 1000.0;
      snprintf(freqStr, sizeof(freqStr), "%.2f", fKhz);
    }

    int16_t cal = (currentMode == USB) ? getCurrentBand()->usbCal :
                  ((currentMode == LSB) ? getCurrentBand()->lsbCal : 0);
    bool isAp = (WiFi.status() != WL_CONNECTED);
    String ip = isAp ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    String ssid = isAp ? String(apSSID) : WiFi.SSID();
    int8_t curAvc = isSSB() ? SsbAvcIdx : AmAvcIdx;

    char json[800];
    snprintf(json, sizeof(json),
      "{\"freq_khz\":\"%s\",\"unit\":\"%s\",\"band\":\"%s\",\"band_id\":%d,"
      "\"mode\":\"%s\",\"mode_id\":%u,\"dig_mode\":%u,\"bfo\":%d,\"cal\":%d,"
      "\"bw\":\"%s\",\"bw_idx\":%d,\"step\":\"%s\","
      "\"agc\":%d,\"att_db\":%d,\"avc\":%d,\"squelch\":%u,"
      "\"rssi\":%u,\"snr\":%u,\"s_level\":%d,"
      "\"lcd_view\":%u,\"is_ap\":%s,\"ip\":\"%s\",\"ssid\":\"%s\","
      "\"synced\":%s,\"sync_src\":\"%s\",\"slot\":%u,\"decoding\":%s,"
      "\"last_duration_ms\":%lu,\"volume\":%u,\"mute\":%s,"
      "\"audio_level\":%u,\"heap_free\":%u,\"msg_count\":%lu}",
      freqStr, unit, getCurrentBand()->bandName, bandIdx,
      bandModeDesc[currentMode], (unsigned int)currentMode, (unsigned int)ft8GetDigitalMode(), (int)currentBFO, (int)cal,
      getCurrentBandwidth()->desc, (int)bands[bandIdx].bandwidthIdx, getCurrentStep()->desc,
      (int)agcIdx, (int)agcNdx, (int)curAvc, (unsigned int)currentSquelch,
      rssi, snr, getStrength(rssi),
      ft8GetViewMode(), isAp ? "true" : "false", ip.c_str(), ssid.c_str(),
      clockAvailable() ? "true" : "false", ntpIsAvailable() ? "NTP" : "Manual",
      ft8GetCurrentSlotSecond(), ft8IsDecodingNow() ? "true" : "false",
      (unsigned long)ft8GetLastDurationMs(), volume,
      ft8IsSpeakerMuted() ? "true" : "false",
      ft8GetAudioLevel(), (unsigned int)ESP.getFreeHeap(),
      (unsigned long)ft8GetTotalDecoded()
    );
    request->send(200, "application/json", json);
  });

  // /api/bookmarks - Return saved radio bookmarks from NVS memories[]
  server.on("/api/bookmarks", HTTP_GET, [] (AsyncWebServerRequest *request) {
    String out;
    out.reserve(1024);
    out = "[";
    bool first = true;
    int maxSlots = getTotalMemories();
    if (maxSlots > 24) maxSlots = 24;
    for (int i = 0; i < maxSlots; i++) {
      if (!memories[i].freq) continue;
      if (!first) out += ",";
      first = false;
      char nameBuf[16];
      strncpy(nameBuf, memories[i].name, 9);
      nameBuf[9] = '\0';
      if (nameBuf[0] == '\0') {
        uint8_t b = memories[i].band < getTotalBands() ? memories[i].band : 0;
        snprintf(nameBuf, sizeof(nameBuf), "%s #%d", bands[b].bandName, i + 1);
      }
      uint8_t m = memories[i].mode <= 3 ? memories[i].mode : USB;
      uint8_t b = memories[i].band < getTotalBands() ? memories[i].band : 1;
      char item[160];
      snprintf(item, sizeof(item),
        "{\"slot\":%d,\"name\":\"%s\",\"freq_hz\":%lu,\"mode_id\":%u,\"mode\":\"%s\",\"band_id\":%u,\"band\":\"%s\"}",
        i, nameBuf, (unsigned long)memories[i].freq, (unsigned int)m, bandModeDesc[m], (unsigned int)b, bands[b].bandName
      );
      out += item;
    }
    out += "]";
    request->send(200, "application/json", out);
  });

  // /api/waterfall - 36 x 120 binary waterfall data (4,320 bytes)
  server.on("/api/waterfall", HTTP_GET, [] (AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(
      200, "application/octet-stream", ft8GetWebWaterfallBuffer(), 36 * 120
    );
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  // /api/ft8/traffic - JSON array of decoded FT8 messages
  server.on("/api/ft8/traffic", HTTP_GET, [] (AsyncWebServerRequest *request) {
    String out = "[";
    int cnt = ft8GetTrafficCount();
    for (int i = 0; i < cnt; i++) {
      const FT8MessageEntry *e = ft8GetTrafficEntry(i);
      if (!e) continue;
      if (i > 0) out += ",";
      char item[192];
      snprintf(item, sizeof(item),
        "{\"time\":\"%s\",\"snr\":%d,\"dt\":%.1f,\"freq\":%u,\"call\":\"%s\",\"msg\":\"%s\"}",
        e->timeStr, (int)e->snr, e->dt, e->freq, e->call, e->msg
      );
      out += item;
    }
    out += "]";
    request->send(200, "application/json", out);
  });

  // /api/logs - JSON array of system logs
  server.on("/api/logs", HTTP_GET, [] (AsyncWebServerRequest *request) {
    request->send(200, "application/json", ft8GetLogsJson());
  });

  // /api/timesync - Set UTC time from browser epoch
  server.on("/api/timesync", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if (request->hasParam("epoch")) {
      uint32_t ep = strtoul(request->getParam("epoch")->value().c_str(), NULL, 10);
      ft8SetEpochTime(ep);
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  // /api/control - Radio & FT8 control actions
  server.on("/api/control", HTTP_POST, [] (AsyncWebServerRequest *request) {
    String action = "";
    if (request->hasParam("action", true)) action = request->getParam("action", true)->value();
    else if (request->hasParam("action")) action = request->getParam("action")->value();

    auto getParamStr = [&](const char *name) -> String {
      if (request->hasParam(name, true)) return request->getParam(name, true)->value();
      if (request->hasParam(name)) return request->getParam(name)->value();
      return "";
    };

    auto tuneBandDigitalFreq = [&](int bId, uint8_t digMode) {
      // Standard FT8 & FT4 dial frequencies for bands[18..26] (160M..10M)
      static const uint16_t ft8Khz[9] = { 1840, 3573, 7074, 10136, 14074, 18100, 21074, 24915, 28074 };
      static const uint16_t ft4Khz[9] = { 1840, 3575, 7047, 10140, 14080, 18104, 21140, 24919, 28180 };
      static const int16_t  ft4Bfo[9] = {    0,  500,  500,     0,     0,     0,     0,     0,     0 };
      if (bId >= 18 && bId <= 26) {
        int idx = bId - 18;
        uint16_t khz = (digMode == FT8_DIG_FT4) ? ft4Khz[idx] : ft8Khz[idx];
        int16_t  bfo = (digMode == FT8_DIG_FT4) ? ft4Bfo[idx] : 0;
        bands[bId].bandMode = USB;
        bands[bId].currentFreq = khz;
        selectBand((uint8_t)bId, false);
        updateFrequency(khz, false);
        updateBFO(bfo, false);
      }
    };

    if (action == "volume") {
      int v = getParamStr("val").toInt();
      if (v < 0) v = 0;
      if (v > 63) v = 63;
      volume = (uint8_t)v;
      rx.setVolume(volume);
      prefsRequestSave(SAVE_SETTINGS);
    } else if (action == "mode") {
      int m = getParamStr("mode").toInt();
      if (m >= 0 && m <= 3) {
        ft8SetDigitalMode(FT8_DIG_OFF);
        if (m == FM && bandIdx != 0) {
          bandIdx = 0;
        } else if (m != FM && bandIdx == 0) {
          bandIdx = (m == AM) ? 16 : 22; // Default MW2 for AM or 20M for USB/LSB
        }
        bands[bandIdx].bandMode = (uint8_t)m;
        currentMode = (uint8_t)m;
        selectBand(bandIdx, false);
        prefsRequestSave(SAVE_ALL);
      }
    } else if (action == "dig_mode") {
      int dm = getParamStr("val").toInt();
      if (dm >= 0 && dm <= 2) {
        ft8SetDigitalMode((uint8_t)dm);
        if (dm == FT8_DIG_FT8 || dm == FT8_DIG_FT4) {
          if (bandIdx < 18 || bandIdx > 26) {
            bandIdx = 22; // Default 20M amateur band
          }
          tuneBandDigitalFreq(bandIdx, (uint8_t)dm);
          prefsRequestSave(SAVE_ALL);
        }
      }
    } else if (action == "freq") {
      double f = getParamStr("freq").toDouble();
      String u = getParamStr("unit");
      u.toLowerCase();
      if (f > 0) {
        // Normalize frequency according to unit or smart range detection
        double khzVal = 0.0;
        if (u == "mhz") {
          khzVal = f * 1000.0;
        } else if (u == "khz") {
          khzVal = f;
        } else if (u == "hz") {
          khzVal = f / 1000.0;
        } else {
          // Auto-detect MHz vs kHz vs 0.01kHz dial digits (e.g. 2155000 -> 21550.00 kHz)
          if (f >= 64.0 && f <= 108.0) {
            khzVal = f * 1000.0; // FM MHz (64..108 MHz)
          } else if (f >= 0.15 && f <= 30.0) {
            khzVal = f * 1000.0; // HF/MW MHz (0.15..30.0 MHz)
          } else if (f >= 150.0 && f <= 30000.0) {
            khzVal = f;          // HF/MW kHz (150..30000 kHz)
          } else if (f >= 64000.0 && f <= 108000.0) {
            khzVal = f;          // FM kHz (64000..108000 kHz)
          } else if (f > 30000.0 && f <= 300000.0) {
            khzVal = f / 10.0;
          } else if (f > 300000.0 && f <= 3000000.0) {
            khzVal = f / 100.0;  // e.g. 2155000 -> 21550.00 kHz
          } else if (f > 3000000.0 && f <= 30000000.0) {
            khzVal = f / 1000.0; // Hz -> kHz
          }
        }

        if (khzVal >= 64000.0 && khzVal <= 108000.0) {
          uint16_t fmFreq = (uint16_t)round(khzVal / 10.0);
          ft8SetDigitalMode(FT8_DIG_OFF);
          if (bandIdx != 0 || currentMode != FM) {
            bandIdx = 0;
            bands[0].bandMode = FM;
            bands[0].currentFreq = fmFreq;
            selectBand(0, false);
          }
          updateFrequency(fmFreq, false);
          prefsRequestSave(SAVE_ALL);
        } else if (khzVal >= 150.0 && khzVal <= 30000.0) {
          uint32_t hz = (uint32_t)round(khzVal * 1000.0);
          uint16_t khz = (uint16_t)(hz / 1000UL);
          int16_t remBfo = (int16_t)(hz % 1000UL);
          uint8_t keepMode = (currentMode == FM) ? AM : currentMode;

          if (bandIdx == 0 || !isFreqInBand(getCurrentBand(), khz)) {
            int matchedBand = 1; // Fallback to ALL band (150..30000 kHz)
            for (int i = getTotalBands() - 1; i >= 2; i--) {
              if (bands[i].bandMode != FM && isFreqInBand(&bands[i], khz)) {
                matchedBand = i;
                break;
              }
            }
            bandIdx = matchedBand;
            bands[bandIdx].currentFreq = khz;
            if (currentMode != FM) {
              bands[bandIdx].bandMode = keepMode;
            }
            selectBand(bandIdx, false);
          }
          updateFrequency(khz, false);
          if (isSSB()) updateBFO(remBfo, false);
          if (currentMode != USB) ft8SetDigitalMode(FT8_DIG_OFF);
          prefsRequestSave(SAVE_ALL);
        }
      }
    } else if (action == "tune") {
      int delta = getParamStr("delta").toInt();
      if (currentMode == FM) {
        updateFrequency(currentFrequency + delta * 10, true);
      } else {
        int32_t newKhz = (int32_t)currentFrequency + delta;
        if (newKhz < 150) newKhz = 150;
        if (newKhz > 30000) newKhz = 30000;
        int16_t savedBfo = currentBFO;
        uint8_t keepMode = currentMode;
        if (!isFreqInBand(getCurrentBand(), (uint16_t)newKhz)) {
          int matchedBand = 1; // ALL band (150..30000 kHz)
          for (int i = getTotalBands() - 1; i >= 2; i--) {
            if (bands[i].bandMode != FM && isFreqInBand(&bands[i], (uint16_t)newKhz)) {
              matchedBand = i;
              break;
            }
          }
          bandIdx = matchedBand;
          bands[bandIdx].currentFreq = (uint16_t)newKhz;
          bands[bandIdx].bandMode = keepMode;
          selectBand(bandIdx, false);
        }
        updateFrequency((uint16_t)newKhz, false);
        if (isSSB() && savedBfo != 0) updateBFO(savedBfo, false);
      }
      prefsRequestSave(SAVE_CUR_BAND);
    } else if (action == "bfo_step") {
      int delta = getParamStr("delta").toInt();
      if (isSSB()) {
        updateBFO(currentBFO + delta, true);
        prefsRequestSave(SAVE_CUR_BAND);
      }
    } else if (action == "bfo_reset") {
      if (isSSB()) {
        updateBFO(0, true);
        prefsRequestSave(SAVE_CUR_BAND);
      }
    } else if (action == "save_cal") {
      int16_t newCal = 0;
      if (currentMode == USB) {
        bands[bandIdx].usbCal += currentBFO;
        newCal = bands[bandIdx].usbCal;
      } else if (currentMode == LSB) {
        bands[bandIdx].lsbCal += currentBFO;
        newCal = bands[bandIdx].lsbCal;
      }
      currentBFO = 0;
      updateBFO(0, true);
      prefsRequestSave(SAVE_BANDS, true);
      char buf[64];
      snprintf(buf, sizeof(buf), "{\"ok\":true,\"cal\":%d}", (int)newCal);
      request->send(200, "application/json", buf);
      return;
    } else if (action == "bw") {
      int d = getParamStr("val").toInt();
      doBandwidth(d ? d : 1);
      prefsRequestSave(SAVE_CUR_BAND);
    } else if (action == "bw_set") {
      int idx = getParamStr("idx").toInt();
      if (idx >= 0 && idx <= 6) {
        bands[bandIdx].bandwidthIdx = (int8_t)idx;
        doBandwidth(0);
        prefsRequestSave(SAVE_CUR_BAND);
      }
    } else if (action == "step") {
      int d = getParamStr("val").toInt();
      doStep(d ? d : 1);
      prefsRequestSave(SAVE_CUR_BAND);
    } else if (action == "agc") {
      int d = getParamStr("val").toInt();
      doAgc(d ? d : 1);
      prefsRequestSave(SAVE_SETTINGS);
    } else if (action == "agc_set") {
      int v = getParamStr("val").toInt();
      if (v < 0) v = 0;
      if (currentMode == FM) FmAgcIdx = (v > 27) ? 27 : v;
      else if (isSSB()) SsbAgcIdx = (v > 1) ? 1 : v;
      else AmAgcIdx = (v > 37) ? 37 : v;
      doAgc(0);
      prefsRequestSave(SAVE_SETTINGS);
    } else if (action == "avc") {
      int d = getParamStr("val").toInt();
      doAvc(d ? d : 1);
      prefsRequestSave(SAVE_SETTINGS);
    } else if (action == "avc_set") {
      int v = (getParamStr("val").toInt() / 2) * 2;
      if (v < 12) v = 12;
      if (v > 90) v = 90;
      if (isSSB()) SsbAvcIdx = v;
      else AmAvcIdx = v;
      doAvc(0);
      prefsRequestSave(SAVE_SETTINGS);
    } else if (action == "squelch") {
      int v = getParamStr("val").toInt();
      if (v < 0) v = 0;
      if (v > 127) v = 127;
      currentSquelch = (uint8_t)v;
      prefsRequestSave(SAVE_SETTINGS);
    } else if (action == "bookmark_tune") {
      int slot = getParamStr("slot").toInt();
      if (slot >= 0 && slot < getTotalMemories() && memories[slot].freq) {
        tuneToMemory(&memories[slot]);
        if (currentMode != USB) ft8SetDigitalMode(FT8_DIG_OFF);
        prefsRequestSave(SAVE_ALL);
      }
    } else if (action == "bookmark_save") {
      String name = getParamStr("name");
      name.trim();
      int slot = getParamStr("slot").toInt();
      if (slot < 0 || slot >= 24) {
        slot = -1;
        for (int i = 0; i < 24; i++) {
          if (!memories[i].freq) { slot = i; break; }
        }
        if (slot < 0) slot = 23;
      }
      memories[slot].freq = freqToHz(currentFrequency, currentMode) + currentBFO;
      memories[slot].band = bandIdx;
      memories[slot].mode = currentMode;
      if (name.length() == 0) {
        name = String(getCurrentBand()->bandName) + " " + String(currentFrequency);
      }
      strncpy(memories[slot].name, name.c_str(), 9);
      memories[slot].name[9] = '\0';
      prefsRequestSave(SAVE_MEMORIES, true);
    } else if (action == "bookmark_del") {
      int slot = getParamStr("slot").toInt();
      if (slot >= 0 && slot < getTotalMemories()) {
        memories[slot].freq = 0;
        memories[slot].name[0] = '\0';
        prefsRequestSave(SAVE_MEMORIES, true);
      }
    } else if (action == "view") {
      ft8SetViewMode((uint8_t)getParamStr("mode").toInt());
    } else if (action == "view_toggle") {
      ft8ToggleViewMode();
    } else if (action == "clear_traffic") {
      ft8ClearTraffic();
    } else if (action == "mute") {
      ft8ToggleSpeakerMute();
    } else if (action == "decode") {
      ft8RequestTestDecode();
    } else if (action == "sync") {
      ft8SyncSlotManual();
    } else if (action == "band") {
      int bId = getParamStr("id").toInt();
      if (bId >= 0 && bId < getTotalBands()) {
        if (bId >= 18 && bId <= 26 && ft8GetDigitalMode() != FT8_DIG_OFF) {
          tuneBandDigitalFreq(bId, ft8GetDigitalMode());
        } else {
          if (bId < 18 || bId > 26) ft8SetDigitalMode(FT8_DIG_OFF);
          selectBand((uint8_t)bId, false);
        }
        prefsRequestSave(SAVE_ALL);
      }
    }

    ft8RequestRedraw();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  // /api/screenshot.bmp - Stream 320x170 16-bit RGB565 BMP directly from sprite
  server.on("/api/screenshot.bmp", HTTP_GET, [] (AsyncWebServerRequest *request) {
    const uint32_t width = 320;
    const uint32_t height = 170;
    const uint32_t hdrSize = 14 + 40 + 12;
    const uint32_t imgBytes = width * height * 2;
    const uint32_t fileSize = hdrSize + imgBytes;

    AsyncWebServerResponse *response = request->beginResponse("image/bmp", fileSize,
      [width, height, hdrSize, imgBytes, fileSize](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        if (index >= fileSize) return 0;

        size_t written = 0;
        if (index == 0) {
          uint8_t hdr[66] = {0};
          hdr[0] = 'B'; hdr[1] = 'M';
          memcpy(&hdr[2], &fileSize, 4);
          memcpy(&hdr[10], &hdrSize, 4);
          uint32_t dibSize = 40;
          memcpy(&hdr[14], &dibSize, 4);
          memcpy(&hdr[18], &width, 4);
          memcpy(&hdr[22], &height, 4);
          uint16_t planes = 1, bpp = 16;
          memcpy(&hdr[26], &planes, 2);
          memcpy(&hdr[28], &bpp, 2);
          uint32_t comp = 3; // BI_BITFIELDS
          memcpy(&hdr[30], &comp, 4);
          memcpy(&hdr[34], &imgBytes, 4);
          uint32_t rMask = 0xF800, gMask = 0x07E0, bMask = 0x001F;
          memcpy(&hdr[54], &rMask, 4);
          memcpy(&hdr[58], &gMask, 4);
          memcpy(&hdr[62], &bMask, 4);

          memcpy(buffer, hdr, hdrSize);
          written += hdrSize;
          index += hdrSize;
        }

        const uint16_t *sprPtr = (const uint16_t *)spr.getPointer();
        while (written + width * 2 <= maxLen && index < fileSize) {
          uint32_t pixOffset = (index - hdrSize) / 2;
          int rowFromBottom = pixOffset / width;
          int y = (height - 1) - rowFromBottom;
          if (y < 0) break;
          uint16_t *dst = (uint16_t *)(buffer + written);
          if (sprPtr) {
            const uint16_t *srcRow = sprPtr + (y * width);
            for (uint32_t x = 0; x < width; x++) {
              uint16_t v = srcRow[x];
              dst[x] = (v >> 8) | (v << 8);
            }
          } else {
            for (uint32_t x = 0; x < width; x++) {
              dst[x] = spr.readPixel(x, y);
            }
          }
          written += width * 2;
          index += width * 2;
        }
        return written;
      }
    );
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  // /api/audio.wav - Live 6000Hz 16-bit PCM WAV snapshot from GPIO34 (zero static RAM)
  server.on("/api/audio.wav", HTTP_GET, [] (AsyncWebServerRequest *request) {
    const size_t totalWavBytes = 44 + 960 * 2;
    AsyncWebServerResponse *response = request->beginResponse("audio/wav", totalWavBytes,
      [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        return ft8FillWavChunk(buffer, maxLen, index);
      }
    );
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  // /api/wifi/list - Return all 8 saved WiFi slots + connected SSID
  server.on("/api/wifi/list", HTTP_GET, [] (AsyncWebServerRequest *request) {
    prefs.begin("network", true, STORAGE_PARTITION);
    String out = "{\"connected_ssid\":\"";
    if (WiFi.status() == WL_CONNECTED) out += WiFi.SSID();
    out += "\",\"networks\":[";
    for (int i = 0; i < MAX_WIFI_SLOTS; i++) {
      char kS[16], kP[16];
      snprintf(kS, sizeof(kS), "wifissid%d", i + 1);
      snprintf(kP, sizeof(kP), "wifipass%d", i + 1);
      String s = prefs.getString(kS, "");
      String p = prefs.getString(kP, "");
      s.replace("\"", "\\\"");
      p.replace("\"", "\\\"");
      if (i > 0) out += ",";
      out += "{\"ssid\":\"" + s + "\",\"pass\":\"" + p + "\"}";
    }
    out += "]}";
    prefs.end();
    request->send(200, "application/json", out);
  });

  // /api/wifi/scan - Scan nearby 2.4GHz WiFi networks and return JSON list
  server.on("/api/wifi/scan", HTTP_GET, [] (AsyncWebServerRequest *request) {
    performWifiScan();
    String out = "[";
    for (int i = 0; i < lastScannedCount; i++) {
      if (i > 0) out += ",";
      String s = lastScannedAPs[i].ssid;
      s.replace("\"", "\\\"");
      char item[128];
      snprintf(item, sizeof(item), "{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"enc\":%s}",
               s.c_str(), (int)lastScannedAPs[i].rssi, (int)lastScannedAPs[i].ch,
               lastScannedAPs[i].enc ? "true" : "false");
      out += item;
    }
    out += "]";
    request->send(200, "application/json", out);
  });

  // /api/wifi/save - Save up to 8 WiFi networks and optionally reconnect immediately
  server.on("/api/wifi/save", HTTP_POST, [] (AsyncWebServerRequest *request) {
    prefs.begin("network", false, STORAGE_PARTITION);
    bool haveSSID = false;
    for (int i = 0; i < MAX_WIFI_SLOTS; i++) {
      char pS[16], pP[16], kS[16], kP[16];
      snprintf(pS, sizeof(pS), "ssid%d", i + 1);
      snprintf(pP, sizeof(pP), "pass%d", i + 1);
      snprintf(kS, sizeof(kS), "wifissid%d", i + 1);
      snprintf(kP, sizeof(kP), "wifipass%d", i + 1);
      if (request->hasParam(pS, true)) {
        String s = request->getParam(pS, true)->value();
        String p = request->hasParam(pP, true) ? request->getParam(pP, true)->value() : "";
        prefs.putString(kS, s);
        prefs.putString(kP, p);
        if (s != "") haveSSID = true;
      }
    }
    prefs.end();

    bool reconnect = request->hasParam("reconnect", true) && request->getParam("reconnect", true)->value() == "1";
    if (reconnect && haveSSID) {
      if (wifiModeIdx <= NET_AP_ONLY) wifiModeIdx = NET_AP_CONNECT;
      netRequestConnect();
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.onNotFound([] (AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.printf("[Web] HTTP server started on port 80 (free_heap=%u, free_8bit=%u, max_alloc=%u)\r\n",
                (unsigned int)ESP.getFreeHeap(),
                (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned int)ESP.getMaxAllocHeap());
}

void webSetConfig(AsyncWebServerRequest *request)
{
  uint32_t prefsSave = 0;

  prefs.begin("network", false, STORAGE_PARTITION);

  if(request->hasParam("username", true) && request->hasParam("password", true))
  {
    loginUsername = request->getParam("username", true)->value();
    loginPassword = request->getParam("password", true)->value();

    prefs.putString("loginusername", loginUsername);
    prefs.putString("loginpassword", loginPassword);
  }

  bool haveSSID = false;
  for(int j=0 ; j<MAX_WIFI_SLOTS ; j++)
  {
    char nameSSID[16], namePASS[16];
    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    if(request->hasParam(nameSSID, true) && request->hasParam(namePASS, true))
    {
      String ssid = request->getParam(nameSSID, true)->value();
      String pass = request->getParam(namePASS, true)->value();
      prefs.putString(nameSSID, ssid);
      prefs.putString(namePASS, pass);
      haveSSID |= ssid != "";
    }
  }

  if(request->hasParam("utcoffset", true))
  {
    String utcOffset = request->getParam("utcoffset", true)->value();
    utcOffsetIdx = utcOffset.toInt();
    clockRefreshTime();
    prefsSave |= SAVE_SETTINGS;
  }

  if(request->hasParam("theme", true))
  {
    String theme = request->getParam("theme", true)->value();
    themeIdx = theme.toInt();
    prefsSave |= SAVE_SETTINGS;
  }

  scrollDirection = request->hasParam("scroll", true)? -1 : 1;
  zoomMenu        = request->hasParam("zoom", true);
  prefsSave |= SAVE_SETTINGS;

  prefs.end();

  prefsRequestSave(prefsSave, true);
  request->redirect("/config");

  if(haveSSID) {
    if (wifiModeIdx <= NET_AP_ONLY) wifiModeIdx = NET_AP_CONNECT;
    netRequestConnect();
  }
}

static const String webInputField(const String &name, const String &value, bool pass)
{
  String newValue(value);
  newValue.replace("\"", "&quot;");
  newValue.replace("'", "&apos;");

  return(
    "<INPUT TYPE='" + String(pass? "PASSWORD":"TEXT") + "' NAME='" +
    name + "' VALUE='" + newValue + "'>"
  );
}

static const String webStyleSheet()
{
  return
"BODY { margin: 0; padding: 0; }"
"H1 { text-align: center; }"
"TABLE { width: 100%; max-width: 768px; border: 0px; margin-left: auto; margin-right: auto; }"
"TH, TD { padding: 0.5em; }"
"TH.HEADING { background-color: #80A0FF; column-span: all; text-align: center; }"
"TD.LABEL { text-align: right; }"
"INPUT[type=text], INPUT[type=password], SELECT { width: 95%; padding: 0.5em; }"
"INPUT[type=submit] { width: 50%; padding: 0.5em 0; }"
".CENTER { text-align: center; }"
;
}

static const String webPage(const String &body)
{
  return
"<!DOCTYPE HTML>"
"<HTML>"
"<HEAD>"
  "<META CHARSET='UTF-8'>"
  "<META NAME='viewport' CONTENT='width=device-width, initial-scale=1.0'>"
  "<TITLE>ATS-Mini Config</TITLE>"
  "<STYLE>" + webStyleSheet() + "</STYLE>"
"</HEAD>"
"<BODY STYLE='font-family: sans-serif;'>" + body + "</BODY>"
"</HTML>"
;
}

static const String webUtcOffsetSelector()
{
  String result = "";
  for(int i=0 ; i<getTotalUTCOffsets(); i++)
  {
    char text[64];
    sprintf(text,
      "<OPTION VALUE='%d'%s>%s (%s)</OPTION>",
      i, utcOffsetIdx==i? " SELECTED":"",
      utcOffsets[i].city, utcOffsets[i].desc
    );
    result += text;
  }
  return(result);
}

static const String webThemeSelector()
{
  String result = "";
  for(int i=0 ; i<getTotalThemes(); i++)
  {
    char text[64];
    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
       i, themeIdx==i? " SELECTED":"", theme[i].name
    );
    result += text;
  }
  return(result);
}

static const String webRadioPage()
{
  String ip = "";
  String ssid = "";
  String freq = currentMode == FM?
    String(currentFrequency / 100.0) + "MHz "
  : String(currentFrequency + currentBFO / 1000.0) + "kHz ";

  if(WiFi.status()==WL_CONNECTED)
  {
    ip = WiFi.localIP().toString();
    ssid = WiFi.SSID();
  }
  else
  {
    ip = WiFi.softAPIP().toString();
    ssid = String(apSSID);
  }

  return webPage(
"<H1>ATS-Mini Pocket Receiver</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/'>FT8 WebSDR</A>&nbsp;|&nbsp;<A HREF='/memory'>Memory</A>&nbsp;|&nbsp;<A HREF='/config'>Config</A>"
"</P>"
"<TABLE COLUMNS=2>"
"<TR><TD CLASS='LABEL'>IP Address</TD><TD><A HREF='http://" + ip + "'>" + ip + "</A> (" + ssid + ")</TD></TR>"
"<TR><TD CLASS='LABEL'>MAC Address</TD><TD>" + String(getMACAddress()) + "</TD></TR>"
"<TR><TD CLASS='LABEL'>Firmware</TD><TD>" + String(getVersion(true)) + "</TD></TR>"
"<TR><TD CLASS='LABEL'>Band</TD><TD>" + String(getCurrentBand()->bandName) + "</TD></TR>"
"<TR><TD CLASS='LABEL'>Frequency</TD><TD>" + freq + String(bandModeDesc[currentMode]) + "</TD></TR>"
"<TR><TD CLASS='LABEL'>Signal Strength</TD><TD>" + String(rssi) + "dBuV</TD></TR>"
"<TR><TD CLASS='LABEL'>Signal to Noise</TD><TD>" + String(snr) + "dB</TD></TR>"
"<TR><TD CLASS='LABEL'>Battery Voltage</TD><TD>" + String(batteryMonitor()) + "V</TD></TR>"
"</TABLE>"
);
}

static const String webMemoryPage()
{
  String items = "";
  for(int j=0 ; j<MEMORY_COUNT ; j++)
  {
    char text[64];
    sprintf(text, "<TR><TD CLASS='LABEL' WIDTH='10%%'>%02d</TD><TD>", j+1);
    items += text;

    if(!memories[j].freq)
      items += "&nbsp;---&nbsp;</TD></TR>";
    else
    {
      String freq = memories[j].mode == FM?
        String(memories[j].freq / 1000000.0) + "MHz "
      : String(memories[j].freq / 1000.0) + "kHz ";
      items += freq + bandModeDesc[memories[j].mode] + "</TD></TR>";
    }
  }

  return webPage(
"<H1>ATS-Mini Pocket Receiver Memory</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/'>FT8 WebSDR</A>&nbsp;|&nbsp;<A HREF='/config'>Config</A>"
"</P>"
"<TABLE COLUMNS=2>" + items + "</TABLE>"
);
}

const String webConfigPage()
{
  prefs.begin("network", true, STORAGE_PARTITION);
  String wifiRows = "";
  for (int i = 0; i < MAX_WIFI_SLOTS; i++) {
    char kS[16], kP[16];
    snprintf(kS, sizeof(kS), "wifissid%d", i + 1);
    snprintf(kP, sizeof(kP), "wifipass%d", i + 1);
    String s = prefs.getString(kS, "");
    String p = prefs.getString(kP, "");
    wifiRows += "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network " + String(i + 1) + "</TH></TR>";
    wifiRows += "<TR><TD CLASS='LABEL'>SSID</TD><TD>" + webInputField(kS, s) + "</TD></TR>";
    wifiRows += "<TR><TD CLASS='LABEL'>Password</TD><TD>" + webInputField(kP, p, false) + "</TD></TR>";
  }
  prefs.end();

  return webPage(
"<H1>ATS-Mini Multi-WiFi & System Config</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/'>FT8 WebSDR</A>"
  "&nbsp;|&nbsp;<A HREF='/memory'>Memory</A>"
"</P>"
"<FORM ACTION='/setconfig' METHOD='POST'>"
  "<TABLE COLUMNS=2>" +
  wifiRows +
  "<TR><TH COLSPAN=2 CLASS='HEADING'>This Web UI Login Credentials</TH></TR>"
  "<TR><TD CLASS='LABEL'>Username</TD><TD>" + webInputField("username", loginUsername) + "</TD></TR>"
  "<TR><TD CLASS='LABEL'>Password</TD><TD>" + webInputField("password", loginPassword, true) + "</TD></TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Settings</TH></TR>"
  "<TR><TD CLASS='LABEL'>Time Zone</TD><TD><SELECT NAME='utcoffset'>" + webUtcOffsetSelector() + "</SELECT></TD></TR>"
  "<TR><TD CLASS='LABEL'>Theme</TD><TD><SELECT NAME='theme'>" + webThemeSelector() + "</SELECT></TD></TR>"
  "<TR><TD CLASS='LABEL'>Reverse Scrolling</TD><TD><INPUT TYPE='CHECKBOX' NAME='scroll' VALUE='on'" +
    (scrollDirection<0? " CHECKED ":"") + "></TD></TR>"
  "<TR><TD CLASS='LABEL'>Zoomed Menu</TD><TD><INPUT TYPE='CHECKBOX' NAME='zoom' VALUE='on'" +
    (zoomMenu? " CHECKED ":"") + "></TD></TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'><INPUT TYPE='SUBMIT' VALUE='Save & Reconnect'></TH></TR>"
  "</TABLE>"
"</FORM>"
);
}
