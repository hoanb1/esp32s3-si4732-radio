#include <WiFi.h>
#include "Common.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "Layout-FT8.h"

static uint32_t remoteTimer = millis();
static uint8_t remoteSeqnum = 0;
static bool remoteLogOn = false;

static uint8_t char2nibble(char key)
{
  if((key >= '0') && (key <= '9')) return(key - '0');
  if((key >= 'A') && (key <= 'F')) return(key - 'A' + 10);
  if((key >= 'a') && (key <= 'f')) return(key - 'a' + 10);
  return(0);
}

//
// Capture current screen image to the remote
//
static void remoteCaptureScreen()
{
  uint16_t width  = spr.width();
  uint16_t height = spr.height();

  // 14 bytes of BMP header
  Serial.println("");
  Serial.print("424d"); // BM
  // Image size
  Serial.printf("%08x", (unsigned int)htonl(14 + 40 + 12 + width * height * 2));
  Serial.print("00000000");
  // Offset to image data
  Serial.printf("%08x", (unsigned int)htonl(14 + 40 + 12));
  // Image header
  Serial.print("28000000"); // Header size
  Serial.printf("%08x", (unsigned int)htonl(width));
  Serial.printf("%08x", (unsigned int)htonl(height));
  Serial.print("01001000"); // 1 plane, 16 bpp
  Serial.print("03000000"); // Compression
  Serial.print("00000000"); // Compressed image size
  Serial.print("00000000"); // X res
  Serial.print("00000000"); // Y res
  Serial.print("00000000"); // Color map
  Serial.print("00000000"); // Colors
  Serial.print("00f80000"); // Red mask
  Serial.print("e0070000"); // Green mask
  Serial.println("1f000000"); // Blue mask

  // Image data
  for(int y=height-1 ; y>=0 ; y--)
  {
    for(int x=0 ; x<width ; x++)
    {
      Serial.printf("%04x", htons(spr.readPixel(x, y)));
    }
    Serial.println("");
  }
}

char readSerialChar()
{
  char key;

  while (!Serial.available());
  key = Serial.read();
  Serial.print(key);
  return key;
}

long int readSerialInteger()
{
  long int result = 0;
  while (true) {
    char ch = Serial.peek();
    if (ch == 0xFF) {
      continue;
    } else if ((ch >= '0') && (ch <= '9')) {
      ch = readSerialChar();
      // Can overflow, but it's ok
      result = result * 10 + (ch - '0');
    } else {
      return result;
    }
  }
}

void readSerialString(char *bufStr, uint8_t bufLen)
{
  uint8_t length = 0;
  while (true) {
    char ch = Serial.peek();
    if (ch == 0xFF) {
      continue;
    } else if (ch == ',' || ch < ' ') {
      bufStr[length] = '\0';
      return;
    } else {
      ch = readSerialChar();
      bufStr[length] = ch;
      if (++length >= bufLen - 1) {
        bufStr[length] = '\0';
        return;
      }
    }
  }
}

static bool expectNewline()
{
  char ch;
  while ((ch = Serial.peek()) == 0xFF);
  if (ch == '\r') {
    Serial.read();
    return true;
  }
  return false;
}

static bool showError(const char *message)
{
  // Consume the remaining input
  while (Serial.available()) readSerialChar();
  Serial.printf("\r\nError: %s\r\n", message);
  return false;
}

static void remoteGetMemories()
{
  for (uint8_t i = 0; i < getTotalMemories(); i++) {
    if (memories[i].freq) {
      Serial.printf("#%02d,%s,%ld,%s\r\n", i + 1, bands[memories[i].band].bandName, memories[i].freq, bandModeDesc[memories[i].mode]);
    }
  }
}


