#include "Ft8Tx.h"
#include "Common.h"
#include "Utils.h"
#include "Layout-FT8.h"
#include "Storage.h"

#include <Preferences.h>
#include <ESPAsyncWebServer.h>

#include <math.h>
#include <stdarg.h>
#include <sys/time.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_rom_gpio.h>
#include <driver/gpio.h>
#include <soc/rtc.h>
#include <soc/i2s_struct.h>
#include <soc/io_mux_reg.h>
#include <soc/gpio_sig_map.h>
#include <soc/regi2c_apll.h>
#include <esp_private/regi2c_ctrl.h>
#include <esp_private/periph_ctrl.h>
#include <ft8_lib.h>

void ft8GetCurrentUTC(uint8_t *h, uint8_t *m, uint8_t *s, uint16_t *msOut);   // Layout-FT8.cpp

#define TX_XTAL_HZ   40000000.0
#define TX_VCO_MAX   500e6
#define TX_TONE_HZ   6.25
#define TX_MAX_TONE_ERR 1.5    // Hz at tone 7; decodes were fine at -1.03 (GPIO16 BCK)

// Station and TX settings, saved in NVS ("ft8tx"); set by serial K commands or /api/ft8tx
static char myCall[12] = "";
static char myGrid[5] = "";
static int8_t txPin = 0;              // 0 = I2S0 MCLK on GPIO0 (CLK_OUT1), else I2S0 BCK on this GPIO
static uint16_t txAudioHz = 1500;     // tone 0 above the USB dial, as in WSJT-X
static int32_t txPpm10 = 0;           // ESP32 crystal error, 0.1 ppm (positive = crystal fast)
// Debug only (not saved)
static double txFreqHz = 0;           // RF frequency of tone 0; 0 = follow the dial (dial + txAudioHz)
static double txVcoMin = 350e6;       // APLL VCO lower limit tried by the search
static bool txLatchSdm = false;       // pulse SDM_STOP after each tone write (as the IDF does)
static bool txWifiOff = false;        // stop WiFi while an FT8 message is sent
static double planForHz = 0;          // RF frequency the current plan was made for
static bool txEnabled = false;        // "Enable TX" (as in WSJT-X): off after every boot, nothing is sent while off
static uint32_t planUs = 0;           // time the last divider search took

// APLL / I2S setup chosen by txPlan()
struct TxPlan {
  uint32_t odiv, mdiv, bdiv;   // APLL output divider, I2S MCLK divider, BCK divider (1 = MCLK pin)
  uint32_t sdm;                // 4 + sdm2 + sdm1/256 + sdm0/65536, scaled by 65536
  uint32_t stepsPerTone;
  double stepHz, baseHz, vcoHz;
};
static TxPlan plan;
static bool planOk = false;

static volatile bool txOn = false;          // RF output running
static volatile bool txAbort = false;
static TaskHandle_t txTask = NULL;
static uint8_t txTones[FT8_NN];
static char txText[40];

bool ft8TxBusy() { return txOn || txTask != NULL; }

// 15 s slot number of the day for the current time, and ms into it
static uint32_t txSlotNow(uint32_t *intoMs) {
  uint8_t h, m, s;
  uint16_t ms;
  ft8GetCurrentUTC(&h, &m, &s, &ms);
  uint32_t daySec = (uint32_t)h * 3600UL + (uint32_t)m * 60UL + s;
  if (intoMs) *intoMs = (daySec % 15) * 1000UL + ms;
  return daySec / 15;
}
static uint32_t txSlotNo = UINT32_MAX;   // slot of the last queued message
static void txEvent(const char *fmt, ...);   // web page event log, below

static bool txAllowed() {
  if (txEnabled) return true;
  Serial.println("[TX] Transmit is disabled (Ke1 or the Enable TX box on the web page)");
  return false;
}

static double xtalHz() { return TX_XTAL_HZ * (1.0 + txPpm10 * 1e-7); }

// Find dividers that put one APLL step (or a whole number of steps) closest to 6.25 Hz at txFreqHz
static double txWantHz() {
  if (txFreqHz > 0) return txFreqHz;
  return currentFrequency * 1000.0 + txAudioHz;
}

