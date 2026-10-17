#include "Layout-FT8.h"
#include "Common.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "Storage.h"

#include <WiFi.h>
#include <esp_timer.h>
#include <soc/sens_struct.h>
#include <hal/misc.h>
#include <sys/time.h>
#include <stdarg.h>
#include <math.h>
#include <ft8_lib.h>
#include <websdr_test1.h>

#define FT8_ADC_PIN         34
#define FT8_SAMPLE_RATE     6000
#define FT8_ADC_RATE        16000 // GPIO34 sampled by a hardware timer (80 MHz / 5 / 1000), resampled 3/8 to 6 kHz
#define FT8_RS_UP           3
#define FT8_RS_DOWN         8
#define FT8_RS_TAPS         96    // taps per polyphase branch (prototype = 288 taps at 48 kHz)
#define FT8_RAW_RING        4096  // 256 ms of raw 16 kHz samples
#define FT8_BLOCK_SAMPLES   960   // 160 ms per FT8 symbol @ 6000 Hz
#define FT8_RING_BLOCKS     4     // 4 x 160 ms = 640 ms ring buffer (absorbs WiFi/I2C jitter without dropping blocks)
#define FT8_MAX_CANDIDATES  100
#define FT8_LDPC_ITERATIONS 35
#define FT8_MIN_SYNC_SCORE  4     // Catch all weak & moderate off-air FT8 signals
#define FT8_ADC_VOLUME      48    // SI4732 volume for GPIO34 capture; the line out sits only ~0.1 V above the ADC floor, so louder clips

// Battery ADC reading (GPIO35), taken at boot before ADC1 switches to DMA mode
volatile uint16_t g_latestVbatRaw = 2400;

// Lock-free 4-block ADC capture ring buffer (4 * 960 * 2 bytes = 7,680 bytes)
static int16_t adcBuf[FT8_RING_BLOCKS][FT8_BLOCK_SAMPLES];
static volatile uint8_t adcWriteBlock = 0;
static volatile uint8_t adcReadBlock = 0;
static volatile uint8_t readyBufIdx = 0;
static volatile uint32_t adcDroppedBlocks = 0;
static volatile uint32_t adcBlockCounter[FT8_RING_BLOCKS];
static volatile uint32_t adcBlockSlot[FT8_RING_BLOCKS];   // 15 s slot id the block belongs to
static volatile uint16_t adcBlockIndex[FT8_RING_BLOCKS];  // block number within that slot (0 = starts at :00.0)
static volatile uint32_t adcTotalBlocks = 0;
static hw_timer_t *adcTimer = NULL;
static volatile bool adcRunning = false;
static volatile uint16_t adcRaw[FT8_RAW_RING];
static volatile uint32_t adcRawHead = 0;                   // total raw samples written by the timer ISR
static uint32_t monSlotId = 0xFFFFFFFF;                     // slot currently being collected into liveMon
static bool monSlotUsable = false;                           // false if the slot start was missed

// Live ADC signal statistics
static float lastAdcMean = 950.0f;
static int16_t lastAdcMin = 950;
static int16_t lastAdcMax = 950;
static int16_t lastAdcVpp = 0;
static float lastBlockRms = 10.0f;
static uint8_t ft8AudioLevel = 0;

// FT8 DSP Monitor (allocated once in ft8Init)
static monitor_t liveMon;
static bool monInitialized = false;
extern "C" {
  float g_ft8_dspFrame[1104]; // 960 samples for FFT + 1103 floats (522+581) for LDPC tov/toc
}
#define dspFrame g_ft8_dspFrame

// Waterfall buffer (36 rows x 120 cols = 4,320 bytes, shared by Web UI and LCD)
#define WEB_WF_ROWS 36
static uint8_t webWf[WEB_WF_ROWS * 120];
#define LCD_WF_ROWS 36
#define LCD_WF_COLS 120

// Decoded FT8 traffic log
static FT8MessageEntry ft8Traffic[FT8_MAX_TRAFFIC];
static int ft8TrafficCount = 0;
static uint32_t ft8TotalDecoded = 0;

// System diagnostic log ring buffer for Web UI (/api/logs) - fixed char array avoids heap fragmentation
static char ft8Logs[FT8_MAX_LOGS][96];
static int ft8LogHead = 0;
static int ft8LogCount = 0;

// FT8 slot timing & state
static uint32_t manualSlotSyncMs = 0;
static uint32_t epochBaseSec = 0;
static uint32_t epochBaseMs = 0;
static bool hasManualEpoch = false;
static uint8_t lastSlotSec = 255;
static bool slotDecoded = false;
static bool ft8IsDecoding = false;
static uint32_t ft8LastDurationMs = 0;
static uint8_t ft8ViewMode = FT8_VIEW_SPLIT;
static uint8_t ft8DigitalMode = FT8_DIG_OFF;
static bool ft8SpeakerMuted = false;  // Speaker amplifier off (SI4732 keeps feeding GPIO34); set automatically in FT8/FT4
static uint8_t ft8AdcVolume = FT8_ADC_VOLUME;   // Speaker off by default in FT8/FT4 (the SI4732 runs loud for the ADC)
static bool ft8AudioStreamOn = false;
static uint32_t ft8AudioStreamSeq = 0;

// Hash table for FT8 nonstandard callsigns
static ftx_callsign_hash_interface_t callHashIf;
struct CallHashEntry {
  char call[12];
  uint32_t hash22;
  bool used;
};
static CallHashEntry callHashTable[32];

static void hashInit() {
  memset(callHashTable, 0, sizeof(callHashTable));
}

static void hashSave(const char *callsign, uint32_t n22) {
  uint32_t idx = (n22 * 2654435761UL) & 31;
  for (int i = 0; i < 32; i++) {
    int pos = (idx + i) & 31;
    if (!callHashTable[pos].used || callHashTable[pos].hash22 == n22 || strcmp(callHashTable[pos].call, callsign) == 0) {
      strncpy(callHashTable[pos].call, callsign, 11);
      callHashTable[pos].call[11] = '\0';
      callHashTable[pos].hash22 = n22;
      callHashTable[pos].used = true;
      return;
    }
  }
}

static bool hashLookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char *callsign) {
  int shift = (hash_type == FTX_CALLSIGN_HASH_12_BITS) ? 10 : (hash_type == FTX_CALLSIGN_HASH_10_BITS ? 12 : 0);
  for (int i = 0; i < 32; i++) {
    if (callHashTable[i].used && ((callHashTable[i].hash22 >> shift) == hash)) {
      strcpy(callsign, callHashTable[i].call);
      return true;
    }
  }
  return false;
}

void ft8GetCurrentUTC(uint8_t *h, uint8_t *m, uint8_t *s, uint16_t *msOut = NULL);

void ft8AddLog(const char *fmt, ...) {
  char buf[104];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  uint8_t h = 0, m = 0, s = 0;
  ft8GetCurrentUTC(&h, &m, &s);
  snprintf(ft8Logs[ft8LogHead], sizeof(ft8Logs[0]), "[%02u:%02u:%02u] %s", h, m, s, buf);
  ft8LogHead = (ft8LogHead + 1) % FT8_MAX_LOGS;
  if (ft8LogCount < FT8_MAX_LOGS) ft8LogCount++;
}

String ft8GetLogsJson() {
  String out;
  out.reserve(1024);
  out = "[";
  int start = (ft8LogHead - ft8LogCount + FT8_MAX_LOGS) % FT8_MAX_LOGS;
  for (int i = 0; i < ft8LogCount; i++) {
    if (i > 0) out += ",";
    String s = ft8Logs[(start + i) % FT8_MAX_LOGS];
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    out += "\"" + s + "\"";
  }
  out += "]";
  return out;
}

// Helper to get current UTC H:M:S and sub-second ms
void ft8GetCurrentUTC(uint8_t *h, uint8_t *m, uint8_t *s, uint16_t *msOut) {
  uint8_t ch = 0, cm = 0, cs = 0;
  uint16_t cms = 0;
  if (clockGetHMS(&ch, &cm, &cs, &cms)) {
    *h = ch;
    *m = cm;
    *s = cs;
    if (msOut) *msOut = cms;
    return;
  }
  if (hasManualEpoch) {
    uint32_t elapsedMs = millis() - epochBaseMs;
    uint32_t nowSec = epochBaseSec + elapsedMs / 1000;
    *h = (nowSec % 86400UL) / 3600UL;
    *m = (nowSec % 3600UL) / 60UL;
    *s = nowSec % 60UL;
    if (msOut) *msOut = elapsedMs % 1000;
    return;
  }
  uint32_t el = millis() - manualSlotSyncMs;
  uint32_t totSec = el / 1000;
  *h = (totSec / 3600) % 24;
  *m = (totSec / 60) % 60;
  *s = totSec % 60;
  if (msOut) *msOut = el % 1000;
}

uint8_t ft8GetCurrentSlotSecond() {
  uint8_t h = 0, m = 0, s = 0;
  ft8GetCurrentUTC(&h, &m, &s);
  return s % 15;
}

