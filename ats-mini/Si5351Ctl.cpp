#include "Common.h"
#include "Si5351Ctl.h"
#include <Wire.h>
#include <Preferences.h>
#include <si5351.h>

#define SI5351_ADDR 0x60

// Si5351 output wired to the SI4732 RCLK pin (pin 13). The rework wire goes from the module pin "0".
#define SI5351_REF_CLK 0

// The module's 25 MHz crystal measured +78 ppm (2026-11-19): CLK0 = 21.075 MHz arrived as a 2649.5 Hz
// tone on USB dial 21074 kHz instead of 1000 Hz (RX already calibrated against the Pi's NTP-locked
// FT8 transmitter, rx_ppm 892.0). 25 MHz * (1 + 78.26e-6) = 25001956.5 Hz.
#define SI5351_XTAL_HZ 25001957UL

static Si5351 si5351;
static bool si5351Ready = false;

bool si5351Probe()
{
  Serial.print("[I2C] devices:");
  for(uint8_t a = 0x08; a < 0x78; a++)
  {
    Wire.beginTransmission(a);
    if(Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
  }
  Serial.println();

  si5351Ready = si5351.init(SI5351_CRYSTAL_LOAD_8PF, SI5351_XTAL_HZ, 0);
  Serial.printf("[SI5351] init %s\r\n", si5351Ready ? "ok" : "no answer at 0x60");
  return si5351Ready;
}

bool si5351Set(uint8_t clk, uint32_t hz)
{
  if(clk > 2) return false;
  if(!si5351Ready && !si5351Probe()) return false;

  const si5351_clock id = (si5351_clock)clk;
  if(hz == 0)
  {
    si5351.output_enable(id, 0);
    Serial.printf("[SI5351] CLK%u off\r\n", clk);
    return true;
  }
  si5351.set_freq((uint64_t)hz * 100ULL, id);
  si5351.output_enable(id, 1);
  Serial.printf("[SI5351] CLK%u = %lu Hz\r\n", clk, (unsigned long)hz);
  return true;
}

static bool extRefOn = false;

bool extRefBegin()
{
  Preferences p;
  bool enabled = false;
  if(p.begin("radiocal", false))
  {
    enabled = p.getBool("extref", false);
    if(enabled && p.getBool("extpend", false))
    {
      // The previous boot started the Si5351 reference and never finished rx.setup()
      p.putBool("extref", false);
      p.putBool("extpend", false);
      enabled = false;
      Serial.println("[SI5351] external RCLK failed last boot: back to the crystal");
    }
    else if(enabled) p.putBool("extpend", true);
    p.end();
  }
  if(!enabled) return false;

  extRefOn = si5351Set(SI5351_REF_CLK, 32768);
  if(!extRefOn)
  {
    extRefEnable(false);
    Serial.println("[SI5351] no Si5351: using the crystal");
  }
  else
  {
    delay(20);   // let the reference settle before the SI4732 reset
    Serial.println("[SI5351] SI4732 reference = CLK0 32768 Hz");
  }
  return extRefOn;
}

void extRefDone()
{
  Preferences p;
  if(p.begin("radiocal", false))
  {
    p.putBool("extpend", false);
    p.end();
  }
}

bool extRefActive() { return extRefOn; }

void extRefEnable(bool on)
{
  Preferences p;
  if(p.begin("radiocal", false))
  {
    p.putBool("extref", on);
    p.putBool("extpend", false);
    p.end();
  }
}

uint16_t rxRefClockHz() { return extRefOn ? 32768 : 32802; }