static bool txPlan() {
  const double xt = xtalHz();
  const double wantHz = txWantHz();
  double bestErr = 1e9;
  TxPlan best = {};
  const uint32_t bMin = txPin == 0 ? 1 : 2, bMax = txPin == 0 ? 1 : 63;
  for (uint32_t odiv = 0; odiv <= 31; odiv++) {
    for (uint32_t mdiv = 2; mdiv <= 255; mdiv++) {
      double base = wantHz * 2.0 * (odiv + 2) * mdiv;   // VCO for bdiv = 1
      if (base * bMin > TX_VCO_MAX) break;
      uint32_t bStart = (uint32_t)ceil(txVcoMin / base);
      if (bStart < bMin) bStart = bMin;
      for (uint32_t bdiv = bStart; bdiv <= bMax; bdiv++) {
        double d = 2.0 * (odiv + 2) * mdiv * bdiv;
        double vco = wantHz * d;
        if (vco > TX_VCO_MAX) break;
        double step = xt / 65536.0 / d;
        uint32_t m = (uint32_t)lround(TX_TONE_HZ / step);
        if (m < 1 || m > 30) continue;
        double err = fabs(m * step - TX_TONE_HZ) * 7.0;   // error at tone 7
        // Prefer exact spacing, then the VCO nearest the middle of its range
        err += fabs(vco - 425e6) * 1e-12;
        if (err < bestErr) {
          bestErr = err;
          best = { odiv, mdiv, bdiv, 0, m, step, 0, vco };
        }
      }
    }
  }
  if (bestErr > TX_MAX_TONE_ERR) return false;   // tones too far from 6.25 Hz to decode
  double d = 2.0 * (best.odiv + 2) * best.mdiv * best.bdiv;
  uint32_t sdm = (uint32_t)lround(best.vcoHz / xt * 65536.0);
  // Keep all 8 tones inside the low SDM byte so each tone is one register write
  uint32_t lo = sdm & 0xFF, top = 255 - 7 * best.stepsPerTone;
  if (lo > top) sdm -= lo - top;
  best.sdm = sdm;
  best.vcoHz = sdm * xt / 65536.0;
  best.baseHz = best.vcoHz / d;
  plan = best;
  return (sdm >> 16) >= 4 && (sdm >> 16) <= 4 + 63;
}

static void txPrintPlan() {
  double spacing = plan.stepHz * plan.stepsPerTone;
  Serial.printf("[TX] %s: base %.2f Hz (asked %.0f), VCO %.3f MHz, odiv=%lu mclk/%lu bck/%lu, step %.4f Hz x%lu = %.4f Hz/tone (tone 7 off by %+.2f Hz), sdm=%lu.%lu.%lu, ppm %+.1f\r\n",
                txPin == 0 ? "GPIO0 MCLK" : "BCK pin", plan.baseHz, txWantHz(), plan.vcoHz / 1e6,
                (unsigned long)plan.odiv, (unsigned long)plan.mdiv, (unsigned long)plan.bdiv, plan.stepHz,
                (unsigned long)plan.stepsPerTone, spacing, (spacing - TX_TONE_HZ) * 7.0,
                (unsigned long)((plan.sdm >> 16) - 4), (unsigned long)((plan.sdm >> 8) & 0xFF),
                (unsigned long)(plan.sdm & 0xFF), txPpm10 / 10.0);
  if (txPin > 0) Serial.printf("[TX] Output: I2S0 BCK on GPIO%d\r\n", txPin);
  Serial.printf("[TX] Divider search took %lu ms\r\n", (unsigned long)(planUs / 1000));
}

static inline void txSetTone(uint8_t tone) {
  REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_DSDM0, (plan.sdm & 0xFF) + tone * plan.stepsPerTone);
  if (txLatchSdm) {
    REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_SDM_STOP, 1);
    REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_SDM_STOP, 0);
  }
}

static void txStart() {
  if (txOn) return;
  periph_module_enable(PERIPH_I2S0_MODULE);
  rtc_clk_apll_enable(true);
  rtc_clk_apll_coeff_set(plan.odiv, plan.sdm & 0xFF, (plan.sdm >> 8) & 0xFF, (plan.sdm >> 16) - 4);

  I2S0.conf.tx_reset = 1;
  I2S0.conf.tx_reset = 0;
  I2S0.conf2.val = 0;                      // plain I2S, no LCD/camera mode
  I2S0.clkm_conf.clka_en = 1;              // I2S clock from APLL
  I2S0.clkm_conf.clkm_div_a = 1;
  I2S0.clkm_conf.clkm_div_b = 0;           // integer MCLK divider only (fractions add jitter)
  I2S0.clkm_conf.clkm_div_num = plan.mdiv;
  I2S0.clkm_conf.clk_en = 1;

  if (txPin == 0) {
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0_CLK_OUT1);
    REG_SET_FIELD(PIN_CTRL, CLK_OUT1, 0);  // CLK_OUT1 = I2S0 MCLK
  } else {
    if (txPin == PIN_LCD_BL) ledcDetach(PIN_LCD_BL);
    I2S0.sample_rate_conf.tx_bck_div_num = plan.bdiv;
    I2S0.sample_rate_conf.tx_bits_mod = 16;
    I2S0.conf.tx_slave_mod = 0;
    gpio_set_direction((gpio_num_t)txPin, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability((gpio_num_t)txPin, GPIO_DRIVE_CAP_3);
    esp_rom_gpio_connect_out_signal(txPin, I2S0O_BCK_OUT_IDX, false, false);
    I2S0.conf.tx_start = 1;                // BCK runs while TX is started, even with an empty FIFO
  }
  txOn = true;
}