void ft8SyncSlotManual() {
  manualSlotSyncMs = millis();
  uint8_t h = 0, m = 0, s = 0;
  if (clockGetHMS(&h, &m, &s, NULL)) {
    uint8_t alignedSec = (s / 15) * 15;
    clockSet(h, m, alignedSec);
  } else if (hasManualEpoch) {
    uint32_t elapsedMs = millis() - epochBaseMs;
    uint32_t nowSec = epochBaseSec + elapsedMs / 1000;
    epochBaseSec = (nowSec / 15) * 15;
    epochBaseMs = millis();
  }
  if (monInitialized) monitor_reset(&liveMon);
  slotDecoded = false;
  ft8AddLog("[INFO] FT8 slot timer aligned to :00");
}

void ft8SetEpochTime(uint32_t epoch, uint16_t ms) {
  if (epoch < 1700000000UL) return;
  struct timeval tv = { (time_t)epoch, (suseconds_t)(ms % 1000) * 1000 };
  settimeofday(&tv, NULL);
  epochBaseSec = epoch;
  epochBaseMs = millis();
  hasManualEpoch = true;
  uint8_t h = (epoch % 86400UL) / 3600UL;
  uint8_t m = (epoch % 3600UL) / 60UL;
  uint8_t s = epoch % 60UL;
  clockSet(h, m, s);
  ft8AddLog("[INFO] UTC clock set: %02u:%02u:%02u", h, m, s);
}

// 15 s slot id and position inside the slot from the UTC clock
static uint32_t ft8SlotId(uint32_t *msIntoSlot) {
  uint8_t h = 0, m = 0, s = 0;
  uint16_t ms = 0;
  ft8GetCurrentUTC(&h, &m, &s, &ms);
  uint32_t daySec = (uint32_t)h * 3600UL + (uint32_t)m * 60UL + s;
  if (msIntoSlot) *msIntoSlot = (daySec % 15) * 1000UL + ms;
  return daySec / 15;
}

// Crystal-timed ADC capture: a hardware timer interrupt reads GPIO34 at exactly 16 kHz straight from the
// SAR ADC registers (pipelined: collect the previous conversion, start the next one), and a task resamples
// 16 kHz -> 6 kHz with a polyphase FIR (low-pass ~2.75 kHz). Blocks of 960 samples (one FT8 symbol) are
// tagged with their slot id and index; block 0 starts at the input sample taken at the slot start.
static float rsTaps[FT8_RS_UP][FT8_RS_TAPS];
static float rsHist[FT8_RS_TAPS];

static void rsDesign() {
  const int n = FT8_RS_UP * FT8_RS_TAPS;
  const float fs = (float)FT8_ADC_RATE * FT8_RS_UP;   // 48 kHz prototype rate
  const float fc = 2750.0f / fs;                      // cycles per sample
  float proto[FT8_RS_UP * FT8_RS_TAPS];
  float sum = 0.0f;
  for (int i = 0; i < n; i++) {
    float m = i - (n - 1) / 2.0f;
    float sinc = (fabsf(m) < 1e-6f) ? 2.0f * fc : sinf(2.0f * (float)M_PI * fc * m) / ((float)M_PI * m);
    float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * i / (n - 1));
    proto[i] = sinc * w;
    sum += proto[i];
  }
  for (int i = 0; i < n; i++) rsTaps[i % FT8_RS_UP][i / FT8_RS_UP] = proto[i] * FT8_RS_UP / sum;
}

static void IRAM_ATTR adcTimerIsr() {
  uint16_t v = (uint16_t)HAL_FORCE_READ_U32_REG_FIELD(SENS.sar_meas_start1, meas1_data_sar);
  SENS.sar_meas_start1.meas1_start_sar = 0;
  SENS.sar_meas_start1.meas1_start_sar = 1;
  uint32_t h = adcRawHead;
  adcRaw[h & (FT8_RAW_RING - 1)] = v;
  adcRawHead = h + 1;
}

static void adcTask(void *arg) {
  uint32_t tail = adcRawHead;           // next raw sample to consume (absolute index)
  uint32_t histCount = 0;
  int histPos = 0;
  uint64_t outIdx = 0;                  // absolute output sample index
  uint64_t inBase = tail;               // raw index corresponding to output 0
  uint16_t sampleIdx = 0;
  uint16_t blockInSlot = 0;
  uint32_t slotId = 0xFFFFFFFF;
  uint32_t pendSlot = 0xFFFFFFFF;
  uint32_t pendInIdx = 0;               // raw index taken at the slot boundary
  const uint32_t groupDelay = (FT8_RS_TAPS - 1) / 2;   // FIR delay in raw samples

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(4));
    if (!adcRunning) { tail = adcRawHead; continue; }

    // Locate the next slot boundary in the raw stream
    uint32_t msInto = 0;
    uint32_t nowSlot = ft8SlotId(&msInto);
    uint32_t head = adcRawHead;
    if (nowSlot != slotId && nowSlot != pendSlot) {
      pendSlot = nowSlot;
      pendInIdx = head - msInto * (FT8_ADC_RATE / 1000);
    }
    if (head - tail > FT8_RAW_RING - 64) tail = head - (FT8_RAW_RING - 64);   // overrun: skip ahead

    while (tail != head) {
      rsHist[histPos] = (float)adcRaw[tail & (FT8_RAW_RING - 1)];
      histPos = (histPos + 1) % FT8_RS_TAPS;
      if (histCount < FT8_RS_TAPS) histCount++;
      uint32_t newest = tail;
      tail++;

      // Emit every output whose newest needed input is available: m = floor(8n/3)
      for (;;) {
        uint64_t prod = (outIdx + 1) * FT8_RS_DOWN;
        uint32_t m = (uint32_t)(inBase + prod / FT8_RS_UP);
        if ((int32_t)(m - newest) > 0) break;
        int phase = prod % FT8_RS_UP;
        outIdx++;
        if (histCount < FT8_RS_TAPS) continue;
        float acc = 0.0f;
        int k = (histPos + FT8_RS_TAPS - 1 - (int)(newest - m)) % FT8_RS_TAPS;
        for (int q = 0; q < FT8_RS_TAPS; q++) {
          acc += rsTaps[phase][q] * rsHist[k];
          if (--k < 0) k = FT8_RS_TAPS - 1;
        }

        // New slot: start block 0 at the output that corresponds to the boundary input sample
        if (pendSlot != 0xFFFFFFFF && (int32_t)(m - (pendInIdx + groupDelay)) >= 0) {
          slotId = pendSlot;
          pendSlot = 0xFFFFFFFF;
          sampleIdx = 0;
          blockInSlot = 0;
        }

        uint8_t wBuf = adcWriteBlock;
        adcBuf[wBuf][sampleIdx] = (int16_t)lroundf(acc);
        if (++sampleIdx >= FT8_BLOCK_SAMPLES) {
          sampleIdx = 0;
          adcBlockCounter[wBuf] = adcTotalBlocks++;
          adcBlockSlot[wBuf] = slotId;
          adcBlockIndex[wBuf] = blockInSlot++;
          readyBufIdx = wBuf;
          uint8_t nextW = (wBuf + 1) % FT8_RING_BLOCKS;
          if (nextW != adcReadBlock) adcWriteBlock = nextW;
          else if (!slotDecoded && !ft8IsDecoding) adcDroppedBlocks++;
        }
      }
    }
  }
}

static bool adcStart() {
  // One regular read configures ADC1 channel 6 (GPIO34), 0 dB attenuation and the RTC controller
  analogSetPinAttenuation(FT8_ADC_PIN, ADC_0db);
  analogRead(FT8_ADC_PIN);
  SENS.sar_meas_wait2.force_xpd_sar = 3;                // keep the SAR ADC powered
  SENS.sar_meas_start1.meas1_start_force = 1;
  SENS.sar_meas_start1.sar1_en_pad_force = 1;
  SENS.sar_meas_start1.sar1_en_pad = (1 << 6);          // ADC1_CH6
  SENS.sar_meas_start1.meas1_start_sar = 0;
  SENS.sar_meas_start1.meas1_start_sar = 1;

  adcTimer = timerBegin(16000000);                      // 80 MHz / 5
  if (!adcTimer) return false;
  timerAttachInterrupt(adcTimer, &adcTimerIsr);
  timerAlarm(adcTimer, 16000000 / FT8_ADC_RATE, true, 0);
  adcRunning = true;
  return true;
}