static bool remoteSetMemory()
{
  Serial.print('#');
  Memory mem;
  uint32_t freq = 0;

  long int slot = readSerialInteger();
  if (readSerialChar() != ',')
    return showError("Expected ','");
  if (slot < 1 || slot > getTotalMemories())
    return showError("Invalid memory slot number");

  char band[8];
  readSerialString(band, 8);
  if (readSerialChar() != ',')
    return showError("Expected ','");
  mem.band = 0xFF;
  for (int i = 0; i < getTotalBands(); i++) {
    if (strcmp(bands[i].bandName, band) == 0) {
      mem.band = i;
      break;
    }
  }
  if (mem.band == 0xFF)
    return showError("No such band");

  freq = readSerialInteger();
  if (readSerialChar() != ',')
    return showError("Expected ','");

  char mode[4];
  readSerialString(mode, 4);
  if (!expectNewline())
    return showError("Expected newline");
  Serial.println();
  mem.mode = 15;
  for (int i = 0; i < getTotalModes(); i++) {
    if (strcmp(bandModeDesc[i], mode) == 0) {
      mem.mode = i;
      break;
    }
  }
  if (mem.mode == 15)
    return showError("No such mode");

  mem.freq = freq;

  if (!isMemoryInBand(&bands[mem.band], &mem)) {
    if (!freq) {
      // Clear slot
      memories[slot-1] = mem;
      return true;
    } else {
      // Handle duplicate band names (15M)
      mem.band = 0xFF;
      for (int i = getTotalBands()-1; i >= 0; i--) {
        if (strcmp(bands[i].bandName, band) == 0) {
          mem.band = i;
          break;
        }
      }
      if (mem.band == 0xFF)
        return showError("No such band");
      if (!isMemoryInBand(&bands[mem.band], &mem))
        return showError("Invalid frequency or mode");
    }
  }

  memories[slot-1] = mem;
  return true;
}

//
// Set current color theme from the remote
//
static void remoteSetColorTheme()
{
  Serial.print("Enter a string of hex colors (x0001x0002...): ");

  uint8_t *p = (uint8_t *)&(TH.bg);

  for(int i=0 ; ; i+=sizeof(uint16_t))
  {
    if(i >= sizeof(ColorTheme)-offsetof(ColorTheme, bg))
    {
      Serial.println(" Ok");
      break;
    }

    if(readSerialChar() != 'x')
    {
      Serial.println(" Err");
      break;
    }

    p[i + 1]  = char2nibble(readSerialChar()) * 16;
    p[i + 1] |= char2nibble(readSerialChar());
    p[i]      = char2nibble(readSerialChar()) * 16;
    p[i]     |= char2nibble(readSerialChar());
  }

  // Redraw screen
  drawScreen();
}

//
// Print current color theme to the remote
//
static void remoteGetColorTheme()
{
  Serial.printf("Color theme %s: ", TH.name);
  const uint8_t *p = (uint8_t *)&(TH.bg);

  for(int i=0 ; i<sizeof(ColorTheme)-offsetof(ColorTheme, bg) ; i+=sizeof(uint16_t))
  {
    Serial.printf("x%02X%02X", p[i+1], p[i]);
  }

  Serial.println();
}

//
// Print current status to the remote
//
void remotePrintStatus()
{
  // Prepare information ready to be sent
  float remoteVoltage = batteryMonitor();

  // S-Meter conditional on compile option
  rx.getCurrentReceivedSignalQuality();
  uint8_t remoteRssi = rx.getCurrentRSSI();
  uint8_t remoteSnr = rx.getCurrentSNR();

  // Use rx.getFrequency to force read of capacitor value from SI4732/5
  rx.getFrequency();
  uint16_t tuningCapacitor = rx.getAntennaTuningCapacitor();

  // Remote serial
  Serial.printf("%u,%u,%d,%d,%s,%s,%s,%s,%hu,%hu,%hu,%hu,%hu,%.2f,%hu\r\n",
                VER_APP,
                currentFrequency,
                currentBFO,
                ((currentMode == USB) ? getCurrentBand()->usbCal :
                 (currentMode == LSB) ? getCurrentBand()->lsbCal : 0),
                getCurrentBand()->bandName,
                bandModeDesc[currentMode],
                getCurrentStep()->desc,
                getCurrentBandwidth()->desc,
                agcIdx,
                volume,
                remoteRssi,
                remoteSnr,
                tuningCapacitor,
                remoteVoltage,
                remoteSeqnum
                );
}