static void txStop() {
  if (!txOn) return;
  if (txPin == 0) {
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0_GPIO0);
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
  } else {
    I2S0.conf.tx_start = 0;
    esp_rom_gpio_connect_out_signal(txPin, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction((gpio_num_t)txPin, GPIO_MODE_INPUT);
    if (txPin == PIN_LCD_BL) {
      ledcAttach(PIN_LCD_BL, 16000, 8);
      ledcWrite(PIN_LCD_BL, sleepOn() ? 0 : currentBrt);
    }
  }
  I2S0.clkm_conf.clk_en = 0;
  rtc_clk_apll_enable(false);
  txOn = false;
}

// Sleep until an absolute esp_timer time, busy-waiting the last 2 ms
static void txWaitUntil(int64_t t) {
  for (;;) {
    int64_t left = t - esp_timer_get_time();
    if (left <= 0 || txAbort) return;
    if (left > 3000) vTaskDelay(pdMS_TO_TICKS((left - 2000) / 1000));
  }
}

static void txMessageTask(void *) {
  // Start 0.5 s into the next 15 s slot (DT = 0)
  uint8_t h, m, s;
  uint16_t ms;
  ft8GetCurrentUTC(&h, &m, &s, &ms);
  uint32_t intoSlot = (s % 15) * 1000UL + ms;
  int64_t t0 = esp_timer_get_time() + (int64_t)(15000UL - intoSlot + 500UL) * 1000;
  if (15000UL - intoSlot + 500UL > 15000UL) t0 -= 15000LL * 1000;   // still before 0.5 s of this slot
  Serial.printf("[TX] '%s' starts in %.1f s\r\n", txText, (t0 - esp_timer_get_time()) / 1e6);

  txWaitUntil(t0 - 300000);
  if (txWifiOff && !txAbort) esp_wifi_stop();
  txWaitUntil(t0);
  if (!txAbort) {
    txSetTone(txTones[0]);
    txStart();
    for (int i = 0; i < FT8_NN && !txAbort; i++) {
      txWaitUntil(t0 + (int64_t)i * 160000);
      txSetTone(txTones[i]);
    }
    txWaitUntil(t0 + (int64_t)FT8_NN * 160000);
    txStop();
    txSetTone(0);
  }
  if (txWifiOff) {
    esp_wifi_start();
    WiFi.reconnect();
  }
  Serial.printf("[TX] '%s' %s\r\n", txText, txAbort ? "aborted" : "sent");
  txTask = NULL;
  vTaskDelete(NULL);
}

static void txToneTestTask(void *) {
  // Tones 0..7 for 2 s each, so a waterfall shows the 8-step ladder
  txSetTone(0);
  txStart();
  for (int k = 0; k < 8 && !txAbort; k++) {
    txSetTone(k);
    Serial.printf("[TX] tone %d = %.2f Hz\r\n", k, plan.baseHz + k * plan.stepsPerTone * plan.stepHz);
    int64_t t = esp_timer_get_time() + 2000000;
    txWaitUntil(t);
  }
  txStop();
  txSetTone(0);
  Serial.println("[TX] tone test done");
  txTask = NULL;
  vTaskDelete(NULL);
}

static bool txReplan() {
  planForHz = txWantHz();
  uint32_t t0 = micros();
  planOk = currentMode == USB && txPlan();
  planUs = micros() - t0;
  if (currentMode != USB) Serial.println("[TX] Radio must be in USB / FT8 mode to transmit");
  else if (!planOk) Serial.printf("[TX] No APLL setting for %.0f Hz (only ~3.5-5 MHz, 80 m FT8 at 3573 kHz)\r\n", planForHz);
  else txPrintPlan();
  return planOk;
}

// Plan again if the dial or the audio offset moved since the last plan
static bool txPlanReady() {
  if (planOk && planForHz == txWantHz() && currentMode == USB) return true;
  return txReplan();
}

