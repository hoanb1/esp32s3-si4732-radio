#ifndef LAYOUT_FT8_H
#define LAYOUT_FT8_H

#include "Common.h"
#include <Arduino.h>

// LCD View Modes in FT8 Layout
#define FT8_VIEW_SPLIT      0
#define FT8_VIEW_WATERFALL  1
#define FT8_VIEW_LOG        2

// Digital Decoder Modes (0=FT8 15s, 1=FT4 7.5s, 2=OFF for FM/AM/SSB Voice)
#define FT8_DIG_FT8         0
#define FT8_DIG_FT4         1
#define FT8_DIG_OFF         2

struct FT8MessageEntry {
  char timeStr[12];
  int8_t snr;
  float dt;
  uint16_t freq;
  char call[16];
  char msg[40];
  bool isCQ;
};

#define FT8_MAX_TRAFFIC 16
#define FT8_MAX_LOGS    10

// Initialize FT8 DSP monitor & ADC sampling on GPIO34
void ft8Init();

// Called from main loop() to process completed 160ms ADC blocks and run automatic 15s slot decode
bool ft8TickWaterfall();

// Draw FT8 UI layout on the 320x170 sprite
void drawLayoutFT8(const char *statusLine1 = 0, const char *statusLine2 = 0);

// Trigger test decode using internal 15s WebSDR PCM vector (websdr_test1_pcm)
void ft8RunTestDecode();
void ft8RequestTestDecode();
void ft8RequestRedraw();

// Trigger immediate decode on current live waterfall
void ft8ForceLiveDecode();

// Synchronize 15s slot timer to :00s manually
void ft8SyncSlotManual();

// Set UTC clock from Unix epoch (used by Web UI /api/timesync)
void ft8SetEpochTime(uint32_t epoch, uint16_t ms = 0);

// Toggle or set LCD view mode (0=SPLIT, 1=WATERFALL, 2=LOG)
void ft8ToggleViewMode();
void ft8SetViewMode(uint8_t mode);
uint8_t ft8GetViewMode();

// Digital Decoder Mode (0=FT8, 1=FT4, 2=OFF)
void ft8SetDigitalMode(uint8_t digMode);
uint8_t ft8GetDigitalMode();
const char* ft8GetDisplayModeDesc();
void ft8CheckFrequencyDigitalMode(uint16_t freqKhz, uint8_t mode);
void ft8TuneBandDigitalFreq(int bId, uint8_t digMode);

// Speaker mute control (mutes speaker without killing SI4732 baseband output to GPIO34 if in FT8 mode)
void ft8ToggleSpeakerMute();
void ft8SetSpeakerMuted(bool muted);
bool ft8IsSpeakerMuted();

// Amateur bands with FT8/FT4 frequencies (indices into bands[]) and the one closest to a frequency
#define FT8_FIRST_BAND 18
#define FT8_LAST_BAND  26
int ft8NearestBand(uint32_t khz);

// Apply SI4732 volume + speaker amplifier state for the current mode/mute/sleep state
void ft8ApplyAudio();
void ft8SetAdcVolume(uint8_t v);

// Clear decoded traffic list
void ft8ClearTraffic();

// Toggle raw ADC audio streaming over Serial (switches baud to 921600 while active)
void ft8ToggleAudioStream();
bool ft8ToggleAdcTimer();

// Print hardware/DSP diagnostic summary to Serial ('d' command)
void ft8PrintDiag();
void ft8TestAudioRouting();

// Getters for Web UI endpoints
uint8_t ft8GetCurrentSlotSecond();
bool ft8IsDecodingNow();
uint32_t ft8GetLastDurationMs();
uint8_t ft8GetAudioLevel();
uint32_t ft8GetTotalDecoded();
int ft8GetTrafficCount();
const FT8MessageEntry* ft8GetTrafficEntry(int idx);
const uint8_t* ft8GetWebWaterfallBuffer(); // 93 * 120 bytes
uint32_t ft8GetWebWaterfallRowCount(); // rows added to the web waterfall since boot (row 0 is the newest)
// Full-resolution waterfall rows (4-bit packed, ft8GetHiResBins() bins); only the last 8 rows are kept
#define FT8_HIRES_ROWS 8
uint16_t ft8GetHiResBins();
const uint8_t *ft8GetHiResRow(uint32_t rowNo);
const uint8_t *ft8GetHiResMeta(uint32_t rowNo);   // [block index in FT8 slot or 255, slot of the hour 0..239]
void ft8AddLog(const char *fmt, ...);
String ft8GetLogsJson();
size_t ft8FillWavChunk(uint8_t *buf, size_t maxLen, size_t index); // WAV audio stream from live ADC ring buffer
uint32_t ft8WebAudioBegin(uint32_t from, uint32_t *count);  // start sample number and length of the next /api/audio chunk
size_t ft8WebAudioCopy(uint8_t *dst, uint32_t seq, size_t n);  // copy mu-law samples seq..seq+n-1
bool ft8WebAudioActive();  // true while web audio has paused FT8 decoding

#endif // LAYOUT_FT8_H