//
// Tick remote time, periodically printing status
//
void remoteTickTime()
{
  if(remoteLogOn && (millis() - remoteTimer >= 500))
  {
    // Mark time and increment diagnostic sequence number
    remoteTimer = millis();
    remoteSeqnum++;
    // Show status
    remotePrintStatus();
  }
}

//
// Recognize and execute given remote command
//
int remoteDoCommand(char key)
{
  int event = 0;

  switch(key)
  {
    case 'R': // Rotate Encoder Clockwise
      event |= 1 << REMOTE_DIRECTION;
      event |= REMOTE_PREFS;
      break;
    case 'r': // Rotate Encoder Counterclockwise
      event |= -1 << REMOTE_DIRECTION;
      event |= REMOTE_PREFS;
      break;
    case 'e': // Encoder Push Button
      event |= REMOTE_CLICK;
      break;
    case 'B': // Band Up
      doBand(1);
      event |= REMOTE_PREFS;
      break;
    case 'b': // Band Down
      doBand(-1);
      event |= REMOTE_PREFS;
      break;
    case 'M': // Mode Up
      doMode(1);
      event |= REMOTE_PREFS;
      break;
    case 'm': // Mode Down
      doMode(-1);
      event |= REMOTE_PREFS;
      break;
    case 'S': // Step Up
      doStep(1);
      event |= REMOTE_PREFS;
      break;
    case 's': // Step Down
      doStep(-1);
      event |= REMOTE_PREFS;
      break;
    case 'W': // Bandwidth Up
      doBandwidth(1);
      event |= REMOTE_PREFS;
      break;
    case 'w': // Bandwidth Down
      doBandwidth(-1);
      event |= REMOTE_PREFS;
      break;
    case 'A': // AGC/ATTN Up
      doAgc(1);
      event |= REMOTE_PREFS;
      break;
    case 'a': // AGC/ATTN Down
      doAgc(-1);
      event |= REMOTE_PREFS;
      break;
    case 'V': // Volume Up
      doVolume(1);
      event |= REMOTE_PREFS;
      break;
    case 'v': // Volume Down
      doVolume(-1);
      event |= REMOTE_PREFS;
      break;
    case 'L': // Backlight Up
      doBrt(1);
      event |= REMOTE_PREFS;
      break;
    case 'l': // Backlight Down
      doBrt(-1);
      event |= REMOTE_PREFS;
      break;
    case 'O':
      sleepOn(true);
      break;
    case 'o':
      sleepOn(false);
      break;
    case 'I':
      doCal(1);
      event |= REMOTE_PREFS;
      break;
    case 'i':
      doCal(-1);
      event |= REMOTE_PREFS;
      break;
    case 'C':
      remoteLogOn = false;
      remoteCaptureScreen();
      break;
    case 't':
      remoteLogOn = !remoteLogOn;
      break;

    case '$':
      remoteGetMemories();
      break;
    case '#':
      if (remoteSetMemory())
        event |= REMOTE_PREFS;
      break;

    case 'T':
    {
      delay(15);
      uint32_t ep = 0;
      while (Serial.available() > 0 && isDigit(Serial.peek())) {
        ep = ep * 10 + (uint32_t)(Serial.read() - '0');
      }
      if (ep >= 1700000000UL) {
        ft8SetEpochTime(ep);
        Serial.printf("[TIME] Synced UTC Epoch: %lu\r\n", (unsigned long)ep);
      } else {
        Serial.println(switchThemeEditor(!switchThemeEditor()) ? "Theme editor enabled" : "Theme editor disabled");
      }
      break;
    }
    case '!':
      if(switchThemeEditor()) remoteSetColorTheme();
      break;
    case '@':
      if(switchThemeEditor()) remoteGetColorTheme();
      break;

    case 'c': // Clear decoded FT8 traffic list
      ft8ClearTraffic();
      event |= REMOTE_CHANGED;
      break;
    case 'D': // Run FT8 15s test vector decode
      ft8RunTestDecode();
      break;
    case 'F': // Force immediate decode on live waterfall
      ft8ForceLiveDecode();
      break;
    case 'Q': // Test carrier on the LCD backlight pin (its wiring radiates): Q<kHz> starts, Q0 restores the backlight
    {
      delay(15);
      uint32_t khz = 0;
      while (Serial.available() > 0 && isDigit(Serial.peek())) {
        khz = khz * 10 + (uint32_t)(Serial.read() - '0');
      }
      ledcDetach(PIN_LCD_BL);
      if (khz > 0) {
        bool ok = ledcAttach(PIN_LCD_BL, khz * 1000UL, 2);
        if (ok) ledcWrite(PIN_LCD_BL, 2);
        Serial.printf("[TEST] Carrier GPIO%d %lu kHz: %s (actual %lu Hz)\r\n", PIN_LCD_BL, (unsigned long)khz,
                      ok ? "ON" : "FAIL", ok ? (unsigned long)ledcReadFreq(PIN_LCD_BL) : 0UL);
      } else {
        ledcAttach(PIN_LCD_BL, 16000, 8);
        ledcWrite(PIN_LCD_BL, sleepOn() ? 0 : currentBrt);
        Serial.println("[TEST] Carrier OFF, backlight restored");
      }
      break;
    }
    case 'P': // Debug: P<Hz> sets SI4732 REFCLK_FREQ and re-tunes (no argument: re-tune only)
    {
      delay(15);
      uint32_t hz = 0;
      while (Serial.available() > 0 && isDigit(Serial.peek())) {
        hz = hz * 10 + (uint32_t)(Serial.read() - '0');
      }
      if (hz >= 31130 && hz <= 34406) rx.setRefClock(hz);
      rxSetFrequency(currentFrequency);
      Serial.printf("[TEST] REFCLK=%lu, re-tuned %u kHz\r\n", (unsigned long)hz, currentFrequency);
      break;
    }
    case 'J': // J<0.1ppm> or J-<0.1ppm>: set AM/SSB crystal ppm correction, saved to NVS; J alone prints it
    {
      delay(15);
      bool neg = Serial.peek() == '-';
      if (neg) Serial.read();
      bool have = false;
      int32_t v = 0;
      while (Serial.available() > 0 && isDigit(Serial.peek())) {
        v = v * 10 + (Serial.read() - '0');
        have = true;
      }
      if (have) {
        ssbPpmCal10 = (int16_t)(neg ? -v : v);
        ssbPpmSave();
        rxSetFrequency(currentFrequency);
        if (isSSB()) applySSBBfo();
      }
      Serial.printf("[CAL] SSB crystal correction %+.1f ppm\r\n", ssbPpmCal10 / 10.0f);
      break;
    }
    case 'U': // U<vol>: SI4732 volume used for GPIO34 capture in FT8/FT4 modes (not saved)
    {
      delay(15);
      int v = 0;
      while (Serial.available() > 0 && isDigit(Serial.peek())) v = v * 10 + (Serial.read() - '0');
      ft8SetAdcVolume((uint8_t)v);
      Serial.printf("[TEST] FT8 ADC volume = %d\r\n", v);
      break;
    }
    case 'G': // Debug: print SI4732 GET_REV firmware information
    {
      rx.getFirmware();
      Serial.printf("[TEST] SI47%02u FW %c.%c PATCH 0x%02X%02X CMP %c.%c CHIPREV %c\r\n",
                    rx.getFirmwarePN(), rx.getFirmwareFWMAJOR(), rx.getFirmwareFWMINOR(),
                    rx.getFirmwarePATCHH(), rx.getFirmwarePATCHL(),
                    rx.getFirmwareCMPMAJOR(), rx.getFirmwareCMPMINOR(), rx.getFirmwareCHIPREV());
      break;
    }
    case 'N': // Debug: turn WiFi radio off (reboot to restore) to check its RF noise contribution
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("[TEST] WiFi OFF");
      break;
    case 'Y': // Debug: pause/resume the 8 kHz ADC sampling timer
      Serial.printf("[TEST] ADC timer %s\r\n", ft8ToggleAdcTimer() ? "RUNNING" : "STOPPED");
      break;
    case 'x': // Toggle raw ADC audio streaming over Serial at 921600 baud
      ft8ToggleAudioStream();
      break;
    case 'd': // Print hardware & FT8 signal diagnostics
      ft8PrintDiag();
      break;
    case 'g': // Test SI4732 LOUT/ROUT mute masks vs GPIO34 ADC routing
      ft8TestAudioRouting();
      break;
    case 'k': // Toggle speaker mute (preserving GPIO34 ADC signal)
      ft8ToggleSpeakerMute();
      break;
    case 'z': // Manually sync FT8 slot to :00s
      ft8SyncSlotManual();
      break;
    case 'u': // Cycle LCD layout (Default -> S-Meter -> FT8)
      uiLayoutIdx = (uiLayoutIdx + 1) % (UI_FT8 + 1);
      event |= REMOTE_PREFS;
      break;
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7':
    {
      extern bool tuneToMemory(const Memory *memory);
      uint8_t mIdx = (uint8_t)(key - '1');
      if (mIdx < getTotalMemories() && memories[mIdx].freq) {
        tuneToMemory(&memories[mIdx]);
        Serial.printf("[TUNE] Switched to FT8 Preset #%d: %s %lu Hz (%s)\r\n",
                      mIdx + 1, bands[bandIdx].bandName, (unsigned long)memories[mIdx].freq, bandModeDesc[currentMode]);
        event |= REMOTE_PREFS;
      }
      break;
    }
    case '8': // Quick tune FM 90.00 MHz
    {
      bandIdx = 0; // VHF FM band
      bands[bandIdx].bandMode = FM;
      bands[bandIdx].currentFreq = 9000;
      selectBand(bandIdx, false);
      Serial.println("[TUNE] Switched to FM 90.00 MHz");
      event |= REMOTE_PREFS;
      break;
    }
    case '9': // Quick tune AM 17.740 MHz (16m broadcast)
    {
      for (int i = getTotalBands() - 1; i >= 0; i--) {
        if (bands[i].bandMode != FM && 17740 >= bands[i].minimumFreq && 17740 <= bands[i].maximumFreq) {
          bandIdx = i;
          break;
        }
      }
      bands[bandIdx].bandMode = AM;
      bands[bandIdx].currentFreq = 17740;
      selectBand(bandIdx, false);
      updateFrequency(17740, false);
      Serial.println("[TUNE] Switched to AM 17.740 MHz");
      event |= REMOTE_PREFS;
      break;
    }
    case '[': // BFO -100 Hz
      if (isSSB()) {
        updateBFO(currentBFO - 100, true);
        Serial.printf("[BFO] %s %u kHz | BFO = %+d Hz\r\n", bands[bandIdx].bandName, currentFrequency, currentBFO);
      }
      break;
    case ']': // BFO +100 Hz
      if (isSSB()) {
        updateBFO(currentBFO + 100, true);
        Serial.printf("[BFO] %s %u kHz | BFO = %+d Hz\r\n", bands[bandIdx].bandName, currentFrequency, currentBFO);
      }
      break;
    case '0': // BFO Reset to 0 Hz
      if (isSSB()) {
        updateBFO(0, true);
        Serial.printf("[BFO] %s %u kHz | BFO = 0 Hz\r\n", bands[bandIdx].bandName, currentFrequency);
      }
      break;

    default:
      // Command not recognized
      return(event);
  }

  // Command recognized
  return(event | REMOTE_CHANGED);
}