static void txSave() {
  Preferences p;
  if (!p.begin("ft8tx", false, STORAGE_PARTITION)) return;
  p.putString("call", myCall);
  p.putString("grid", myGrid);
  p.putChar("pin", txPin);
  p.putUShort("audio", txAudioHz);
  p.putInt("ppm10", txPpm10);
  p.end();
}

static void txLoad() {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;
  Preferences p;
  if (!p.begin("ft8tx", true, STORAGE_PARTITION)) return;
  strlcpy(myCall, p.getString("call", "").c_str(), sizeof(myCall));
  strlcpy(myGrid, p.getString("grid", "").c_str(), sizeof(myGrid));
  txPin = p.getChar("pin", 0);
  txAudioHz = p.getUShort("audio", 1500);
  txPpm10 = p.getInt("ppm10", 0);
  p.end();
}

// Callsign: 3..10 of A-Z 0-9 /, with a digit; grid: AA00..RR99
static bool validCall(const char *c) {
  size_t n = strlen(c);
  bool digit = false;
  if (n < 3 || n > 10) return false;
  for (size_t i = 0; i < n; i++) {
    if (isdigit((unsigned char)c[i])) digit = true;
    else if (!isupper((unsigned char)c[i]) && c[i] != '/') return false;
  }
  return digit;
}
static bool validGrid(const char *g) {
  return strlen(g) == 4 && g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' &&
         isdigit((unsigned char)g[2]) && isdigit((unsigned char)g[3]);
}

static bool txSetStation(const char *call, const char *grid) {
  char c[sizeof(myCall)], g[sizeof(myGrid)];
  strlcpy(c, call, sizeof(c));
  strlcpy(g, grid, sizeof(g));
  for (char *q = c; *q; q++) *q = toupper((unsigned char)*q);
  g[0] = toupper((unsigned char)g[0]);
  g[1] = toupper((unsigned char)g[1]);
  if (!validCall(c) || !validGrid(g)) {
    Serial.printf("[QSO] Invalid station '%s %s'\r\n", call, grid);
    return false;
  }
  strlcpy(myCall, c, sizeof(myCall));
  strlcpy(myGrid, g, sizeof(myGrid));
  txSave();
  return true;
}

// Encode a message and send it in the next slot (this slot if it is still before 0.5 s)
static bool txQueue(const char *text) {
  if (!txAllowed()) return false;
  if (txTask) {
    Serial.printf("[TX] Busy, '%s' not sent\r\n", text);
    return false;
  }
  if (!myCall[0]) {
    Serial.println("[TX] Set the station callsign first (Kn<call> <grid>)");
    return false;
  }
  if (!txPlanReady()) return false;
  ftx_message_t msg;
  ftx_message_rc_t rc = ftx_message_encode(&msg, NULL, text);
  if (rc != FTX_MESSAGE_RC_OK) {
    Serial.printf("[TX] Cannot encode '%s' (rc=%d)\r\n", text, (int)rc);
    return false;
  }
  ft8_encode(msg.payload, txTones);
  strlcpy(txText, text, sizeof(txText));
  txEvent("TX %s", text);
  uint32_t into;
  uint32_t slot = txSlotNow(&into);
  txSlotNo = into < 500 ? slot : slot + 1;
  txStop();
  xTaskCreatePinnedToCore(txMessageTask, "ft8tx", 4096, NULL, 20, &txTask, 1);
  return true;
}

// Last TX / QSO events for the web page ("HH:MM:SS text")
#define TX_EVENTS 8
static char txEvents[TX_EVENTS][64];
static uint8_t txEventHead = 0, txEventCount = 0;
static uint32_t txEventSeq = 0;
static void txEvent(const char *fmt, ...) {
  uint8_t h, m, sec;
  uint16_t ms;
  ft8GetCurrentUTC(&h, &m, &sec, &ms);
  char *e = txEvents[txEventHead];
  int n = snprintf(e, sizeof(txEvents[0]), "%02u:%02u:%02u ", h, m, sec);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e + n, sizeof(txEvents[0]) - n, fmt, ap);
  va_end(ap);
  for (char *q = e; *q; q++) if (*q == '"' || *q == '\\') *q = ' ';
  txEventHead = (txEventHead + 1) % TX_EVENTS;
  if (txEventCount < TX_EVENTS) txEventCount++;
  txEventSeq++;
}

// ---- QSO sequencer: decides each listening slot's reply from what was decoded in it ----
// Caller:   CQ me grid      -> (dx me grid)  -> dx me rpt  -> (me dx Rrpt) -> dx me RR73 -> (me dx 73)
// Answerer: (CQ dx grid)    -> dx me grid    -> (me dx rpt) -> dx me Rrpt  -> (me dx RR73) -> dx me 73
enum QsoState { Q_IDLE, Q_CQ, Q_SENT_RPT, Q_SENT_RR73, Q_ANS_WAIT, Q_SENT_GRID, Q_SENT_RRPT, Q_SENT_73 };
static const char *const qsoNames[] = { "idle", "calling CQ", "sent report", "sent RR73",
                                        "waiting for CQ", "sent grid", "sent R-report", "sent 73" };