void ft8Init() {
  if (monInitialized) return;

  hashInit();
  callHashIf.lookup_hash = hashLookup;
  callHashIf.save_hash = hashSave;

  // Initialize FT8 DSP monitor: 200..2900 Hz @ 6000 Hz, time_osr=1 (allocated in 32-bit IRAM), freq_osr=1
  Serial.printf("[FT8] Before monitor: IRAM-only free=%u, largest 32bit=%u\r\n",
                (unsigned int)(heap_caps_get_free_size(MALLOC_CAP_32BIT) - heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_32BIT));

  // FT8 stations use 200..3000 Hz audio; 2900 Hz keeps clear of the 3 kHz Nyquist edge
  monitor_config_t cfg;
  cfg.f_min = 200.0f;
  cfg.f_max = 2900.0f;
  cfg.sample_rate = FT8_SAMPLE_RATE;
  cfg.time_osr = 1;
  cfg.freq_osr = 1;
  cfg.protocol = FTX_PROTOCOL_FT8;

  monitor_init(&liveMon, &cfg);
  monInitialized = true;
  Serial.printf("[FT8] DSP Monitor ready: wf.mag=%p, osr1=%p (%dx%d, time_osr=%d), free_heap=%u (8bit=%u)\r\n",
                liveMon.wf.mag, liveMon.wf.mag_osr1, liveMon.wf.max_blocks, liveMon.wf.num_bins,
                liveMon.wf.time_osr, (unsigned int)ESP.getFreeHeap(),
                (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT));

  memset(webWf, 0, sizeof(webWf));

  // Battery is read once before the ADC1 DMA driver takes over the unit
  analogReadResolution(12);
  analogSetPinAttenuation(35, ADC_11db);
  g_latestVbatRaw = (uint16_t)analogRead(35);

  // Start crystal-timed capture of GPIO34 audio
  rsDesign();
  bool adcOk = adcStart();
  xTaskCreatePinnedToCore(adcTask, "ft8_adc", 4096, NULL, 5, NULL, 1);
  Serial.printf("[FT8] ADC capture %s (%d Hz timer, resample %d/%d -> %d Hz)\r\n",
                adcOk ? "running" : "FAILED", FT8_ADC_RATE, FT8_RS_UP, FT8_RS_DOWN, FT8_SAMPLE_RATE);

  ft8AddLog("[INFO] FT8 DSP receiver started on GPIO34 (6000 Hz, 200..2900 Hz)");
}

static int g_lastRowAvgMag = 0;
static int g_lastRowMinMag = 0;
static int g_lastRowMaxMag = 0;
static float g_absNoiseFloor = 111.0f;

// Update shared Web UI & LCD waterfall (36 rows x 120 cols) from the latest FFT block in mon->wf
static void pushMonitorRowToWaterfalls(const monitor_t *mon, int blockIdx) {
  if (!mon || !mon->wf.mag || blockIdx < 0 || blockIdx >= mon->wf.max_blocks) return;
  const uint8_t *rowMag0 = &mon->wf.mag[blockIdx * mon->wf.block_stride];
  int numBins = mon->wf.num_bins;
  const uint8_t *rowMag1 = (mon->wf.time_osr > 1 && mon->wf.mag_osr1)
                           ? &mon->wf.mag_osr1[blockIdx * mon->wf.block_stride]
                           : rowMag0;

  // Compute row mean, min, max across the passband (0.5 dB units)
  int sumMag = 0;
  int minMag = 255;
  int maxMag = 0;
  for (int b = 0; b < numBins; b++) {
    int v = wf_read_u8(&rowMag0[b]);
    sumMag += v;
    if (v < minMag) minMag = v;
    if (v > maxMag) maxMag = v;
  }
  int avgMag = sumMag / (numBins > 0 ? numBins : 1);
  g_lastRowAvgMag = avgMag;
  g_lastRowMinMag = minMag;
  g_lastRowMaxMag = maxMag;

  // Calibrate absolute hardware ADC noise floor ONLY when audio RMS is at the quiet floor (RMS < 13.0)
  // At Vol=0 (Muted), RMS is ~10.8 and avgMag is ~110..111.
  if (lastBlockRms < 13.0f && avgMag >= 95 && avgMag <= 116) {
    g_absNoiseFloor = 0.85f * g_absNoiseFloor + 0.15f * (float)avgMag;
  }

  // Step 1: Compute per-column average and peak across the 120 columns (200..2900 Hz)
  uint8_t colAvg[120];
  uint8_t colPeak[120];
  for (int c = 0; c < 120; c++) {
    int bStart = (c * numBins) / 120;
    int bEnd = ((c + 1) * numBins) / 120;
    if (bEnd <= bStart) bEnd = bStart + 1;
    int s = 0;
    uint8_t pk = 0;
    for (int b = bStart; b < bEnd; b++) {
      uint8_t m = wf_read_u8(&rowMag0[b]);
      uint8_t m1 = wf_read_u8(&rowMag1[b]);
      if (m1 > m) m = m1;
      s += m;
      if (m > pk) pk = m;
    }
    colAvg[c] = (uint8_t)(s / (bEnd - bStart));
    colPeak[c] = pk;
  }

  // Step 2: Preserve harmonic peaks (2/3 peak + 1/3 avg) + RC high-pass bass compensation (+6 at 200Hz .. 0 at 1300Hz)
  // with light temporal smoothing so voice formants and music notes on AM/FM stand out vividly
  static uint8_t prevSmooth[120] = {0};
  int smoothCol[120];
  int sumComp = 0;
  for (int c = 0; c < 120; c++) {
    int bassComp = (c < 60) ? ((60 - c) / 10) : 0;
    int rawVal = ((int)colPeak[c] * 2 + (int)colAvg[c]) / 3 + bassComp;
    int cL = (c > 0) ? (c - 1) : 0;
    int cR = (c < 119) ? (c + 1) : 119;
    int rawL = ((int)colPeak[cL] * 2 + (int)colAvg[cL]) / 3 + ((cL < 60) ? ((60 - cL) / 10) : 0);
    int rawR = ((int)colPeak[cR] * 2 + (int)colAvg[cR]) / 3 + ((cR < 60) ? ((60 - cR) / 10) : 0);
    int freqSmoothed = (rawL + 2 * rawVal + rawR + 2) >> 2;
    if (prevSmooth[c] < 10) {
      prevSmooth[c] = (uint8_t)freqSmoothed;
    } else {
      prevSmooth[c] = (uint8_t)((3 * freqSmoothed + (int)prevSmooth[c] + 2) >> 2);
    }
    smoothCol[c] = prevSmooth[c];
    sumComp += smoothCol[c];
  }
  int meanSmooth = sumComp / 120;

  // Volume & Signal Envelope gain:
  // On real AM/FM stations (SNR=15..31 dB, Vol=36), RMS is 16..35 and avgMag is 117..130, with harmonic peaks +4..+20 above meanSmooth.
  int volGain = 0;
  int baseEnv = 0;
  if (!ft8SpeakerMuted && volume > 0 && (lastBlockRms > 13.0f || avgMag > (int)(g_absNoiseFloor + 2.0f))) {
    int dbAboveFloor = avgMag - (int)g_absNoiseFloor;
    if (dbAboveFloor < 1) dbAboveFloor = 1;
    if (dbAboveFloor > 45) dbAboveFloor = 45;
    volGain = ((int)volume * (14 + dbAboveFloor / 2)) / 36;
    if (volGain < 5) volGain = 5;
    if (volGain > 38) volGain = 38;
    baseEnv = ((int)volume * dbAboveFloor * 2) / 36;
    if (baseEnv > 75) baseEnv = 75;
  }

  // Scroll waterfall (WEB_WF_ROWS x 120 cols) down by 1 row
  memmove(&webWf[120], &webWf[0], (WEB_WF_ROWS - 1) * 120);
  bool ssbMode = isSSB();
  for (int c = 0; c < 120; c++) {
    int audioVis = 0;
    if (ssbMode) {
      // On SSB (FT8), suppress flat white-noise floor so only real signals/tones above local passband mean light up
      int specDiff = smoothCol[c] - (meanSmooth + 3);
      if (specDiff > 0 && volGain > 0) {
        audioVis = specDiff * 14;
      }
    } else {
      // 1. Continuous Audio Spectrum + Harmonic Formant emphasis for AM/FM broadcast
      int aboveFloor = smoothCol[c] - (int)(g_absNoiseFloor + 2.0f);
      if (aboveFloor < 0) aboveFloor = 0;
      int specDiff = smoothCol[c] - (meanSmooth - 1);
      if (volGain > 0) {
        audioVis = (aboveFloor * 3 * (int)volume) / 36 + ((specDiff > 0) ? (specDiff * volGain) : 0);
        if (audioVis > 0 && audioVis < baseEnv) audioVis = baseEnv;
      }
    }

    // 2. Narrowband FT8 FSK tone detector (sharp 1-bin peak >= 4.0 dB above local neighborhood)
    int toneVis = 0;
    if ((int)colPeak[c] >= (int)(g_absNoiseFloor + 18.0f)) {
      int c0 = (c >= 3) ? (c - 3) : 0;
      int c1 = (c <= 116) ? (c + 3) : 119;
      int localSum = 0;
      for (int k = c0; k <= c1; k++) localSum += colAvg[k];
      int localBase = localSum / (c1 - c0 + 1);
      int toneDiff = (int)colPeak[c] - (localBase + 8);
      if (toneDiff > 0) {
        toneVis = toneDiff * 16;
      }
    }

    int vis = (toneVis > audioVis) ? toneVis : audioVis;
    if (vis > 255) vis = 255;
    webWf[c] = (uint8_t)vis;
  }
}


