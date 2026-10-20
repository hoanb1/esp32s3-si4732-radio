#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "Layout-FT8.h"
#include "Ft8Tx.h"
#include "WebUI.h"
#include "WorldMap.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ping/ping_sock.h>
#include <NTPClient.h>
#include <ESPmDNS.h>
#include <esp_sntp.h>
#include <sys/time.h>

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

//
// /ws: one WebSocket per open page carries everything the page watches, so it does not
// open a new HTTP connection several times a second (that ran this board out of heap):
//   text  '{...}'   status JSON (same as /api/status), every 500 ms
//   text  other     a radio log line (WebLog.h)
//   binary 'W' n    the n newest waterfall rows (120 bytes each, newest first)
//   binary 'H' bins(u16 LE) n   n full-resolution rows, oldest first, each: block index in the FT8 slot
//                   (255 = not slot aligned), slot of the hour (0..239), bins/2 bytes (4 bits per 6.25 Hz bin)
//   binary 'A' seq  web audio: uint32 LE start sample number + 6 kHz mu-law samples,
//                   only to pages that asked with "audio=1"
//
#define WS_MAX_PAGES 3
static AsyncWebSocket ws("/ws");
struct WsPage { uint32_t id; bool audio; uint32_t logSeq; uint32_t wfSeq; };
static WsPage wsPages[WS_MAX_PAGES];
static portMUX_TYPE wsMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t wsAudSeq = 0;
static void wsTick();

// Frames are allocated on the heap; a failed allocation aborts (no exceptions), so only
// build one when it leaves this much byte-addressable heap for WiFi/lwIP. Skipped frames come later.
// (ESP.getFreeHeap() also counts ~30 KB of 32-bit-only IRAM that new/malloc cannot use.)
#define WS_HEAP_RESERVE 9000
#define WS_AUDIO_MAX    2400   // 400 ms per frame at most; a page further behind skips ahead
#define WS_WF_ROWS_MAX  12     // waterfall rows per frame (1.4 KB); a new page catches up over a few ticks
static bool wsHeapOk(size_t bytes)
{
  return heap_caps_get_free_size(MALLOC_CAP_8BIT) > WS_HEAP_RESERVE + bytes &&
         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) > bytes + 1024;
}

// NTP Client to get UTC time
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org");

static bool wifiInitAP();
static bool wifiConnect();
static void webInit();
static void processControlQueue();
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

//
// Gateway ping watchdog. With a weak signal the STA sometimes stops receiving while the
// driver still reports WL_CONNECTED (no ping, no HTTP, RSSI still updates), and nothing
// recovers it. Ping the gateway every 20 s; after 65 s without a reply, drop the
// association so the reconnect logic below starts over.
//
#define GW_PING_INTERVAL_MS 20000
#define GW_DEAD_MS          65000
static esp_ping_handle_t gwPing = NULL;
static uint32_t gwPingTarget = 0;
static volatile uint32_t gwLastReplyMs = 0;

static void gwPingOk(esp_ping_handle_t hdl, void *args) { gwLastReplyMs = millis(); }

static void gatewayWatchdog()
{
  if (WiFi.status() != WL_CONNECTED || !(WiFi.getMode() & WIFI_STA)) return;
  uint32_t gw = (uint32_t)WiFi.gatewayIP();
  if (!gw) return;

  if (!gwPing || gw != gwPingTarget) {
    if (gwPing) { esp_ping_stop(gwPing); esp_ping_delete_session(gwPing); gwPing = NULL; }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = GW_PING_INTERVAL_MS;
    cfg.timeout_ms = 2000;
    cfg.data_size = 8;
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = gw;
    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = gwPingOk;
    if (esp_ping_new_session(&cfg, &cbs, &gwPing) != ESP_OK) { gwPing = NULL; return; }
    gwPingTarget = gw;
    gwLastReplyMs = millis();   // grace period for the first reply
    esp_ping_start(gwPing);
    return;
  }

  if (millis() - gwLastReplyMs > GW_DEAD_MS) {
    Serial.printf("[WiFi] Gateway %s silent for %lu s while 'connected': reconnecting\r\n",
                  WiFi.gatewayIP().toString().c_str(), (unsigned long)((millis() - gwLastReplyMs) / 1000));
    gwLastReplyMs = millis();
    WiFi.disconnect(false, false);   // keep the config; the watchdog below reconnects
  }
}