#define QSO_MAX_TRIES 4

static QsoState qso = Q_IDLE;
static char dxCall[12] = "";
static char dxWant[12] = "";       // answer only this station's CQ (empty = anyone)
static char rptSent[5] = "", rptRcvd[5] = "";
static uint8_t qsoTries = 0;

// Best message heard this slot for the sequencer
static char heardTo[12], heardDe[12], heardExtra[8];
static int heardSnr;
static bool heardAny = false;

static void qsoSet(QsoState st, const char *why) {
  qso = st;
  qsoTries = 0;
  Serial.printf("[QSO] %s%s%s\r\n", qsoNames[st], why ? ": " : "", why ? why : "");
  txEvent("%s%s%s", qsoNames[st], why ? ": " : "", why ? why : "");
}

static void qsoStart(QsoState st, const char *want) {
  if (!txAllowed()) return;
  if (!myCall[0] || !myGrid[0]) {
    Serial.println("[QSO] Set the station first: Kn<call> <grid>");
    return;
  }
  dxCall[0] = rptSent[0] = rptRcvd[0] = 0;
  strlcpy(dxWant, want ? want : "", sizeof(dxWant));
  heardAny = false;
  qsoSet(st, NULL);
  if (st == Q_CQ) {
    char m[40];
    snprintf(m, sizeof(m), "CQ %s %s", myCall, myGrid);
    txQueue(m);
  }
}

static bool isReport(const char *s) {
  return (s[0] == '+' || s[0] == '-') && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2]) && !s[3];
}
static bool isGrid(const char *s) {
  return strlen(s) == 4 && s[0] >= 'A' && s[0] <= 'R' && s[1] >= 'A' && s[1] <= 'R' &&
         isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) && strcmp(s, "RR73") != 0;
}

void ft8TxOnDecode(const char *text, int snr) {
  if (qso == Q_IDLE) return;
  char t[3][12] = { "", "", "" };
  int n = sscanf(text, "%11s %11s %11s", t[0], t[1], t[2]);
  if (n < 2) return;
  bool wanted;
  if (qso == Q_ANS_WAIT) {
    wanted = !strcmp(t[0], "CQ") && strcmp(t[1], myCall) && (!dxWant[0] || !strcmp(t[1], dxWant));
  } else if (qso == Q_CQ) {
    wanted = !strcmp(t[0], myCall) && n == 3 && isGrid(t[2]);
  } else {
    wanted = !strcmp(t[0], myCall) && !strcmp(t[1], dxCall);
  }
  if (!wanted || heardAny) return;
  txEvent("RX %+d %s", snr, text);
  strlcpy(heardTo, t[0], sizeof(heardTo));
  strlcpy(heardDe, t[1], sizeof(heardDe));
  strlcpy(heardExtra, n == 3 ? t[2] : "", sizeof(heardExtra));
  heardSnr = snr;
  heardAny = true;
}