// Extract transmitting station callsign from unpacked FT8 message text
static void extractSenderCallsign(const char *msgText, char *outCall, size_t maxLen, bool *outIsCQ) {
  outCall[0] = '\0';
  *outIsCQ = false;

  char tmp[48];
  strncpy(tmp, msgText, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';

  char *tok1 = strtok(tmp, " ");
  char *tok2 = strtok(NULL, " ");
  char *tok3 = strtok(NULL, " ");

  if (!tok1) return;

  if (strcmp(tok1, "CQ") == 0) {
    *outIsCQ = true;
    if (tok2) {
      // Check if tok2 has a digit (standard callsign) or is a modifier like "DX", "TEST", "POTA"
      bool hasDigit = false;
      for (const char *p = tok2; *p; p++) {
        if (*p >= '0' && *p <= '9') { hasDigit = true; break; }
      }
      const char *chosen = (!hasDigit && tok3) ? tok3 : tok2;
      strncpy(outCall, chosen, maxLen - 1);
      outCall[maxLen - 1] = '\0';
    } else {
      strncpy(outCall, "CQ", maxLen - 1);
    }
  } else if (tok2) {
    strncpy(outCall, tok2, maxLen - 1);
    outCall[maxLen - 1] = '\0';
  } else {
    strncpy(outCall, tok1, maxLen - 1);
    outCall[maxLen - 1] = '\0';
  }
}

// Run FT8 Costas sync search + LDPC decoding on the current waterfall in `mon`
static int ft8DecodeFromMonitor(monitor_t *mon, bool isTest) {
  if (!mon || !mon->wf.mag || mon->wf.num_blocks < 40) return 0;

  ft8IsDecoding = true;
  uint32_t t0 = millis();

  ftx_candidate_t candidates[FT8_MAX_CANDIDATES];
  int numCandidates = ftx_find_candidates(&mon->wf, FT8_MAX_CANDIDATES, candidates, FT8_MIN_SYNC_SCORE);

  uint16_t seenHashes[32];
  int numSeen = 0;
  int decodedNow = 0;

  uint8_t utcH = 0, utcM = 0, utcS = 0;
  ft8GetCurrentUTC(&utcH, &utcM, &utcS);
  char timeStr[12];
  snprintf(timeStr, sizeof(timeStr), "%02u:%02u:%02u", utcH, utcM, utcS);

  int topScore = (numCandidates > 0) ? candidates[0].score : 0;
  uint16_t topFreqHz = (numCandidates > 0) ? (uint16_t)roundf((mon->min_bin + candidates[0].freq_offset) * 6.25f) : 0;
  int minLdpcErr = 999;
  uint16_t bestErrFreqHz = 0;
  float bestErrDt = 0.0f;
  int bestErrScore = 0;

  for (int i = 0; i < numCandidates; i++) {
    // Never exceed 1050 ms (live) or 1400 ms (test) so decoding never starves Web UI or next FT8 slot
    if (millis() - t0 > (isTest ? 1400UL : 1050UL)) break;

    const ftx_candidate_t *cand = &candidates[i];
    ftx_message_t msg;
    ftx_decode_status_t status;

    bool ok = ftx_decode_candidate(&mon->wf, cand, FT8_LDPC_ITERATIONS, &msg, &status);
    if (status.ldpc_errors < minLdpcErr) {
      minLdpcErr = status.ldpc_errors;
      bestErrFreqHz = (uint16_t)roundf((mon->min_bin + cand->freq_offset + (float)cand->freq_sub / mon->wf.freq_osr) * 6.25f);
      bestErrDt = (cand->time_offset + (float)cand->time_sub / mon->wf.time_osr) * 0.16f - (isTest ? 0.0f : 0.5f);
      bestErrScore = cand->score;
    }
    if (!ok) {
      continue;
    }

    // Deduplicate identical messages in the same slot
    bool duplicate = false;
    for (int h = 0; h < numSeen; h++) {
      if (seenHashes[h] == msg.hash) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    char text[40];
    ftx_message_offsets_t offsets;
    ftx_message_rc_t rc = ftx_message_decode(&msg, &callHashIf, text, &offsets);
    if (rc != FTX_MESSAGE_RC_OK || text[0] == '\0') {
      continue;
    }

    if (numSeen < 32) seenHashes[numSeen++] = msg.hash;

    // Compute SNR (dB), DT (s), and Audio Frequency (Hz)
    int snrDb = (cand->score / 3) - 24;
    if (snrDb < -24) snrDb = -24;
    if (snrDb > 24) snrDb = 24;

    float dtSec = (cand->time_offset + (float)cand->time_sub / mon->wf.time_osr) * 0.16f - (isTest ? 0.0f : 0.5f);
    uint16_t freqHz = (uint16_t)roundf((mon->min_bin + cand->freq_offset + (float)cand->freq_sub / mon->wf.freq_osr) * 6.25f);

    char senderCall[16];
    bool isCQ = false;
    extractSenderCallsign(text, senderCall, sizeof(senderCall), &isCQ);

    // Prepend newest message at index 0 of ft8Traffic[]
    if (ft8TrafficCount < FT8_MAX_TRAFFIC) {
      memmove(&ft8Traffic[1], &ft8Traffic[0], ft8TrafficCount * sizeof(FT8MessageEntry));
      ft8TrafficCount++;
    } else {
      memmove(&ft8Traffic[1], &ft8Traffic[0], (FT8_MAX_TRAFFIC - 1) * sizeof(FT8MessageEntry));
    }

    FT8MessageEntry *entry = &ft8Traffic[0];
    strncpy(entry->timeStr, timeStr, sizeof(entry->timeStr) - 1);
    entry->timeStr[sizeof(entry->timeStr) - 1] = '\0';
    entry->snr = (int8_t)snrDb;
    entry->dt = roundf(dtSec * 10.0f) / 10.0f;
    entry->freq = freqHz;
    strncpy(entry->call, senderCall, sizeof(entry->call) - 1);
    entry->call[sizeof(entry->call) - 1] = '\0';
    strncpy(entry->msg, text, sizeof(entry->msg) - 1);
    entry->msg[sizeof(entry->msg) - 1] = '\0';
    entry->isCQ = isCQ;

    decodedNow++;
    ft8TotalDecoded++;

    Serial.printf("  %s | %+3ddB | %+4.1fs | %4uHz | %-8s | %s\r\n",
                  timeStr, snrDb, entry->dt, freqHz, senderCall, text);
    ft8AddLog("[%s] %s | %+ddB | %uHz | %s", isTest ? "TEST" : "FT8", senderCall, snrDb, freqHz, text);
  }

  ft8LastDurationMs = millis() - t0;
  ft8IsDecoding = false;

  Serial.printf("[FT8 %s] %s %u kHz (BFO:%+d) | Decoded=%d in %lu ms (cand=%d, top=%d@%uHz, bestErr=%d@%uHz/DT%+4.1fs/sc%d, blk=%d, drop=%lu, Vpp=%d, RMS=%.1f)\r\n",
                isTest ? "TEST" : "LIVE", getCurrentBand()->bandName, currentFrequency, currentBFO,
                decodedNow, (unsigned long)ft8LastDurationMs, numCandidates, topScore, topFreqHz,
                minLdpcErr, bestErrFreqHz, bestErrDt, bestErrScore, mon->wf.num_blocks, (unsigned long)adcDroppedBlocks, lastAdcVpp, lastBlockRms);
  if (decodedNow > 0 || isTest) {
    ft8AddLog("[INFO] %s decode done: %d messages (%lu ms, %d blocks)",
              isTest ? "TEST" : "LIVE", decodedNow, (unsigned long)ft8LastDurationMs, mon->wf.num_blocks);
  }

  // Discard ADC blocks queued during the decode (tail of this slot, not needed any more)
  adcReadBlock = adcWriteBlock;

  return decodedNow;
}

static volatile bool g_webTestDecodePending = false;
static volatile bool g_webRedrawPending = false;

void ft8RequestTestDecode() {
  g_webTestDecodePending = true;
}

void ft8RequestRedraw() {
  g_webRedrawPending = true;
}

bool ft8TickWaterfall() {
  if (!monInitialized) ft8Init();

  bool needRedraw = false;
  if (g_webRedrawPending) {
    g_webRedrawPending = false;
    needRedraw = true;
  }
  if (g_webTestDecodePending) {
    g_webTestDecodePending = false;
    ft8RunTestDecode();
    return true;
  }

  uint8_t h = 0, m = 0, s = 0;
  uint16_t subMs = 0;
  ft8GetCurrentUTC(&h, &m, &s, &subMs);
  uint8_t slotSec = s % 15;
  bool digActive = (ft8DigitalMode != FT8_DIG_OFF) && (currentMode == USB || currentMode == LSB);

  lastSlotSec = slotSec;

  // Drain any pending 160ms ADC blocks from the lock-free ring buffer
  while (adcReadBlock != adcWriteBlock) {
    uint8_t rBuf = adcReadBlock;
    int16_t *raw = (int16_t *)adcBuf[rBuf];

    // A block from a new slot starts a new FT8 collection; block 0 begins exactly at the slot start
    uint32_t blkSlot = adcBlockSlot[rBuf];
    if (blkSlot != monSlotId) {
      if (!sleepOn() && isSSB() && (lastAdcMean < 100.0f || rx.getPatchId() == 0)) {
        Serial.printf("[FT8] Recovering SI4732 SSB patch / GPIO34 bias (mean=%.0f, patch=0x%04X)...\r\n",
                      lastAdcMean, rx.getPatchId());
        unloadSSB();
        selectBand(bandIdx, false);
      }
      monSlotId = blkSlot;
      monSlotUsable = (adcBlockIndex[rBuf] == 0);
      adcDroppedBlocks = 0;
      monitor_reset(&liveMon);
      slotDecoded = digActive && !monSlotUsable;   // a slot whose start was missed cannot be time-aligned
      if (uiLayoutIdx == UI_FT8) needRedraw = true;
    }
    for (int i = 1; i < FT8_BLOCK_SAMPLES - 1; i++) {
      int16_t prev = raw[i - 1];
      int16_t cur  = raw[i];
      int16_t next = raw[i + 1];
      if (abs(cur - prev) > 300 && abs(cur - next) > 300) {
        raw[i] = (prev + next) >> 1;
      }
    }

    // Debug: stream raw 6000 Hz ADC blocks over Serial (921600 baud) as "AUD0" + seq + 960 x int16
    if (ft8AudioStreamOn) {
      Serial.write((const uint8_t *)"AUD0", 4);
      ft8AudioStreamSeq = adcBlockCounter[rBuf];
      Serial.write((const uint8_t *)&ft8AudioStreamSeq, 4);
      Serial.write((const uint8_t *)raw, FT8_BLOCK_SAMPLES * 2);
    }

    int32_t sum = 0;
    int16_t minV = 4095;
    int16_t maxV = 0;
    for (int i = 0; i < FT8_BLOCK_SAMPLES; i++) {
      int16_t v = raw[i];
      sum += v;
      if (v < minV) minV = v;
      if (v > maxV) maxV = v;
    }
    float mean = (float)sum / (float)FT8_BLOCK_SAMPLES;
    lastAdcMean = mean;
    lastAdcMin = minV;
    lastAdcMax = maxV;
    lastAdcVpp = maxV - minV;

    // Fixed linear scaling (+6dB digital boost for quiet speaker volume) preserves exact log-dB differences across all 79 symbols
    const float normScale = 1.0f / 256.0f;
    float sumSq = 0.0f;
    for (int i = 0; i < FT8_BLOCK_SAMPLES; i++) {
      float diff = (float)raw[i] - mean;
      sumSq += diff * diff;
      dspFrame[i] = diff * normScale;
    }
    lastBlockRms = sqrtf(sumSq / (float)FT8_BLOCK_SAMPLES);

    // Done reading this ring buffer slot
    adcReadBlock = (rBuf + 1) % FT8_RING_BLOCKS;

    // Compute VU meter percentage (0..100%)
    int vu = (int)((lastAdcVpp * 100) / 350);
    if (vu < 0) vu = 0;
    if (vu > 100) vu = 100;
    ft8AudioLevel = (uint8_t)vu;

    // In non-digital mode (FM / AM / SSB Voice), wrap monitor blocks continuously so waterfall never pauses
    if (!digActive && liveMon.wf.num_blocks >= liveMon.wf.max_blocks) {
      monitor_reset(&liveMon);
    }

    // Feed live 960-sample frame into FT8 STFT waterfall if slot hasn't finished yet
    if (!slotDecoded && liveMon.wf.num_blocks < liveMon.wf.max_blocks) {
      int blockIdx = liveMon.wf.num_blocks;
      monitor_process(&liveMon, dspFrame);
      pushMonitorRowToWaterfalls(&liveMon, blockIdx);
      // Redraw TFT every 2 blocks (320ms) when in FT8 LCD layout
      if (uiLayoutIdx == UI_FT8 && (blockIdx & 1) == 0) {
        needRedraw = true;
      }
    }
  }

  // Trigger live FT8 LDPC decoding at 84 blocks (13.44s) so the ~1.07s decode finishes at ~14.51s (before :00.0s of next slot)
  if (digActive && !slotDecoded) {
    if (liveMon.wf.num_blocks >= 84 || ((slotSec == 13 && subMs >= 440 || slotSec >= 14) && liveMon.wf.num_blocks >= 55)) {
      slotDecoded = true;
      int cnt = ft8DecodeFromMonitor(&liveMon, false);
      if (cnt > 0 && uiLayoutIdx == UI_FT8) {
        needRedraw = true;
      }
    } else if (slotSec >= 14) {
      slotDecoded = true;
    }
  }

  return needRedraw;
}


void ft8RunTestDecode() {
  if (!monInitialized) ft8Init();

  Serial.println("\r\n[FT8] Starting DSP decode (6000Hz, 93 blocks)...");
  ft8AddLog("[INFO] Decoding the WebSDR test vector (15s)...");
  ft8IsDecoding = true;
  drawScreen();

  monitor_reset(&liveMon);
  const int totalSamples = 90000;
  int numBlocks = totalSamples / FT8_BLOCK_SAMPLES; // 93 blocks

  for (int b = 0; b < numBlocks && b < liveMon.wf.max_blocks; b++) {
    const int16_t *src = &websdr_test1_pcm[b * FT8_BLOCK_SAMPLES];
    for (int i = 0; i < FT8_BLOCK_SAMPLES; i++) {
      int16_t sample = (int16_t)pgm_read_word(&src[i]);
      dspFrame[i] = (float)sample / 32768.0f;
    }
    int blockIdx = liveMon.wf.num_blocks;
    monitor_process(&liveMon, dspFrame);
    pushMonitorRowToWaterfalls(&liveMon, blockIdx);
  }

  int decoded = ft8DecodeFromMonitor(&liveMon, true);
  Serial.printf("[FT8] Cycle done: %d messages in %lu ms\r\n", decoded, (unsigned long)ft8LastDurationMs);

  // Reset monitor so next live ADC blocks continue cleanly
  monitor_reset(&liveMon);
  slotDecoded = false;
  drawScreen();
}

void ft8ForceLiveDecode() {
  if (!monInitialized) return;
  if (liveMon.wf.num_blocks >= 20) {
    ft8DecodeFromMonitor(&liveMon, false);
  }
}

void ft8ToggleViewMode() {
  ft8ViewMode = (ft8ViewMode + 1) % 3;
}

void ft8SetViewMode(uint8_t mode) {
  ft8ViewMode = mode % 3;
}

uint8_t ft8GetViewMode() {
  return ft8ViewMode;
}

void ft8SetDigitalMode(uint8_t digMode) {
  if (digMode > 2) digMode = FT8_DIG_OFF;
  // FT8/FT4 run the SI4732 loud for the ADC, so silence the speaker when entering them and restore it when leaving
  if (digMode != ft8DigitalMode) ft8SpeakerMuted = (digMode != FT8_DIG_OFF);
  ft8DigitalMode = digMode;
  if (monInitialized) monitor_reset(&liveMon);
  slotDecoded = false;
  ft8ApplyAudio();
}

uint8_t ft8GetDigitalMode() {
  if (currentMode != USB) return FT8_DIG_OFF;
  return ft8DigitalMode;
}

const char* ft8GetDisplayModeDesc() {
  if (currentMode == USB) {
    if (ft8DigitalMode == FT8_DIG_FT8) return "FT8";
    if (ft8DigitalMode == FT8_DIG_FT4) return "FT4";
    if (bands[bandIdx].bandwidthIdx == 0) return "CW";
  }
  return bandModeDesc[currentMode];
}

void ft8CheckFrequencyDigitalMode(uint16_t freqKhz, uint8_t mode) {
  static const uint16_t ft8Khz[9] = { 1840, 3573, 7074, 10136, 14074, 18100, 21074, 24915, 28074 };
  static const uint16_t ft4Khz[8] = { 3575, 7047, 10140, 14080, 18104, 21140, 24919, 28180 };
  if (mode == USB) {
    for (int i = 0; i < 9; i++) {
      if (freqKhz == ft8Khz[i]) {
        SsbAgcIdx = 0;
        SsbAvcIdx = 84;
        SsbSoftMuteIdx = 0;
        bands[bandIdx].bandwidthIdx = 5; // 4.0k wide flat passband for FT8
        doAgc(0);
        doAvc(0);
        doBandwidth(0);
        ft8SetDigitalMode(FT8_DIG_FT8);
        uiLayoutIdx = UI_FT8;
        return;
      }
    }
    for (int i = 0; i < 8; i++) {
      if (freqKhz == ft4Khz[i]) {
        SsbAgcIdx = 0;
        SsbAvcIdx = 84;
        SsbSoftMuteIdx = 0;
        bands[bandIdx].bandwidthIdx = 5; // 4.0k wide flat passband for FT4
        doAgc(0);
        doAvc(0);
        doBandwidth(0);
        ft8SetDigitalMode(FT8_DIG_FT4);
        uiLayoutIdx = UI_FT8;
        return;
      }
    }
  }
  ft8SetDigitalMode(FT8_DIG_OFF);
  if (uiLayoutIdx == UI_FT8) {
    uiLayoutIdx = UI_DEFAULT;
  }
}

void ft8TuneBandDigitalFreq(int bId, uint8_t digMode) {
  static const uint16_t ft8Khz[9] = { 1840, 3573, 7074, 10136, 14074, 18100, 21074, 24915, 28074 };
  static const uint16_t ft4Khz[9] = { 1840, 3575, 7047, 10140, 14080, 18104, 21140, 24919, 28180 };
  static const int16_t  ft4Bfo[9] = {    0,  500,  500,     0,     0,     0,     0,     0,     0 };
  if (bId < 18 || bId > 26) {
    bId = 24; // Default 15M (21.074 MHz) amateur band
  }
  int idx = bId - 18;
  uint16_t khz = (digMode == FT8_DIG_FT4) ? ft4Khz[idx] : ft8Khz[idx];
  int16_t  bfo = (digMode == FT8_DIG_FT4) ? ft4Bfo[idx] : 0;
  SsbAgcIdx = 0;
  SsbAvcIdx = 84;
  SsbSoftMuteIdx = 0;
  ft8SetDigitalMode(digMode);
  uiLayoutIdx = UI_FT8;
  bandIdx = (uint8_t)bId;
  bands[bId].bandMode = USB;
  bands[bId].bandwidthIdx = 5; // 4.0k wide flat passband for FT8/FT4
  bands[bId].currentFreq = khz;
  selectBand((uint8_t)bId, false);
  updateFrequency(khz, false);
  updateBFO(bfo, false);
}

void ft8ToggleSpeakerMute() {
  ft8SpeakerMuted = !ft8SpeakerMuted;
  ft8ApplyAudio();
}

// Single place that decides SI4732 volume and speaker amplifier state.
// FT8/FT4 need a fixed, high SI4732 line level on GPIO34 (volume <= 36 is below the ADC noise floor),
// so in digital modes the speaker is silenced with the amplifier instead of the SI4732 volume.
// The speaker and GPIO34 share the SI4732 line out: with the speaker on, the user volume wins
// (decoding is best near ft8AdcVolume); with it off, the ADC level is used.
void ft8ApplyAudio() {
  bool digital = ft8GetDigitalMode() != FT8_DIG_OFF;
  bool speaker = !sleepOn() && !muteOn(MUTE_MAIN) && !muteOn(MUTE_SQUELCH) && !ft8SpeakerMuted;

  // The SI4732 keeps producing audio for the ADC whatever the speaker does; MUTE_MAIN/SQUELCH are handled by muteOn()
  if (!muteOn(MUTE_MAIN) && !muteOn(MUTE_SQUELCH)) {
    rx.setVolume(digital && !speaker ? ft8AdcVolume : volume);
    rx.setAudioMute(false);
  }
#if PIN_AMP_EN >= 0
  digitalWrite(PIN_AMP_EN, speaker ? HIGH : LOW);
#endif
}

void ft8SetAdcVolume(uint8_t v) {
  ft8AdcVolume = v > 63 ? 63 : v;
  ft8ApplyAudio();
}

bool ft8IsSpeakerMuted() {
  return ft8SpeakerMuted;
}

bool ft8ToggleAdcTimer() {
  if (!adcTimer) return false;
  if (adcRunning) { adcRunning = false; timerStop(adcTimer); }
  else { timerStart(adcTimer); adcRunning = true; }
  return adcRunning;
}

void ft8ToggleAudioStream() {
  ft8AudioStreamOn = !ft8AudioStreamOn;
  Serial.flush();
  Serial.updateBaudRate(ft8AudioStreamOn ? 921600 : 115200);
  ft8AudioStreamSeq = 0;
}

void ft8ClearTraffic() {
  ft8TrafficCount = 0;
  ft8AddLog("[INFO] FT8 message list cleared");
}

uint8_t ft8GetAudioLevel() { return ft8AudioLevel; }
bool ft8IsDecodingNow() { return ft8IsDecoding; }
uint32_t ft8GetLastDurationMs() { return ft8LastDurationMs; }
uint32_t ft8GetTotalDecoded() { return ft8TotalDecoded; }
int ft8GetTrafficCount() { return ft8TrafficCount; }
const FT8MessageEntry* ft8GetTrafficEntry(int idx) {
  if (idx < 0 || idx >= ft8TrafficCount) return NULL;
  return &ft8Traffic[idx];
}
const uint8_t* ft8GetWebWaterfallBuffer() { return webWf; }

// Stream WAV audio (960 samples @ 6000 Hz, 16-bit PCM mono = 1964 bytes total) in chunks of any size
size_t ft8FillWavChunk(uint8_t *buf, size_t maxLen, size_t index) {
  const uint32_t numSamples = FT8_BLOCK_SAMPLES;
  const uint32_t dataBytes = numSamples * 2;
  const uint32_t totalSize = 44 + dataBytes;
  if (index >= totalSize) return 0;

  size_t written = 0;
  if (index < 44) {
    uint8_t hdr[44];
    const uint32_t riffSize = 36 + dataBytes;
    const uint32_t sampleRate = FT8_SAMPLE_RATE;
    const uint32_t byteRate = sampleRate * 2;
    const uint16_t blockAlign = 2;
    const uint16_t bitsPerSample = 16;
    const uint16_t audioFormat = 1;
    const uint16_t numChannels = 1;
    const uint32_t fmtSize = 16;

    memcpy(hdr + 0, "RIFF", 4);
    memcpy(hdr + 4, &riffSize, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    memcpy(hdr + 16, &fmtSize, 4);
    memcpy(hdr + 20, &audioFormat, 2);
    memcpy(hdr + 22, &numChannels, 2);
    memcpy(hdr + 24, &sampleRate, 4);
    memcpy(hdr + 28, &byteRate, 4);
    memcpy(hdr + 32, &blockAlign, 2);
    memcpy(hdr + 34, &bitsPerSample, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &dataBytes, 4);

    size_t hdrCopy = 44 - index;
    if (hdrCopy > maxLen) hdrCopy = maxLen;
    memcpy(buf, hdr + index, hdrCopy);
    written += hdrCopy;
    index += hdrCopy;
  }

  uint8_t rBuf = readyBufIdx;
  int16_t mean = (int16_t)lastAdcMean;
  while (written + 2 <= maxLen && index < totalSize) {
    uint32_t sampleIdx = (index - 44) / 2;
    int32_t s = ((int32_t)adcBuf[rBuf][sampleIdx] - mean) * 32;
    if (s < -32767) s = -32767;
    if (s > 32767) s = 32767;
    int16_t pcm = (int16_t)s;
    memcpy(buf + written, &pcm, 2);
    written += 2;
    index += 2;
  }
  return written;
}

void ft8TestAudioRouting() {
  Serial.println("\r\n=== TESTING VOLUME vs WATERFALL & ADC RESPONSE (1.0s per level) ===");
  uint8_t savedVol = volume;
  uint8_t testVols[3] = {0, 20, 36};
  for (int i = 0; i < 3; i++) {
    uint8_t v = testVols[i];
    if (v == 0) {
      rx.setAudioMute(true);
      rx.setVolume(0);
    } else {
      rx.setAudioMute(false);
      rx.setVolume(v);
    }
    delay(250);
    while (adcReadBlock != adcWriteBlock) ft8TickWaterfall();
    int peakVpp = 0, peakAvgMag = 0, peakMaxMag = 0;
    float peakRms = 0.0f;
    int bestAct20 = 0, bestAct100 = 0, bestMaxWf = 0;
    for (int f = 0; f < 5; f++) {
      delay(175);
      while (adcReadBlock != adcWriteBlock) ft8TickWaterfall();
      if (lastAdcVpp > peakVpp) peakVpp = lastAdcVpp;
      if (lastBlockRms > peakRms) peakRms = lastBlockRms;
      if (g_lastRowAvgMag > peakAvgMag) peakAvgMag = g_lastRowAvgMag;
      if (g_lastRowMaxMag > peakMaxMag) peakMaxMag = g_lastRowMaxMag;
      int act20 = 0, act100 = 0, maxWf = 0;
      for (int c = 0; c < 120; c++) {
        if (webWf[c] > 20) act20++;
        if (webWf[c] > 100) act100++;
        if (webWf[c] > maxWf) maxWf = webWf[c];
      }
      if (act20 > bestAct20) bestAct20 = act20;
      if (act100 > bestAct100) bestAct100 = act100;
      if (maxWf > bestMaxWf) bestMaxWf = maxWf;
    }
    Serial.printf("  Vol=%2u (%s) -> Peak Vpp=%4d, RMS=%5.1f | FFT avgMag=%3d, max=%3d (floor=%.1f) | WF row0: >20=%2d/120, >100=%2d/120, max=%3d\r\n",
                  v, v == 0 ? "MUTED" : "AUDIO", peakVpp, peakRms,
                  peakAvgMag, peakMaxMag, g_absNoiseFloor,
                  bestAct20, bestAct100, bestMaxWf);
  }
  volume = savedVol;
  rx.setAudioMute(false);
  rx.setVolume(volume);
  Serial.println("=== VOLUME vs WATERFALL TEST COMPLETE ===");
}


void ft8PrintDiag() {
  rx.getCurrentReceivedSignalQuality();
  uint8_t curRssi = rx.getCurrentRSSI();
  uint8_t curSnr = rx.getCurrentSNR();
  float biasV = (lastAdcMean * 0.95f) / 4095.0f;
  float acVpp = (lastAdcVpp * 0.95f) / 4095.0f;

  int wfAct20 = 0, wfAct100 = 0, wfMax = 0;
  for (int i = 0; i < 120 * 20; i++) {
    if (webWf[i] > 20) wfAct20++;
    if (webWf[i] > 100) wfAct100++;
    if (webWf[i] > wfMax) wfMax = webWf[i];
  }

  Serial.println("\r\n========== HARDWARE & SIGNAL DIAGNOSTICS ==========");
  Serial.printf("Si4732 Radio : %s %u kHz (BFO: %d Hz, Cal: %d Hz, BW: %s, PatchID: 0x%04X)\r\n",
                bandModeDesc[currentMode], currentFrequency, currentBFO,
                currentMode == USB ? getCurrentBand()->usbCal : getCurrentBand()->lsbCal,
                getCurrentBandwidth()->desc, rx.getPatchId());
  Serial.printf("Si4732 Signal: RSSI = %u dBuV | SNR = %u dB | RF_AGC = %s | SSB_AVC = %d dB (Max) | Speaker_Vol = %u/63\r\n",
                curRssi, curSnr, disableAgc ? "ATTN" : "ON(MaxSens)", isSSB() ? SsbAvcIdx : AmAvcIdx, volume);
  Serial.printf("Speaker Audio: %s | GPIO34 ADC Atten: 0dB (0..0.95V Max Hardware Sensitivity)\r\n", ft8SpeakerMuted ? "WHISPER(10/63)" : "QUIET(20/63)");
  Serial.printf("ADC_GPIO34(raw): Avg=%.0f (Bias: %.2fV) | Min=%d Max=%d | Vpp=%d (AC_Vpp: %.3fV, RMS=%.1f, Drop=%lu)\r\n",
                lastAdcMean, biasV, lastAdcMin, lastAdcMax, lastAdcVpp, acVpp, lastBlockRms, (unsigned long)adcDroppedBlocks);
  Serial.printf("Waterfall FFT: BW=200..2900Hz (%d bins) | avgMag=%d (absFloor=%.1f, min=%d, max=%d) | WF(20rows): >20=%d%%, >100=%d%%, max=%d\r\n",
                liveMon.wf.num_bins, g_lastRowAvgMag, g_absNoiseFloor, g_lastRowMinMag, g_lastRowMaxMag,
                (wfAct20 * 100) / 2400, (wfAct100 * 100) / 2400, wfMax);
  if (liveMon.wf.mag && liveMon.wf.num_blocks > 0 && liveMon.wf.num_bins > 0) {
    int nBlk = liveMon.wf.num_blocks;
    int nBin = liveMon.wf.num_bins;
    int stride = liveMon.wf.block_stride;
    uint16_t peakBins[5] = {0};
    int peakDiffs[5] = {0};
    for (int b = 2; b < nBin - 2; b++) {
      int sumB = 0, maxB = 0;
      for (int r = 0; r < nBlk; r++) {
        uint8_t m = wf_read_u8(&liveMon.wf.mag[r * stride + b]);
        sumB += m;
        if (m > maxB) maxB = m;
      }
      int avgB = sumB / nBlk;
      int diff = maxB - g_lastRowAvgMag;
      int avgDiff = avgB - g_lastRowAvgMag;
      int score = diff + avgDiff * 2;
      for (int k = 0; k < 5; k++) {
        // Keep peaks separated by at least 4 bins (25 Hz)
        if (abs(b - (int)peakBins[k]) < 4 && peakDiffs[k] > 0) {
          if (score > peakDiffs[k]) { peakDiffs[k] = score; peakBins[k] = b; }
          break;
        }
        if (score > peakDiffs[k]) {
          for (int j = 4; j > k; j--) { peakDiffs[j] = peakDiffs[j - 1]; peakBins[j] = peakBins[j - 1]; }
          peakDiffs[k] = score;
          peakBins[k] = b;
          break;
        }
      }
    }
    Serial.printf("Passband Peaks: 1:%uHz(%+.1fdB) 2:%uHz(%+.1fdB) 3:%uHz(%+.1fdB) 4:%uHz(%+.1fdB) 5:%uHz(%+.1fdB)\r\n",
                  (unsigned int)roundf((liveMon.min_bin + peakBins[0]) * 6.25f), peakDiffs[0] * 0.5f,
                  (unsigned int)roundf((liveMon.min_bin + peakBins[1]) * 6.25f), peakDiffs[1] * 0.5f,
                  (unsigned int)roundf((liveMon.min_bin + peakBins[2]) * 6.25f), peakDiffs[2] * 0.5f,
                  (unsigned int)roundf((liveMon.min_bin + peakBins[3]) * 6.25f), peakDiffs[3] * 0.5f,
                  (unsigned int)roundf((liveMon.min_bin + peakBins[4]) * 6.25f), peakDiffs[4] * 0.5f);
  }
  uint8_t utcH = 0, utcM = 0, utcS = 0;
  uint16_t utcMs = 0;
  ft8GetCurrentUTC(&utcH, &utcM, &utcS, &utcMs);
  Serial.printf("FT8 Live Slot: %02us/15s | Blocks=%d/%d | TotalDecoded=%lu | UTC=%02u:%02u:%02u.%03u\r\n",
                ft8GetCurrentSlotSecond(), liveMon.wf.num_blocks, liveMon.wf.max_blocks,
                (unsigned long)ft8TotalDecoded, utcH, utcM, utcS, utcMs);
  Serial.printf("Battery(GPIO35): %.2f V | Heap Free: %u bytes\r\n",
                batteryMonitor(), (unsigned int)ESP.getFreeHeap());
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi Status  : CONNECTED to '%s' | IP: %s | RSSI: %d dBm\r\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("WiFi Status  : AP Mode ('%s') | IP: %s\r\n",
                  RECEIVER_NAME, WiFi.softAPIP().toString().c_str());
  }
  Serial.println("===================================================");
}


// Convert 0..255 waterfall magnitude into 16-bit RGB565 heatmap color
static uint16_t wfHeatColor565(uint8_t val) {
  if (val < 25)  return 0x0003; // Deep Navy
  if (val < 70)  return 0x0113; // Dark Blue
  if (val < 125) return 0x043F; // Medium Blue
  if (val < 170) return 0x07FF; // Cyan
  if (val < 215) return 0xFFE0; // Yellow
  if (val < 245) return 0xF800; // Red
  return 0xFFFF;                // White
}

// Draw FT8 Layout on 320x170 Sprite
void drawLayoutFT8(const char *statusLine1, const char *statusLine2) {
  spr.fillSprite(TFT_BLACK);
  spr.setTextFont(0);

  // Top Header Bar (y = 0..20)
  spr.fillRect(0, 0, 320, 20, 0x10A2); // Dark slate header

  // Band & Frequency on left
  char freqBuf[36];
  if (currentMode == FM) {
    snprintf(freqBuf, sizeof(freqBuf), "%s %.2fM FM", getCurrentBand()->bandName, currentFrequency / 100.0f);
  } else {
    uint32_t fHz = (uint32_t)currentFrequency * 1000UL + currentBFO;
    snprintf(freqBuf, sizeof(freqBuf), "%s %lu.%02lu %s",
             getCurrentBand()->bandName, fHz / 1000UL, (fHz % 1000UL) / 10UL, ft8GetDisplayModeDesc());
  }
  spr.setTextDatum(ML_DATUM);
  spr.setTextColor(0x07FF, 0x10A2); // Cyan
  spr.drawString(freqBuf, 4, 10, 2);

  // Slot progress & UTC clock in middle (x = 216 so it never overlaps WiFi/Battery icons)
  uint8_t utcH = 0, utcM = 0, utcS = 0;
  ft8GetCurrentUTC(&utcH, &utcM, &utcS);
  uint8_t slotSec = utcS % 15;
  bool isDigMode = (ft8GetDigitalMode() != FT8_DIG_OFF);
  char timeBuf[24];
  if (isDigMode) {
    snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u:%02u [%02us]", utcH, utcM, utcS, slotSec);
  } else {
    uint8_t locH = (utcH + utcOffsets[utcOffsetIdx].offset / 4) % 24;
    snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u:%02u", locH, utcM, utcS);
  }
  spr.setTextDatum(MR_DATUM);
  spr.setTextColor(ft8IsDecoding ? 0xF81F : 0xFFE0, 0x10A2);
  spr.drawString(timeBuf, 216, 10, 1);

  // WiFi & Battery icons on far right
  drawWiFiIndicator(226, 2);
  drawBattery(288, 2);

  // 15s Slot Progress Bar (or S-Meter bar in Voice/FM/AM mode) + Audio VU bar (y = 20..24)
  int slotW = isDigMode ? ((slotSec * 200) / 15) : ((getStrength(rssi) * 200) / 15);
  if (slotW > 200) slotW = 200;
  spr.fillRect(0, 20, 200, 4, 0x2104);
  spr.fillRect(0, 20, slotW, 4, ft8IsDecoding ? 0xF81F : 0x07E0);

  int vuW = (ft8AudioLevel * 118) / 100;
  spr.fillRect(202, 20, 118, 4, 0x2104);
  spr.fillRect(202, 20, vuW, 4, ft8AudioLevel > 80 ? 0xF800 : 0x07FF);

  if (ft8ViewMode == FT8_VIEW_SPLIT) {
    // Left Half (x = 0..144): Live Audio FFT Spectrum (y = 26..55, 30px tall) + Scrolling Waterfall (y = 57..152)
    spr.drawRect(0, 25, 146, 128, 0x3186);
    spr.drawFastHLine(1, 56, 144, 0x4A69);
    for (int c = 0; c < 72; c++) {
      int idx0 = (c * LCD_WF_COLS) / 72;
      int idx1 = ((c + 1) * LCD_WF_COLS) / 72;
      uint8_t v = webWf[idx0];
      if (idx1 > idx0 + 1 && webWf[idx0 + 1] > v) v = webWf[idx0 + 1];
      int barH = (v * 29) / 255;
      if (barH > 0) {
        spr.fillRect(1 + c * 2, 56 - barH, 2, barH, wfHeatColor565(v));
        spr.fillRect(1 + c * 2, 56 - barH, 2, 1, 0xFFE0); // Bright yellow spectrum contour cap
      }
    }
    for (int r = 0; r < 32 && r < LCD_WF_ROWS; r++) {
      int y = 57 + r * 3;
      for (int c = 0; c < 72; c++) {
        uint8_t v = webWf[r * 120 + (c * LCD_WF_COLS) / 72];
        if (v > 5) {
          spr.fillRect(1 + c * 2, y, 2, 3, wfHeatColor565(v));
        }
      }
    }

    // Right Half (x = 148..319): Decoded FT8 Traffic Table (in FT8/FT4 mode) or Radio Parameters (in FM/AM/SSB Voice mode)
    spr.drawRect(148, 25, 172, 128, 0x3186);
    spr.setTextDatum(TL_DATUM);
    if (!isDigMode) {
      spr.setTextColor(0x07FF, TFT_BLACK);
      char hdrLine[32];
      snprintf(hdrLine, sizeof(hdrLine), "Che do: %s (%s)", ft8GetDisplayModeDesc(), getCurrentBand()->bandName);
      spr.drawString(hdrLine, 154, 30, 2);

      spr.setTextColor(0xFFFF, TFT_BLACK);
      char pLine1[36];
      snprintf(pLine1, sizeof(pLine1), "Step:%s  BW:%s", getCurrentStep()->desc, getCurrentBandwidth()->desc);
      spr.drawString(pLine1, 154, 50, 2);

      char pLine2[36];
      snprintf(pLine2, sizeof(pLine2), "AGC:%s  Vol:%u/63", agcIdx == 0 ? "On" : "Att", volume);
      spr.drawString(pLine2, 154, 70, 2);

      spr.setTextColor(0x07E0, TFT_BLACK);
      char sigBuf[32];
      snprintf(sigBuf, sizeof(sigBuf), "RSSI:%udBuV SNR:%udB", rssi, snr);
      spr.drawString(sigBuf, 154, 92, 2);

      String ipStr = (WiFi.status() == WL_CONNECTED) ? ("Web: " + WiFi.localIP().toString()) : "AP: 10.1.1.1";
      spr.setTextColor(0xFFE0, TFT_BLACK);
      spr.drawString(ipStr.c_str(), 154, 114, 2);

      spr.setTextColor(0xAD55, TFT_BLACK);
      spr.drawString("Pho am thanh 200..2900Hz", 154, 136, 1);
    } else if (ft8TrafficCount == 0) {
      spr.setTextColor(0xAD55, TFT_BLACK);
      spr.drawString(ft8GetDigitalMode() == FT8_DIG_FT4 ? "Dang thu FT4 (GPIO34)" : "Dang thu FT8 (GPIO34)", 154, 34, 2);
      char blkBuf[36];
      snprintf(blkBuf, sizeof(blkBuf), "Vol:%u | Vpp:%d | RMS:%.0f", volume, lastAdcVpp, lastBlockRms);
      spr.setTextColor(0x07FF, TFT_BLACK);
      spr.drawString(blkBuf, 154, 54, 1);
      if (WiFi.status() == WL_CONNECTED) {
        String ipStr = "Web: " + WiFi.localIP().toString();
        spr.setTextColor(0x07E0, TFT_BLACK);
        spr.drawString(ipStr.c_str(), 154, 72, 2);
      } else {
        spr.setTextColor(0xFFE0, TFT_BLACK);
        spr.drawString("AP: 10.1.1.1", 154, 72, 2);
      }
      spr.setTextColor(0x7BEF, TFT_BLACK);
      char sigBuf[32];
      snprintf(sigBuf, sizeof(sigBuf), "RSSI:%udBuV SNR:%udB", rssi, snr);
      spr.drawString(sigBuf, 154, 94, 2);
      spr.setTextColor(0xFFE0, TFT_BLACK);
      spr.drawString("BW: 200..2900Hz (SSB)", 154, 118, 1);
    } else {
      int maxRows = ft8TrafficCount < 7 ? ft8TrafficCount : 7;
      for (int i = 0; i < maxRows; i++) {
        const FT8MessageEntry &e = ft8Traffic[i];
        int ry = 28 + i * 17;
        uint16_t color = e.isCQ ? 0x07E0 : 0xFFFF; // Green for CQ, White for QSO
        spr.setTextColor(color, TFT_BLACK);
        char line[32];
        snprintf(line, sizeof(line), "%+3d %-7s %.12s", e.snr, e.call, e.msg);
        spr.drawString(line, 151, ry, 2);
      }
    }
  } else if (ft8ViewMode == FT8_VIEW_WATERFALL) {
    // Top strip (y = 26..58): Real-Time Live Audio FFT Spectrum Bars (150..2550 Hz)
    spr.drawFastHLine(0, 59, 320, 0x4A69);
    for (int c = 0; c < LCD_WF_COLS; c++) {
      uint8_t v = webWf[c];
      int barH = (v * 32) / 255;
      if (barH > 0) {
        int x = (c * 320) / LCD_WF_COLS;
        int w = ((c + 1) * 320) / LCD_WF_COLS - x;
        spr.fillRect(x, 58 - barH, w > 0 ? w : 1, barH, wfHeatColor565(v));
        spr.fillRect(x, 58 - barH, w > 0 ? w : 1, 1, 0xFFE0);
      }
    }
    // Frequency scale ticks on spectrum divider (waterfall spans 200..2900 Hz)
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(0x7BEF, TFT_BLACK);
    spr.drawString("200Hz", 2, 26, 1);
    spr.drawString("1.1k", 104, 26, 1);
    spr.drawString("2.0k", 210, 26, 1);
    spr.drawString("2.9kHz", 280, 26, 1);

    // Bottom area (y = 60..152): Scrolling 2D Waterfall (31 rows x 3px)
    for (int r = 0; r < 31 && r < LCD_WF_ROWS; r++) {
      int y = 60 + r * 3;
      for (int c = 0; c < LCD_WF_COLS; c++) {
        uint8_t v = webWf[r * 120 + c];
        if (v > 5) {
          int x = (c * 320) / LCD_WF_COLS;
          int w = ((c + 1) * 320) / LCD_WF_COLS - x;
          spr.fillRect(x, y, w > 0 ? w : 1, 3, wfHeatColor565(v));
        }
      }
    }
  } else {
    // Full-screen Decoded Log Table (x = 0..319, y = 26..152)
    spr.setTextDatum(TL_DATUM);
    if (ft8TrafficCount == 0) {
      spr.setTextColor(0xAD55, TFT_BLACK);
      spr.drawString("Dang lang nghe goi tin FT8 tren anten...", 12, 65, 2);
    } else {
      int maxRows = ft8TrafficCount < 7 ? ft8TrafficCount : 7;
      for (int i = 0; i < maxRows; i++) {
        const FT8MessageEntry &e = ft8Traffic[i];
        int ry = 27 + i * 17;
        spr.setTextColor(e.isCQ ? 0x07E0 : 0xFFFF, TFT_BLACK);
        char line[48];
        snprintf(line, sizeof(line), "%s %+3ddB %4uHz %-8s %s",
                 e.timeStr + 3, e.snr, e.freq, e.call, e.msg);
        spr.drawString(line, 4, ry, 2);
      }
    }
  }

  // Bottom Status Bar (y = 154..169)
  spr.fillRect(0, 154, 320, 16, 0x10A2);
  spr.setTextDatum(ML_DATUM);
  spr.setTextColor(0xBDF7, 0x10A2);
  if (statusLine1 && statusLine1[0]) {
    spr.drawString(statusLine1, 4, 162, 2);
  } else {
    char botBuf[64];
    String ipDisp = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    snprintf(botBuf, sizeof(botBuf), "IP:%s | Vol:%u | Vpp:%d | RSSI:%u",
             ipDisp.c_str(), volume, lastAdcVpp, rssi);
    spr.drawString(botBuf, 4, 162, 2);
  }

  // Draw sidebar menu if a menu/command is active
  if (currentCmd != CMD_NONE) {
    drawSideBar(currentCmd, MENU_OFFSET_X, MENU_OFFSET_Y, MENU_DELTA_X);
  }
}
