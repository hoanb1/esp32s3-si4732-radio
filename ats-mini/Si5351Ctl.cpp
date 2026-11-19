#include "Common.h"
#include "Si5351Ctl.h"
#include <Wire.h>
#include <si5351.h>

#define SI5351_ADDR 0x60

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

  // xo_freq 0 = library default 25 MHz
  si5351Ready = si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
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