void netTickTime()
{
  wsTick();
  processControlQueue();

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
  gatewayWatchdog();
  static uint32_t lastWifiCheckMs = 0;
  static uint8_t disconnectTicks = 0;
  if (millis() - lastWifiCheckMs >= 6000) {
    lastWifiCheckMs = millis();
    static uint8_t netLogTicks = 0;
    if (++netLogTicks >= 10) {   // once a minute, for the web log card
      netLogTicks = 0;
      Serial.printf("[NET] RSSI %d dBm, 8-bit heap %u (min %u, largest %u), web clients %u\r\n", (int)WiFi.RSSI(),
                    heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ws.count());
    }
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

  if(netMode != NET_SYNC)
  {
    webInit();
  }

  // Initialize WiFi and try connecting to saved networks (HH-1301, HH-1301_IoT, etc.)
  if(netMode > NET_AP_ONLY && wifiConnect())
  {
    if(netMode != NET_SYNC && showStatus) delay(200);
  }

  if(netMode == NET_SYNC)
  {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
  }
}

static volatile bool sntpSynced = false;

static void sntpSyncCallback(struct timeval *tv)
{
  sntpSynced = true;
}

// Start background SNTP (lwIP task, non-blocking); keeps the system clock UTC-accurate to ~10 ms
static void sntpStart()
{
  static bool started = false;
  if(started) return;
  started = true;
  sntp_set_time_sync_notification_cb(sntpSyncCallback);
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
}

bool ntpIsAvailable()
{
  return(sntpSynced);
}

bool ntpSyncTime()
{
  // Mirror the SNTP system clock into the display clock (HH:MM); FT8 timing reads the system clock directly
  if(!sntpSynced) return(false);
  struct timeval tv;
  gettimeofday(&tv, NULL);
  time_t t = tv.tv_sec;
  return(clockSet((t % 86400) / 3600, (t % 3600) / 60, t % 60));
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
    ft8AddLog("[INFO] Connecting to primary Wi-Fi '%s'...", savedSsid[0].c_str());
    drawScreen(("Connecting: " + savedSsid[0]).c_str(), "Press encoder to skip");
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
    drawScreen("Scanning 2.4GHz Wi-Fi...", "Looking for saved networks...");
    performWifiScan();

    for (int s = 0; s < lastScannedCount && WiFi.status() != WL_CONNECTED; s++) {
      for (int i = 0; i < MAX_WIFI_SLOTS; i++) {
        if (savedSsid[i] != "" && savedSsid[i] == lastScannedAPs[s].ssid) {
          Serial.printf("[WiFi] Trying slot #%d: SSID='%s' (%d dBm)...\r\n",
                        i + 1, savedSsid[i].c_str(), (int)lastScannedAPs[s].rssi);
          ft8AddLog("[INFO] Connecting to Wi-Fi '%s'...", savedSsid[i].c_str());
          drawScreen(("Connecting: " + savedSsid[i]).c_str(), "Press encoder to skip");

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
    ft8AddLog("[WARN] Wi-Fi not connected, running AP+STA: %s (%s)",
              apSSID, WiFi.softAPIP().toString().c_str());
    drawScreen(("AP: " + String(apSSID)).c_str(), ("Web UI: " + WiFi.softAPIP().toString()).c_str());
    return(false);
  }
  else
  {
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    sntpStart();
    Serial.printf("[WiFi] CONNECTED to '%s' | IP: %s | RSSI: %d dBm\r\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    ft8AddLog("[INFO] Connected to Wi-Fi '%s' - IP: %s (%d dBm)",
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
// Web control actions are queued by the AsyncTCP task and executed from loop(),
// so that only one task ever talks to the SI4732 over I2C
//
struct ControlRequest {
  uint8_t count;
  String keys[6];
  String values[6];
};

#define CONTROL_QUEUE_SIZE 6
static ControlRequest controlQueue[CONTROL_QUEUE_SIZE];
static uint8_t controlHead = 0;
static uint8_t controlTail = 0;
static SemaphoreHandle_t controlMutex = NULL;

static bool queueControlRequest(AsyncWebServerRequest *request)
{
  if (!controlMutex) return false;
  xSemaphoreTake(controlMutex, portMAX_DELAY);
  uint8_t next = (controlHead + 1) % CONTROL_QUEUE_SIZE;
  bool ok = next != controlTail;
  if (ok) {
    ControlRequest &cr = controlQueue[controlHead];
    cr.count = 0;
    for (size_t i = 0; i < request->params() && cr.count < 6; i++) {
      const AsyncWebParameter *p = request->getParam(i);
      cr.keys[cr.count] = p->name();
      cr.values[cr.count] = p->value();
      cr.count++;
    }
    controlHead = next;
  }
  xSemaphoreGive(controlMutex);
  return ok;
}

static void runControlAction(const ControlRequest &cr)
{
  auto getParamStr = [&](const char *name) -> String {
    for (uint8_t i = 0; i < cr.count; i++)
      if (cr.keys[i] == name) return cr.values[i];
    return "";
  };
  String action = getParamStr("action");

    if (action == "volume") {
      int v = getParamStr("val").toInt();
      if (v < 0) v = 0;
      if (v > 63) v = 63;
      volume = (uint8_t)v;
      ft8ApplyAudio();
      prefsRequestSave(SAVE_SETTINGS);
    } else if (action == "mode") {
      int m = getParamStr("mode").toInt();
      if (m >= 0 && m <= 4) {
        ft8SetDigitalMode(FT8_DIG_OFF);
        if (uiLayoutIdx == UI_FT8) uiLayoutIdx = UI_DEFAULT;
        if (m == 4) {
          // CW mode = USB + 0.5kHz narrow filter (ssbBandwidths[0] = "0.5k")
          if (bandIdx == 0) bandIdx = 20; // Default 40M
          bands[bandIdx].bandMode = USB;
          bands[bandIdx].bandwidthIdx = 0;
          currentMode = USB;
          selectBand(bandIdx, false);
        } else {
          if (m == FM && bandIdx != 0) {
            bandIdx = 0;
          } else if (m != FM && bandIdx == 0) {
            bandIdx = (m == AM) ? 16 : 22; // Default MW2 for AM or 20M for USB/LSB
          }
          if ((m == USB || m == LSB) && bands[bandIdx].bandwidthIdx == 0) {
            bands[bandIdx].bandwidthIdx = 4; // Restore 3.0kHz SSB filter when leaving CW
          }
          bands[bandIdx].bandMode = (uint8_t)m;
          currentMode = (uint8_t)m;
          selectBand(bandIdx, false);
        }
        prefsRequestSave(SAVE_ALL);
      }
    } else if (action == "dig_mode") {
      int dm = getParamStr("val").toInt();
      if (dm >= 0 && dm <= 2) {
        if (dm == FT8_DIG_FT8 || dm == FT8_DIG_FT4) {
          if (bandIdx < 18 || bandIdx > 26) {
            bandIdx = 22; // Default 20M amateur band
          }
          ft8TuneBandDigitalFreq(bandIdx, (uint8_t)dm);
          prefsRequestSave(SAVE_ALL);
        } else {
          ft8SetDigitalMode(FT8_DIG_OFF);
          if (uiLayoutIdx == UI_FT8) uiLayoutIdx = UI_DEFAULT;
          prefsRequestSave(SAVE_SETTINGS);
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
          if (uiLayoutIdx == UI_FT8) uiLayoutIdx = UI_DEFAULT;
          if (bandIdx != 0 || currentMode != FM) {
            bandIdx = 0;
            bands[0].bandMode = FM;
            bands[0].currentFreq = fmFreq;
            selectBand(0, false);
          }
          updateFrequency(fmFreq, false);
          clearStationInfo();
          prefsRequestSave(SAVE_ALL);
        } else if (khzVal >= 150.0 && khzVal <= 30000.0) {
          uint32_t hz = (uint32_t)round(khzVal * 1000.0);
          uint16_t khz = (uint16_t)(hz / 1000UL);
          int16_t remBfo = (int16_t)(hz % 1000UL);
          uint8_t keepMode = (currentMode == FM) ? AM : currentMode;
          static const uint16_t digKhz[17] = {
            1840, 3573, 7074, 10136, 14074, 18100, 21074, 24915, 28074,
            3575, 7047, 10140, 14080, 18104, 21140, 24919, 28180
          };
          bool isDigFreq = false;
          for (int i = 0; i < 17; i++) {
            if (khz == digKhz[i]) {
              keepMode = USB;
              isDigFreq = true;
              break;
            }
          }

          if (bandIdx == 0 || !isFreqInBand(getCurrentBand(), khz) || (isDigFreq && currentMode != USB)) {
            int matchedBand = 1; // Fallback to ALL band (150..30000 kHz)
            for (int i = getTotalBands() - 1; i >= 2; i--) {
              if (bands[i].bandMode != FM && isFreqInBand(&bands[i], khz)) {
                matchedBand = i;
                break;
              }
            }
            bandIdx = matchedBand;
            bands[bandIdx].currentFreq = khz;
            if (currentMode != FM || isDigFreq) {
              bands[bandIdx].bandMode = keepMode;
            }
            selectBand(bandIdx, false);
          }
          updateFrequency(khz, false);
          if (isSSB()) updateBFO(remBfo, false);
          clearStationInfo();
          identifyFrequency(currentFrequency + currentBFO / 1000);
          ft8CheckFrequencyDigitalMode(currentFrequency, currentMode);
          prefsRequestSave(SAVE_ALL);
        }
      }
    } else if (action == "tune") {
      int delta = getParamStr("delta").toInt();   // kHz
      if (currentMode == FM) {
        // FM is tuned in 10 kHz units; steps below 10 kHz move one unit. Clamp instead of wrapping.
        int units = delta / 10;
        if (!units && delta) units = delta > 0 ? 1 : -1;
        int f = (int)currentFrequency + units;
        const Band *band = getCurrentBand();
        if (f < band->minimumFreq) f = band->minimumFreq;
        if (f > band->maximumFreq) f = band->maximumFreq;
        updateFrequency(f, false);
        clearStationInfo();
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
        clearStationInfo();
        identifyFrequency(currentFrequency + currentBFO / 1000);
        // FT8/FT4 only on their dial frequencies: leaving one turns the decoder off, landing on one turns it on
        if (isSSB()) ft8CheckFrequencyDigitalMode(currentFrequency, currentMode);
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
      Serial.printf("[Web] Saved band calibration: %d Hz\r\n", (int)newCal);
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
    } else if (action == "softmute") {
      int v = getParamStr("val").toInt();
      if (v < 0) v = 0;
      if (v > 32) v = 32;
      if (isSSB()) SsbSoftMuteIdx = v;
      else AmSoftMuteIdx = v;
      doSoftMute(0);
      prefsRequestSave(SAVE_SETTINGS);
    } else if (action == "squelch") {
      int v = getParamStr("val").toInt();
      if (v < 0) v = 0;
      if (v > 127) v = 127;
      currentSquelch = (uint8_t)v;
      prefsRequestSave(SAVE_SETTINGS);
    } else if (action == "brt") {
      int v = getParamStr("val").toInt();
      if (v < 10) v = 10;
      if (v > 255) v = 255;
      currentBrt = (uint16_t)v;
      ledcWrite(PIN_LCD_BL, currentBrt);
      prefsRequestSave(SAVE_SETTINGS);
    } else if (action == "theme") {
      int v = getParamStr("val").toInt();
      if (v >= 0 && v < getTotalThemes()) {
        themeIdx = (uint8_t)v;
        prefsRequestSave(SAVE_SETTINGS);
      } else {
        themeIdx = (themeIdx + 1) % getTotalThemes();
        prefsRequestSave(SAVE_SETTINGS);
      }
    } else if (action == "seek") {
      int dir = getParamStr("dir").toInt();
      webBtnEvent = (dir < 0) ? 5 : 4;
    } else if (action == "enc") {
      String type = getParamStr("type");
      if (type == "rot") {
        int d = getParamStr("dir").toInt();
        webEncDelta = (int16_t)(d ? d : 1);
      } else if (type == "click") {
        webBtnEvent = 1;
      } else if (type == "short") {
        webBtnEvent = 2;
      } else if (type == "long") {
        webBtnEvent = 3;
      }
    } else if (action == "bookmark_tune") {
      int slot = getParamStr("slot").toInt();
      if (slot >= 0 && slot < getTotalMemories() && memories[slot].freq) {
        tuneToMemory(&memories[slot]);
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
    } else if (action == "lcd_layout") {
      int lay = getParamStr("val").toInt();
      if (lay >= 0 && lay <= UI_FT8) {
        uiLayoutIdx = (uint8_t)lay;
        prefsRequestSave(SAVE_SETTINGS);
      }
    } else if (action == "view") {
      uiLayoutIdx = UI_FT8;
      ft8SetViewMode((uint8_t)getParamStr("mode").toInt());
      prefsRequestSave(SAVE_SETTINGS);
    } else if (action == "view_toggle") {
      uiLayoutIdx = (uiLayoutIdx + 1) % (UI_FT8 + 1);
      prefsRequestSave(SAVE_SETTINGS);
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
          ft8TuneBandDigitalFreq(bId, ft8GetDigitalMode());
        } else {
          selectBand((uint8_t)bId, false);
          ft8CheckFrequencyDigitalMode(currentFrequency, currentMode);
        }
        prefsRequestSave(SAVE_ALL);
      }
    }
}

// Execute queued web control actions (called from loop())
static void processControlQueue()
{
  while (controlMutex) {
    ControlRequest cr;
    xSemaphoreTake(controlMutex, portMAX_DELAY);
    bool have = controlTail != controlHead;
    if (have) {
      cr = controlQueue[controlTail];
      controlTail = (controlTail + 1) % CONTROL_QUEUE_SIZE;
    }
    xSemaphoreGive(controlMutex);
    if (!have) break;
    runControlAction(cr);
    ft8RequestRedraw();
  }
}

//
// Response for a fixed in-memory buffer that only advances by the bytes the
// TCP stack accepted. ESPAsyncWebServer's own byte-buffer response ignores a
// failed tcp_write (ERR_MEM under low heap) and silently drops a whole 2920
// byte chunk, which corrupts the gzipped page and larger API payloads.
// A stalled send is retried from the next ACK or the ~500 ms client poll.
//
#define FLASH_RESP_MAX_IN_FLIGHT 2920

class FlashResponse : public AsyncWebServerResponse
{
  public:
    FlashResponse(int code, const char *type, const uint8_t *data, size_t len) : _data(data)
    {
      _code = code;
      _contentType = type;
      _contentLength = len;
      addHeader("Connection", "close", false);
    }

    bool _sourceValid() const override { return true; }

    void _respond(AsyncWebServerRequest *request) override
    {
      _assembleHead(_head, request->version());
      _state = RESPONSE_HEADERS;
      _ack(request, 0, 0);
    }

    size_t _ack(AsyncWebServerRequest *request, size_t len, uint32_t time) override
    {
      (void)time;
      _ackedLength += len;
      AsyncClient *client = request->client();
      size_t queued = 0;

      while (_state == RESPONSE_HEADERS || _state == RESPONSE_CONTENT) {
        const char *src;
        size_t left;
        if (_state == RESPONSE_HEADERS) {
          src = _head.c_str() + _headSent;
          left = _head.length() - _headSent;
        } else {
          src = (const char *)_data + _sentLength;
          left = _contentLength - _sentLength;
        }
        if (!left) {
          if (_state == RESPONSE_HEADERS) { _head = String(); _state = RESPONSE_CONTENT; }
          else _state = RESPONSE_WAIT_ACK;
          continue;
        }
        // Keep at most 2 segments in flight: lwIP copies queued data into heap
        // pbufs and this board has no PSRAM, so a full send window starves Wi-Fi
        size_t inFlight = _writtenLength - _ackedLength;
        if (inFlight >= FLASH_RESP_MAX_IN_FLIGHT) break;
        if (left > FLASH_RESP_MAX_IN_FLIGHT - inFlight) left = FLASH_RESP_MAX_IN_FLIGHT - inFlight;
        size_t n = client->add(src, left);
        if (!n) break;
        if (_state == RESPONSE_HEADERS) _headSent += n; else _sentLength += n;
        _writtenLength += n;
        queued += n;
      }
      if (queued) client->send();

      if (_state == RESPONSE_WAIT_ACK && _ackedLength >= _writtenLength) _state = RESPONSE_END;
      return queued;
    }

  private:
    const uint8_t *_data;
    String _head;
    size_t _headSent = 0;
};


//
// Radio status JSON for /api/status and the /ws push (callers own the buffer: two tasks build it)
//
static int buildStatusJson(char *json, size_t cap)
{
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
  int8_t curSm = isSSB() ? SsbSoftMuteIdx : AmSoftMuteIdx;
  bool isStereo = (currentMode == FM) && rx.getCurrentPilot();
  const char *tStr = clockGet();
  if (!tStr) tStr = "--:--";

  auto cleanJsonStr = [](char *dst, const char *src, size_t maxLen) {
    size_t j = 0;
    if (src) {
      for (size_t i = 0; src[i] != '\0' && j + 1 < maxLen; i++) {
        char c = src[i];
        if (c >= 32 && c != '"' && c != '\\') dst[j++] = c;
      }
    }
    dst[j] = '\0';
  };

  char stName[40], rdsTxt[64], cleanSsid[40];
  cleanJsonStr(stName, getStationName(), sizeof(stName));
  cleanJsonStr(rdsTxt, getRadioText(), sizeof(rdsTxt));
  cleanJsonStr(cleanSsid, ssid.c_str(), sizeof(cleanSsid));

  uint32_t adcSlot, adcSlotRaw;
  uint16_t adcSlotTail;
  ft8GetAdcClock(&adcSlot, &adcSlotRaw, &adcSlotTail);

  int len = snprintf(json, cap,
    "{\"freq_khz\":\"%s\",\"raw_freq\":%u,\"unit\":\"%s\",\"band\":\"%s\",\"band_id\":%d,"
    "\"band_min\":%u,\"band_max\":%u,"
    "\"mode\":\"%s\",\"mode_id\":%u,\"dig_mode\":%u,\"bfo\":%d,\"cal\":%d,"
    "\"bw\":\"%s\",\"bw_idx\":%d,\"step\":\"%s\","
    "\"agc\":%d,\"att_db\":%d,\"avc\":%d,\"softmute\":%d,\"squelch\":%u,"
    "\"rssi\":%u,\"snr\":%u,\"s_level\":%d,\"stereo\":%s,"
    "\"station\":\"%s\",\"rds_text\":\"%s\",\"local_time\":\"%s\",\"vbat\":%.2f,"
    "\"brt\":%u,\"theme\":%u,\"theme_name\":\"%s\",\"cmd\":%u,"
    "\"lcd_layout\":%u,\"lcd_view\":%u,\"is_ap\":%s,\"ip\":\"%s\",\"ssid\":\"%s\","
    "\"synced\":%s,\"sync_src\":\"%s\",\"slot\":%u,\"decoding\":%s,"
    "\"last_duration_ms\":%lu,\"volume\":%u,\"mute\":%s,\"web_audio\":%s,"
    "\"audio_level\":%u,\"heap_free\":%u,\"msg_count\":%lu,"
    "\"adc_slot\":%lu,\"adc_slot_raw\":%lu,\"adc_slot_tail\":%u}",
    freqStr, (unsigned int)currentFrequency, unit, getCurrentBand()->bandName, bandIdx,
    (unsigned int)getCurrentBand()->minimumFreq, (unsigned int)getCurrentBand()->maximumFreq,
    ft8GetDisplayModeDesc(), (unsigned int)currentMode, (unsigned int)ft8GetDigitalMode(), (int)currentBFO, (int)cal,
    getCurrentBandwidth()->desc, (int)bands[bandIdx].bandwidthIdx, getCurrentStep()->desc,
    (int)agcIdx, (int)agcNdx, (int)curAvc, (int)curSm, (unsigned int)currentSquelch,
    rssi, snr, getStrength(rssi), isStereo ? "true" : "false",
    stName, rdsTxt, tStr, batteryMonitor(),
    (unsigned int)currentBrt, (unsigned int)themeIdx, TH.name, (unsigned int)currentCmd,
    (unsigned int)uiLayoutIdx, ft8GetViewMode(), isAp ? "true" : "false", ip.c_str(), cleanSsid,
    clockAvailable() ? "true" : "false", ntpIsAvailable() ? "NTP" : "Manual",
    ft8GetCurrentSlotSecond(), ft8IsDecodingNow() ? "true" : "false",
    (unsigned long)ft8GetLastDurationMs(), volume,
    ft8IsSpeakerMuted() ? "true" : "false",
    ft8WebAudioActive() ? "true" : "false",
    ft8GetAudioLevel(), (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
    (unsigned long)ft8GetTotalDecoded(),
    (unsigned long)adcSlot, (unsigned long)adcSlotRaw, (unsigned int)adcSlotTail
  );
  if (len < 0) len = 0;
  if (len >= (int)cap) len = cap - 1;
  return len;
}

//
// /ws WebSocket: page bookkeeping (AsyncTCP task) and sending (loop task)
//
static void wsOnEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
                      void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT) {
    bool added = false;
    portENTER_CRITICAL(&wsMux);
    for (int i = 0; i < WS_MAX_PAGES && !added; i++) {
      if (!wsPages[i].id) {
        uint32_t head = webLogHead();
        uint32_t wf = ft8GetWebWaterfallRowCount();
        // Replay the recent log lines and the whole waterfall
        wsPages[i] = { client->id(), false, head > WEBLOG_LINES ? head - WEBLOG_LINES : 0, wf > 36 ? wf - 36 : 0 };
        added = true;
      }
    }
    portEXIT_CRITICAL(&wsMux);
    if (!added) client->close(1013, "too many pages");
  } else if (type == WS_EVT_DISCONNECT) {
    portENTER_CRITICAL(&wsMux);
    for (int i = 0; i < WS_MAX_PAGES; i++)
      if (wsPages[i].id == client->id()) wsPages[i].id = 0;
    portEXIT_CRITICAL(&wsMux);
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT && len >= 7 &&
        !memcmp(data, "audio=", 6)) {
      bool on = data[6] == '1';
      portENTER_CRITICAL(&wsMux);
      for (int i = 0; i < WS_MAX_PAGES; i++)
        if (wsPages[i].id == client->id()) wsPages[i].audio = on;
      portEXIT_CRITICAL(&wsMux);
    }
  }
}

static void wsTick()
{
  static uint32_t lastAudioMs = 0, lastLogMs = 0, lastCleanMs = 0, lastStatusMs = 0;
  uint32_t now = millis();
  if (!ws.count()) return;

  if (now - lastCleanMs >= 1000) {
    lastCleanMs = now;
    ws.cleanupClients(WS_MAX_PAGES);
  }

  WsPage pages[WS_MAX_PAGES];
  portENTER_CRITICAL(&wsMux);
  memcpy(pages, wsPages, sizeof(pages));
  portEXIT_CRITICAL(&wsMux);

  // Audio: every 200 ms, the samples since the last send to every listening page.
  // A page whose send queue is backed up skips a chunk (it sees the gap and rebuffers).
  bool anyAudio = false;
  for (int i = 0; i < WS_MAX_PAGES; i++) anyAudio |= pages[i].id && pages[i].audio;
  if (anyAudio && now - lastAudioMs >= 200) {
    lastAudioMs = now;
    uint32_t count = 0;
    uint32_t start = ft8WebAudioBegin(wsAudSeq, &count);   // also keeps web audio switched on
    if (count > WS_AUDIO_MAX) {
      start += count - WS_AUDIO_MAX;
      count = WS_AUDIO_MAX;
    }
    if (count && wsHeapOk(5 + count)) {
      auto buf = std::make_shared<std::vector<uint8_t>>(5 + count);
      uint8_t *b = buf->data();
      b[0] = 'A'; b[1] = start; b[2] = start >> 8; b[3] = start >> 16; b[4] = start >> 24;
      ft8WebAudioCopy(b + 5, start, count);
      wsAudSeq = start + count;
      for (int i = 0; i < WS_MAX_PAGES; i++) {
        if (!pages[i].id || !pages[i].audio) continue;
        AsyncWebSocketClient *c = ws.client(pages[i].id);
        if (c && c->canSend() && c->queueLen() < 4) c->binary(buf);
      }
    }
  }

  // Status JSON and new waterfall rows, every 500 ms
  if (now - lastStatusMs >= 500) {
    lastStatusMs = now;
    static char json[960];
    int len = buildStatusJson(json, sizeof(json));
    if (!wsHeapOk(len)) return;
    auto status = std::make_shared<std::vector<uint8_t>>((uint8_t *)json, (uint8_t *)json + len);
    uint32_t wfHead = ft8GetWebWaterfallRowCount();
    const uint8_t *wf = ft8GetWebWaterfallBuffer();
    for (int i = 0; i < WS_MAX_PAGES; i++) {
      if (!pages[i].id) continue;
      AsyncWebSocketClient *c = ws.client(pages[i].id);
      if (!c || !c->canSend() || c->queueLen() >= 4) continue;   // busy page: it gets the next one
      c->text(status);
      uint32_t behind = wfHead - pages[i].wfSeq;
      if (behind > 36) behind = 36;
      uint32_t rows = behind > WS_WF_ROWS_MAX ? WS_WF_ROWS_MAX : behind;
      if (rows && !wsHeapOk(2 + rows * 120)) rows = 0;   // the page catches up on a later tick
      if (rows) {
        auto buf = std::make_shared<std::vector<uint8_t>>(2 + rows * 120);
        (*buf)[0] = 'W';
        (*buf)[1] = (uint8_t)rows;
        memcpy(buf->data() + 2, wf + (behind - rows) * 120, rows * 120);   // oldest pending rows first
        c->binary(buf);
      }
      // Same rows at full resolution; the radio keeps only the last FT8_HIRES_ROWS, so a page that is
      // still catching up gets them from the next tick on
      uint16_t hb = ft8GetHiResBins();
      size_t rowBytes = 2 + (hb + 1) / 2;
      if (hb && rows && behind <= FT8_HIRES_ROWS && wsHeapOk(4 + rows * rowBytes)) {
        auto hbuf = std::make_shared<std::vector<uint8_t>>(4 + rows * rowBytes);
        uint8_t *h = hbuf->data();
        h[0] = 'H'; h[1] = hb & 0xFF; h[2] = hb >> 8; h[3] = (uint8_t)rows;
        uint32_t first = wfHead - behind;   // oldest pending row, same rows as the 'W' frame
        for (uint32_t r = 0; r < rows; r++) {
          uint8_t *dst = h + 4 + r * rowBytes;
          memcpy(dst, ft8GetHiResMeta(first + r), 2);
          memcpy(dst + 2, ft8GetHiResRow(first + r), rowBytes - 2);
        }
        c->binary(hbuf);
      }
      if (rows) {
        portENTER_CRITICAL(&wsMux);
        if (wsPages[i].id == pages[i].id) wsPages[i].wfSeq = wfHead - (behind - rows);
        portEXIT_CRITICAL(&wsMux);
      }
    }
  }

  // Logs: a few new lines per page every 100 ms
  if (now - lastLogMs >= 100) {
    lastLogMs = now;
    uint32_t head = webLogHead();
    char line[WEBLOG_LINE_LEN];
    for (int i = 0; i < WS_MAX_PAGES; i++) {
      if (!pages[i].id || pages[i].logSeq == head) continue;
      AsyncWebSocketClient *c = ws.client(pages[i].id);
      if (!c) continue;
      uint32_t seq = pages[i].logSeq;
      if (head - seq > WEBLOG_LINES) seq = head - WEBLOG_LINES;   // fell behind: skip the lost lines
      for (int n = 0; n < 4 && seq != head && c->canSend() && c->queueLen() < 6; n++, seq++)
        if (wsHeapOk(WEBLOG_LINE_LEN) && webLogGet(seq, line, sizeof(line))) c->text(line);
      portENTER_CRITICAL(&wsMux);
      if (wsPages[i].id == pages[i].id) wsPages[i].logSeq = seq;
      portEXIT_CRITICAL(&wsMux);
    }
  }
}

//
// Initialize internal web server with full FT8 + Multi-WiFi APIs
//
static void webInit()
{
  if (webServerStarted) return;
  webServerStarted = true;
  if (!controlMutex) controlMutex = xSemaphoreCreateMutex();

  // Main Web UI page - pre-gzipped in flash (zero heap copy), revalidated by ETag
  server.on("/", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if (request->hasHeader("If-None-Match") && request->header("If-None-Match") == WEBUI_ETAG) {
      AsyncWebServerResponse *response = request->beginResponse(304);
      response->addHeader("ETag", WEBUI_ETAG);
      response->addHeader("Cache-Control", "no-cache");
      request->send(response);
      return;
    }
    AsyncWebServerResponse *response = new FlashResponse(200, "text/html; charset=utf-8", WEBUI_HTML_GZ, WEBUI_HTML_GZ_LEN);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("ETag", WEBUI_ETAG);
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // World map for the web UI station map (Natural Earth 1:110m SVG, gzipped in flash), revalidated by ETag
  server.on("/map.svg", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if (request->hasHeader("If-None-Match") && request->header("If-None-Match") == WORLDMAP_ETAG) {
      AsyncWebServerResponse *response = request->beginResponse(304);
      response->addHeader("ETag", WORLDMAP_ETAG);
      response->addHeader("Cache-Control", "no-cache");
      request->send(response);
      return;
    }
    AsyncWebServerResponse *response = new FlashResponse(200, "image/svg+xml", WORLDMAP_SVG_GZ, WORLDMAP_SVG_GZ_LEN);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("ETag", WORLDMAP_ETAG);
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
    static char json[960];
    int len = buildStatusJson(json, sizeof(json));
    AsyncWebServerResponse *response = new FlashResponse(200, "application/json", (const uint8_t *)json, (size_t)len);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
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
    AsyncWebServerResponse *response = new FlashResponse(200, "application/octet-stream", ft8GetWebWaterfallBuffer(), 36 * 120);
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
  ft8TxRegisterWeb(server);

  server.on("/api/timesync", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if (request->hasParam("epoch")) {
      uint32_t ep = strtoul(request->getParam("epoch")->value().c_str(), NULL, 10);
      uint16_t ms = request->hasParam("ms") ? (uint16_t)strtoul(request->getParam("ms")->value().c_str(), NULL, 10) : 0;
      // Browser time is only a fallback; SNTP is more accurate when available
      if (!ntpIsAvailable()) ft8SetEpochTime(ep, ms);
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  // /api/control - Radio & FT8 control actions
  server.on("/api/control", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    // Runs in the AsyncTCP task: only copy the parameters, the radio is driven from loop()
    if (!queueControlRequest(request)) {
      request->send(503, "application/json", "{\"ok\":false,\"error\":\"control queue full\"}");
      return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  // /api/screenshot.bmp - Stream 320x170 16-bit RGB565 BMP from the (8-bit) sprite
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

        while (written + width * 2 <= maxLen && index < fileSize) {
          uint32_t pixOffset = (index - hdrSize) / 2;
          int rowFromBottom = pixOffset / width;
          int y = (height - 1) - rowFromBottom;
          if (y < 0) break;
          uint16_t *dst = (uint16_t *)(buffer + written);
          for (uint32_t x = 0; x < width; x++) {
            dst[x] = spr.readPixel(x, y);
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

  ws.onEvent(wsOnEvent);
  server.addHandler(&ws);
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
