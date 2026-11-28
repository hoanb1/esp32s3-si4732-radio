// Standalone V3S pin probe (ESP32-S3-WROOM-1 N16R8 + SI4732-A10).
// Finds the SI4732 on I2C, reads GET_REV, surveys external pull resistors and
// samples the GPIO11 audio wire while FM plays (WiFi off).
// Prints over native USB CDC; the report repeats every 10 s.
//
// arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,PSRAM=opi,FlashSize=16M tools/v3s-probe

#include <Wire.h>

#define PIN_RADIO_EN 15
#define PIN_RADIO_RST 16
#define PIN_SCL 17
#define PIN_SDA 18
#define PIN_MUTE 3
#define PIN_AMP_EN 10
#define PIN_VBAT 4
#define PIN_AUDIO 11  // SI4732 LOUT single-wire mod

static String report;

static void logf(const char *fmt, ...)
{
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  report += line;
  report += '\n';
}

// Read a pin with each internal pull to tell whether something external drives it
static const char *pullState(int pin)
{
  pinMode(pin, INPUT_PULLDOWN);
  delay(2);
  int down = digitalRead(pin);
  pinMode(pin, INPUT_PULLUP);
  delay(2);
  int up = digitalRead(pin);
  pinMode(pin, INPUT);
  if(down && up) return "external HIGH";
  if(!down && !up) return "external LOW";
  if(!down && up) return "floating/weak";
  return "?";
}

