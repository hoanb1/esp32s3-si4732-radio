#include "Ft8Tx.h"
#include "Common.h"
#include "Utils.h"
#include "Layout-FT8.h"

#include <math.h>
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

// Settings (serial K command, not saved)
static int8_t txPin = 0;              // 0 = I2S0 MCLK on GPIO0 (CLK_OUT1), else I2S0 BCK on this GPIO
static double txFreqHz = 3574500.0;   // RF frequency of tone 0 (80 m dial 3573 kHz + 1500 Hz audio)
static double txVcoMin = 350e6;       // APLL VCO lower limit tried by the search
static int32_t txPpm10 = 0;           // ESP32 crystal error, 0.1 ppm (positive = crystal fast)
static bool txLatchSdm = false;       // pulse SDM_STOP after each tone write (as the IDF does)
static bool txWifiOff = false;        // stop WiFi while an FT8 message is sent

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

static double xtalHz() { return TX_XTAL_HZ * (1.0 + txPpm10 * 1e-7); }

// Find dividers that put one APLL step (or a whole number of steps) closest to 6.25 Hz at txFreqHz
static bool txPlan() {
  const double xt = xtalHz();
  double bestErr = 1e9;
  TxPlan best = {};
  const uint32_t bMin = txPin == 0 ? 1 : 2, bMax = txPin == 0 ? 1 : 63;
  for (uint32_t odiv = 0; odiv <= 31; odiv++) {
    for (uint32_t mdiv = 2; mdiv <= 255; mdiv++) {
      for (uint32_t bdiv = bMin; bdiv <= bMax; bdiv++) {
        double d = 2.0 * (odiv + 2) * mdiv * bdiv;
        double vco = txFreqHz * d;
        if (vco > TX_VCO_MAX) break;
        if (vco < txVcoMin) continue;
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
  if (bestErr > 1e8) return false;
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
                txPin == 0 ? "GPIO0 MCLK" : "BCK pin", plan.baseHz, txFreqHz, plan.vcoHz / 1e6,
                (unsigned long)plan.odiv, (unsigned long)plan.mdiv, (unsigned long)plan.bdiv, plan.stepHz,
                (unsigned long)plan.stepsPerTone, spacing, (spacing - TX_TONE_HZ) * 7.0,
                (unsigned long)((plan.sdm >> 16) - 4), (unsigned long)((plan.sdm >> 8) & 0xFF),
                (unsigned long)(plan.sdm & 0xFF), txPpm10 / 10.0);
  if (txPin > 0) Serial.printf("[TX] Output: I2S0 BCK on GPIO%d\r\n", txPin);
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
  planOk = txPlan();
  if (!planOk) Serial.printf("[TX] No APLL setting for %.0f Hz\r\n", txFreqHz);
  else txPrintPlan();
  return planOk;
}

void ft8TxCommand(const char *args) {
  while (*args == ' ') args++;
  char op = *args ? *args++ : '?';
  while (*args == ' ') args++;

  if (op != '0' && op != '?' && txTask) {
    Serial.println("[TX] Busy (K0 aborts)");
    return;
  }
  switch (op) {
    case '0':   // stop everything
      txAbort = true;
      for (int i = 0; i < 50 && txTask; i++) delay(10);
      txStop();
      txAbort = false;
      Serial.println("[TX] OFF");
      break;
    case 'f':   // Kf<Hz>: RF frequency of tone 0
      txFreqHz = atof(args);
      txReplan();
      break;
    case 'p':   // Kp<gpio>: 0 = GPIO0 MCLK, else BCK on that GPIO
      txStop();
      txPin = (int8_t)atoi(args);
      txReplan();
      break;
    case 'u':   // Ku<0.1 ppm>: ESP32 crystal error
      txPpm10 = atol(args);
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
      if (!planOk && !txReplan()) break;
      int k = constrain(atoi(args), 0, 7);
      txSetTone(k);
      txStart();
      Serial.printf("[TX] Carrier ON tone %d = %.2f Hz\r\n", k, plan.baseHz + k * plan.stepsPerTone * plan.stepHz);
      break;
    }
    case 't':   // Kt: tone ladder test
      if (!planOk && !txReplan()) break;
      txStop();
      xTaskCreatePinnedToCore(txToneTestTask, "ft8tx", 4096, NULL, 20, &txTask, 1);
      break;
    case 'm':   // Km<message>: send one FT8 message in the next slot
    {
      if (!planOk && !txReplan()) break;
      ftx_message_t msg;
      ftx_message_rc_t rc = ftx_message_encode(&msg, NULL, args);
      if (rc != FTX_MESSAGE_RC_OK) {
        Serial.printf("[TX] Cannot encode '%s' (rc=%d)\r\n", args, (int)rc);
        break;
      }
      ft8_encode(msg.payload, txTones);
      strlcpy(txText, args, sizeof(txText));
      txStop();
      xTaskCreatePinnedToCore(txMessageTask, "ft8tx", 4096, NULL, 20, &txTask, 1);
      break;
    }
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
      Serial.printf("[TX] %s, latch %d, WiFi off %d. Commands: K0 Kc<tone> Kt Km<msg> Kf<Hz> Kp<gpio> Ku<0.1ppm> Kv<MHz> Kl Kw\r\n",
                    txOn ? "ON" : "off", txLatchSdm, txWifiOff);
      break;
    }
  }
}