void ft8TxSlotDone() {
  uint32_t slot = txSlotNow(NULL);
  bool heard = heardAny;
  heardAny = false;
  if (qso == Q_IDLE || slot == txSlotNo) return;   // our own transmit slot: nothing to answer

  char m[40] = "";
  const char *x = heard ? heardExtra : "";
  switch (qso) {
    case Q_CQ:
      if (heard) {
        strlcpy(dxCall, heardDe, sizeof(dxCall));
        snprintf(rptSent, sizeof(rptSent), "%+03d", constrain(heardSnr, -24, 24));
        snprintf(m, sizeof(m), "%s %s %s", dxCall, myCall, rptSent);
        qsoSet(Q_SENT_RPT, dxCall);
      } else if (++qsoTries < QSO_MAX_TRIES * 2) {
        snprintf(m, sizeof(m), "CQ %s %s", myCall, myGrid);
      } else qsoSet(Q_IDLE, "no answer to CQ");
      break;
    case Q_SENT_RPT:
      if (heard && x[0] == 'R' && isReport(x + 1)) {
        strlcpy(rptRcvd, x + 1, sizeof(rptRcvd));
        snprintf(m, sizeof(m), "%s %s RR73", dxCall, myCall);
        qsoSet(Q_SENT_RR73, NULL);
      } else if (++qsoTries < QSO_MAX_TRIES) {
        snprintf(m, sizeof(m), "%s %s %s", dxCall, myCall, rptSent);
      } else qsoSet(Q_IDLE, "no R-report");
      break;
    case Q_SENT_RR73:
      if (heard && !strcmp(x, "73")) {
        Serial.printf("[QSO] COMPLETE with %s: sent %s, received %s\r\n", dxCall, rptSent, rptRcvd);
        ft8AddLog("[QSO] complete with %s (sent %s, rcvd %s)", dxCall, rptSent, rptRcvd);
        txEvent("QSO with %s complete (sent %s, rcvd %s)", dxCall, rptSent, rptRcvd);
        qsoSet(Q_IDLE, "73 received");
      } else if (heard && x[0] == 'R' && ++qsoTries < QSO_MAX_TRIES) {
        snprintf(m, sizeof(m), "%s %s RR73", dxCall, myCall);   // they missed our RR73
      } else {
        Serial.printf("[QSO] COMPLETE with %s (no 73): sent %s, received %s\r\n", dxCall, rptSent, rptRcvd);
        txEvent("QSO with %s complete, no 73 (sent %s, rcvd %s)", dxCall, rptSent, rptRcvd);
        qsoSet(Q_IDLE, NULL);
      }
      break;
    case Q_ANS_WAIT:
      if (heard) {
        strlcpy(dxCall, heardDe, sizeof(dxCall));
        snprintf(m, sizeof(m), "%s %s %s", dxCall, myCall, myGrid);
        qsoSet(Q_SENT_GRID, dxCall);
      }
      break;
    case Q_SENT_GRID:
      if (heard && isReport(x)) {
        strlcpy(rptRcvd, x, sizeof(rptRcvd));
        snprintf(rptSent, sizeof(rptSent), "%+03d", constrain(heardSnr, -24, 24));
        snprintf(m, sizeof(m), "%s %s R%s", dxCall, myCall, rptSent);
        qsoSet(Q_SENT_RRPT, NULL);
      } else if (++qsoTries < QSO_MAX_TRIES) {
        snprintf(m, sizeof(m), "%s %s %s", dxCall, myCall, myGrid);
      } else qsoSet(Q_IDLE, "no report");
      break;
    case Q_SENT_RRPT:
      if (heard && (!strcmp(x, "RR73") || !strcmp(x, "RRR"))) {
        snprintf(m, sizeof(m), "%s %s 73", dxCall, myCall);
        Serial.printf("[QSO] COMPLETE with %s: sent %s, received %s\r\n", dxCall, rptSent, rptRcvd);
        ft8AddLog("[QSO] complete with %s (sent %s, rcvd %s)", dxCall, rptSent, rptRcvd);
        txEvent("QSO with %s complete (sent %s, rcvd %s)", dxCall, rptSent, rptRcvd);
        qsoSet(Q_SENT_73, NULL);
      } else if (++qsoTries < QSO_MAX_TRIES) {
        snprintf(m, sizeof(m), "%s %s R%s", dxCall, myCall, rptSent);
      } else qsoSet(Q_IDLE, "no RR73");
      break;
    case Q_SENT_73:
      if (heard && (!strcmp(x, "RR73") || !strcmp(x, "RRR")) && ++qsoTries < QSO_MAX_TRIES) {
        snprintf(m, sizeof(m), "%s %s 73", dxCall, myCall);   // they missed our 73
      } else qsoSet(Q_IDLE, NULL);
      break;
    default:
      break;
  }
  if (m[0]) txQueue(m);
}

// Abort a message or carrier on air now
static void txHalt() {
  txAbort = true;
  for (int i = 0; i < 50 && txTask; i++) delay(10);
  txStop();
  txAbort = false;
}