static bool ack(uint8_t addr)
{
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Wait for the CTS bit in the status byte
static int waitCts(uint8_t addr)
{
  for(int i = 0 ; i < 100 ; i++)
  {
    delay(5);
    if(Wire.requestFrom(addr, (uint8_t)1) == 1)
    {
      int s = Wire.read();
      if(s & 0x80) return s;
    }
  }
  return -1;
}

static void getRev(uint8_t addr)
{
  // POWER_UP: FM receive, crystal oscillator on, analog audio out
  Wire.beginTransmission(addr);
  Wire.write(0x01);
  Wire.write(0x10);
  Wire.write(0x05);
  Wire.endTransmission();
  delay(500);
  int s = waitCts(addr);
  logf("POWER_UP status 0x%02X", s);

  Wire.beginTransmission(addr);
  Wire.write(0x10);  // GET_REV
  Wire.endTransmission();
  waitCts(addr);
  uint8_t r[9] = {0};
  int n = Wire.requestFrom(addr, (uint8_t)9);
  for(int i = 0 ; i < n && i < 9 ; i++) r[i] = Wire.read();
  logf("GET_REV (%d bytes): status 0x%02X part Si47%02d fw %c.%c patch %02X%02X comp %c.%c rev %c",
       n, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);

  Wire.beginTransmission(addr);
  Wire.write(0x11);  // POWER_DOWN
  Wire.endTransmission();
}

static void command(uint8_t addr, std::initializer_list<uint8_t> bytes)
{
  Wire.beginTransmission(addr);
  for(uint8_t b : bytes) Wire.write(b);
  Wire.endTransmission();
  waitCts(addr);
}

static void setProperty(uint8_t addr, uint16_t prop, uint16_t value)
{
  command(addr, {0x12, 0x00, (uint8_t)(prop >> 8), (uint8_t)prop, (uint8_t)(value >> 8), (uint8_t)value});
  delay(10);
}

// DC level and AC swing on the audio wire (SI4732 LOUT -> GPIO11)
static void measureAudio(const char *label)
{
  const int samples = 4000;
  uint32_t lo = 0xFFFFFFFF, hi = 0;
  double sum = 0, squares = 0;

  delay(300);
  for(int i = 0 ; i < samples ; i++)
  {
    uint32_t mv = analogReadMilliVolts(PIN_AUDIO);
    if(mv < lo) lo = mv;
    if(mv > hi) hi = mv;
    sum += mv;
    squares += (double)mv * mv;
    delayMicroseconds(100);
  }

  double mean = sum / samples;
  double variance = squares / samples - mean * mean;
  logf("GPIO%d %-26s mean %4.0f mV, min %4u, max %4u, rms %5.1f mV", PIN_AUDIO, label,
       mean, (unsigned)lo, (unsigned)hi, sqrt(variance > 0 ? variance : 0));
}

// Play FM, sample the audio wire in each state. The speaker is audible only while
// MUTE is LOW and AMP_EN is HIGH (3 s, listen for it).
static void audioTest(uint8_t addr)
{
  digitalWrite(PIN_RADIO_RST, LOW);
  delay(10);
  digitalWrite(PIN_RADIO_RST, HIGH);
  delay(10);
  measureAudio("radio not powered up");

  command(addr, {0x01, 0x10, 0x05});          // POWER_UP: FM, crystal, analog out
  delay(500);
  setProperty(addr, 0x1302, 0);               // no soft mute
  setProperty(addr, 0x4000, 40);              // volume
  command(addr, {0x20, 0x00, 0x22, 0x2E, 0x00}); // tune 87.50 MHz
  delay(100);
  command(addr, {0x21, 0x0C});                // seek up, wrap
  delay(5000);
  command(addr, {0x22, 0x01});                // FM_TUNE_STATUS, cancel seek
  uint8_t r[8] = {0};
  int n = Wire.requestFrom(addr, (uint8_t)8);
  for(int i = 0 ; i < n && i < 8 ; i++) r[i] = Wire.read();
  logf("seek: valid %d freq %u.%02u MHz rssi %u snr %u", r[1] & 1,
       ((r[2] << 8) | r[3]) / 100, ((r[2] << 8) | r[3]) % 100, r[4], r[5]);

  digitalWrite(PIN_MUTE, HIGH);
  digitalWrite(PIN_AMP_EN, LOW);
  measureAudio("playing MUTE=1 AMP=0");
  digitalWrite(PIN_MUTE, LOW);
  measureAudio("playing MUTE=0 AMP=0");
  digitalWrite(PIN_AMP_EN, HIGH);
  measureAudio("playing MUTE=0 AMP=1");
  delay(3000);
  digitalWrite(PIN_MUTE, HIGH);
  measureAudio("playing MUTE=1 AMP=1");
  digitalWrite(PIN_AMP_EN, LOW);

  setProperty(addr, 0x4001, 3);               // radio hard mute L+R
  measureAudio("radio hard mute");
  setProperty(addr, 0x4001, 0);
  setProperty(addr, 0x4000, 63);
  measureAudio("playing volume 63");

  command(addr, {0x11});                      // POWER_DOWN
  measureAudio("radio powered down");
}

// Power and reset the radio, then scan one SDA/SCL pair
static bool probe(int sda, int scl, int rst)
{
  pinMode(rst, OUTPUT);
  digitalWrite(rst, LOW);
  delay(10);
  digitalWrite(rst, HIGH);
  delay(10);

  Wire.end();
  if(!Wire.begin(sda, scl, 100000)) return false;
  Wire.setTimeOut(20);

  bool found = false;
  for(uint8_t a = 0x08 ; a < 0x78 ; a++)
  {
    if(!ack(a)) continue;
    logf("SDA %d SCL %d RST %d: device at 0x%02X", sda, scl, rst, a);
    if(a == 0x11 || a == 0x63)
    {
      getRev(a);
      found = true;
    }
  }
  return found;
}

static void run()
{
  logf("=== V3S probe ===");
  logf("chip %s rev %d, flash %u MB, PSRAM %u bytes",
       ESP.getChipModel(), ESP.getChipRevision(),
       (unsigned)(ESP.getFlashChipSize() >> 20), (unsigned)ESP.getPsramSize());

  const int survey[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47, 48};
  for(int pin : survey) logf("pull GPIO%d: %s", pin, pullState(pin));

  // Keep the amplifier off and audio muted while probing
  pinMode(PIN_AMP_EN, OUTPUT);
  digitalWrite(PIN_AMP_EN, LOW);
  pinMode(PIN_MUTE, OUTPUT);
  digitalWrite(PIN_MUTE, HIGH);

  // Does the radio answer without the enable pin?
  pinMode(PIN_RADIO_EN, OUTPUT);
  digitalWrite(PIN_RADIO_EN, LOW);
  delay(200);
  logf("--- EN GPIO%d LOW", PIN_RADIO_EN);
  bool offFound = probe(PIN_SDA, PIN_SCL, PIN_RADIO_RST);

  digitalWrite(PIN_RADIO_EN, HIGH);
  delay(200);
  logf("--- EN GPIO%d HIGH", PIN_RADIO_EN);
  bool found = probe(PIN_SDA, PIN_SCL, PIN_RADIO_RST);
  if(found) audioTest(ack(0x11) ? 0x11 : 0x63);

  // Control: with reset held low the SI4732 must not answer
  digitalWrite(PIN_RADIO_RST, LOW);
  delay(10);
  logf("--- RST GPIO%d held LOW: 0x11=%d 0x63=%d", PIN_RADIO_RST, ack(0x11), ack(0x63));

  if(!found) logf("SI4732 not found on SDA %d SCL %d", PIN_SDA, PIN_SCL);
  logf("RESULT found_en_high=%d found_en_low=%d", found, offFound);

  Wire.end();
  pinMode(PIN_RADIO_RST, OUTPUT);
  digitalWrite(PIN_RADIO_RST, LOW);
  logf("VBAT GPIO%d: %u mV at pin (x1.702 = %u mV)", PIN_VBAT,
       (unsigned)analogReadMilliVolts(PIN_VBAT), (unsigned)(analogRead(PIN_VBAT) * 1.702f));
}

void setup()
{
  Serial.begin(115200);
  delay(2000);
  run();
}

void loop()
{
  Serial.print(report);
  Serial.println();
  delay(10000);
}