void ft8TxCommand(const char *args) {
  txLoad();
  while (*args == ' ') args++;
  char op = *args ? *args++ : '?';
  while (*args == ' ') args++;

  if (op != '0' && op != '?' && op != 'e' && op != 'x' && txTask) {
    Serial.println("[TX] Busy (K0 aborts)");
    return;
  }
  switch (op) {
    case '0':   // stop everything
      txHalt();
      Serial.println("[TX] OFF");
      break;
    case 'e':   // Ke1 / Ke0: enable or disable transmitting; disabling stops the QSO and any TX at once
      txEnabled = atoi(args) != 0;
      if (!txEnabled) {
        if (qso != Q_IDLE) qsoSet(Q_IDLE, "TX disabled");
        txHalt();
      }
      Serial.printf("[TX] Transmit %s\r\n", txEnabled ? "ENABLED" : "disabled");
      txEvent("TX %s", txEnabled ? "enabled" : "disabled");
      break;
    case 'f':   // Kf<Hz>: debug, fixed RF frequency of tone 0 (Kf0 = follow the dial again)
      txFreqHz = atof(args);
      txReplan();
      break;
    case 'o':   // Ko<Hz>: audio offset of tone 0 above the dial (saved)
      txAudioHz = constrain(atoi(args), 200, 2900);
      txSave();
      txReplan();
      break;
    case 'p':   // Kp<gpio>: 0 = GPIO0 MCLK, else BCK on that GPIO (saved)
      txStop();
      txPin = (int8_t)atoi(args);
      txSave();
      txReplan();
      break;
    case 'u':   // Ku<0.1 ppm>: ESP32 crystal error (saved)
      txPpm10 = atol(args);
      txSave();
      txReplan();
      break;
    case 'v':   // Kv<MHz>: lowest APLL VCO to allow
      txVcoMin = atof(args) * 1e6;
      txReplan();
      break;
    case 'l':   // Kl<0|1>: pulse SDM_STOP after tone writes
      txLatchSdm = atoi(args) != 0;
      Serial.printf("[TX] SDM latch %s\r\n", txLatchSdm ? "ON" : "OFF");
      break;
    case 'w':   // Kw<0|1>: WiFi off while sending
      txWifiOff = atoi(args) != 0;
      Serial.printf("[TX] WiFi off during TX: %s\r\n", txWifiOff ? "yes" : "no");
      break;
    case 'c':   // Kc[tone]: steady carrier on a tone (0..7)
    {
      if (!txAllowed() || !txPlanReady()) break;
      int k = constrain(atoi(args), 0, 7);
      txSetTone(k);
      txStart();
      Serial.printf("[TX] Carrier ON tone %d = %.2f Hz\r\n", k, plan.baseHz + k * plan.stepsPerTone * plan.stepHz);
      break;
    }
    case 't':   // Kt: tone ladder test
      if (!txAllowed() || !txPlanReady()) break;
      txStop();
      xTaskCreatePinnedToCore(txToneTestTask, "ft8tx", 4096, NULL, 20, &txTask, 1);
      break;
    case 'm':   // Km<message>: send one FT8 message in the next slot
      txQueue(args);
      break;
    case 'n':   // Kn<call> <grid>: station callsign and 4-character grid for QSOs
    {
      char call[sizeof(myCall)] = "", grid[sizeof(myGrid)] = "";
      if (sscanf(args, "%11s %4s", call, grid) == 2) txSetStation(call, grid);
      Serial.printf("[QSO] Station %s %s\r\n", myCall[0] ? myCall : "(not set)", myGrid);
      break;
    }
    case 'q':   // Kq: call CQ until someone answers, then run the QSO
      qsoStart(Q_CQ, NULL);
      break;
    case 'a':   // Ka[call]: wait for a CQ (from call, or anyone) and answer it
      qsoStart(Q_ANS_WAIT, *args ? args : NULL);
      break;
    case 'x':   // Kx: stop the QSO sequencer (a message already on air finishes)
      qsoSet(Q_IDLE, "stopped");
      break;
    default:
    {
      uint8_t h, m, sec;
      uint16_t ms;
      ft8GetCurrentUTC(&h, &m, &sec, &ms);
      struct timeval tv;
      gettimeofday(&tv, NULL);
      Serial.printf("[TX] UTC %02u:%02u:%02u.%03u (system clock %ld.%03ld)\r\n", h, m, sec, ms,
                    (long)tv.tv_sec, (long)(tv.tv_usec / 1000));
      if (!planOk) txReplan();
      else txPrintPlan();
      Serial.printf("[TX] %s, latch %d, WiFi off %d. Commands: Ke<0|1> Kn<call> <grid> Kq Ka[call] Kx Km<msg> K0 Kc<tone> Kt Ko<audio Hz> Kp<gpio> Ku<0.1ppm> Kf<Hz> Kv<MHz> Kl Kw\r\n",
                    txOn ? "ON" : "off", txLatchSdm, txWifiOff);
      break;
    }
  }
}

// ---- Web API: GET /api/ft8tx = status; POST action=save|cq|answer|stop|send ----
// The web handler runs in the AsyncTCP task, so it only queues K command lines for ft8TxPoll()
#define TX_WEBQ 4
static char txWebQ[TX_WEBQ][48];
static volatile uint8_t txWebHead = 0, txWebTail = 0;
static portMUX_TYPE txWebMux = portMUX_INITIALIZER_UNLOCKED;

static void txWebPush(const char *line) {
  portENTER_CRITICAL(&txWebMux);
  uint8_t next = (txWebHead + 1) % TX_WEBQ;
  if (next != txWebTail) {
    strlcpy(txWebQ[txWebHead], line, sizeof(txWebQ[0]));
    txWebHead = next;
  }
  portEXIT_CRITICAL(&txWebMux);
}

void ft8TxPoll() {
  txLoad();
  // Keep the plan in step with the dial so the web page shows whether this frequency can be sent
  static uint32_t lastCheck = 0;
  if (!txTask && millis() - lastCheck > 1000) {
    lastCheck = millis();
    if (currentMode == USB && planForHz != txWantHz()) {
      planForHz = txWantHz();
      uint32_t t0 = micros();
      planOk = txPlan();
      planUs = micros() - t0;
    }
  }
  while (txWebTail != txWebHead) {
    char line[48];
    portENTER_CRITICAL(&txWebMux);
    strlcpy(line, txWebQ[txWebTail], sizeof(line));
    txWebTail = (txWebTail + 1) % TX_WEBQ;
    portEXIT_CRITICAL(&txWebMux);
    ft8TxCommand(line);
  }
}

static String txStatusJson() {
  char b[200];
  bool here = planOk && planForHz == txWantHz() && currentMode == USB;
  double spacing = plan.stepHz * plan.stepsPerTone;
  snprintf(b, sizeof(b),
           "{\"call\":\"%s\",\"grid\":\"%s\",\"audio\":%u,\"pin\":%d,\"ppm\":%.1f,\"usb\":%s,\"plan\":%s,",
           myCall, myGrid, txAudioHz, txPin, txPpm10 / 10.0, currentMode == USB ? "true" : "false",
           here ? "true" : "false");
  String out;
  out.reserve(160 + TX_EVENTS * 68);   // one allocation: the heap is tight while pages load
  out = b;
  snprintf(b, sizeof(b), "\"rf\":%.1f,\"tone_err\":%.2f,\"enabled\":%s,\"on\":%s,\"busy\":%s,\"text\":\"%s\",",
           here ? plan.baseHz : txWantHz(), here ? (spacing - TX_TONE_HZ) * 7.0 : 0.0, txEnabled ? "true" : "false",
           txOn ? "true" : "false",
           txTask ? "true" : "false", txTask ? txText : "");
  out += b;
  snprintf(b, sizeof(b), "\"qso\":\"%s\",\"dx\":\"%s\",\"rpt_sent\":\"%s\",\"rpt_rcvd\":\"%s\",\"seq\":%lu,\"events\":[",
           qsoNames[qso], dxCall, rptSent, rptRcvd, (unsigned long)txEventSeq);
  out += b;
  for (uint8_t i = 0; i < txEventCount; i++) {
    uint8_t k = (txEventHead + TX_EVENTS - 1 - i) % TX_EVENTS;   // newest first
    out += i ? ",\"" : "\"";
    out += txEvents[k];
    out += "\"";
  }
  out += "]}";
  return out;
}

void ft8TxRegisterWeb(AsyncWebServer &server) {
  server.on("/api/ft8tx", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    auto param = [request](const char *name) -> String {
      if (request->hasParam(name, true)) return request->getParam(name, true)->value();
      if (request->hasParam(name)) return request->getParam(name)->value();
      return String();
    };
    String action = param("action");
    char line[48];
    if (action == "save") {
      String call = param("call"), grid = param("grid");
      call.trim();
      grid.trim();
      if (call.length() && grid.length()) {
        snprintf(line, sizeof(line), "n %s %s", call.c_str(), grid.c_str());
        txWebPush(line);
      }
      if (param("audio").length()) { snprintf(line, sizeof(line), "o%ld", param("audio").toInt()); txWebPush(line); }
      if (param("pin").length()) { snprintf(line, sizeof(line), "p%ld", param("pin").toInt()); txWebPush(line); }
      if (param("ppm").length()) { snprintf(line, sizeof(line), "u%ld", lroundf(param("ppm").toFloat() * 10)); txWebPush(line); }
    } else if (action == "cq") {
      txWebPush("q");
    } else if (action == "answer") {
      snprintf(line, sizeof(line), "a %s", param("call").c_str());
      txWebPush(line);
    } else if (action == "enable") {
      txWebPush(param("on") == "1" ? "e1" : "e0");
    } else if (action == "stop") {
      txWebPush("x");
      txWebPush("0");
    } else if (action == "send") {
      String msg = param("msg");
      msg.trim();
      msg.toUpperCase();
      if (msg.length()) {
        snprintf(line, sizeof(line), "m %s", msg.c_str());
        txWebPush(line);
      }
    }
    request->send(200, "application/json", txStatusJson());
  });
}
