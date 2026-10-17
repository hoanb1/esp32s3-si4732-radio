#include "Common.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "Layout-FT8.h"

// Positions specific to this layout (the S-meter layout keeps the classic ones from Draw.h)
#define TOPBAR_H        18    // Status bar height
#define DEF_FREQ_Y      56    // Frequency vertical center
#define DEF_FUNIT_Y     39    // Frequency unit top
#define DEF_NAME_Y      88    // Station name / signal line top
#define CHIP_X         122    // Band chip left edge

//
// Segmented S-meter with dimmed empty segments, drawn on the status bar
//
static void drawBarSMeter(int strength, int x, int y, bool stereo, uint16_t bg)
{
  spr.drawTriangle(x + 1, y + 1, x + 11, y + 1, x + 6, y + 6, TH.smeter_icon);
  spr.drawLine(x + 6, y + 1, x + 6, y + 14, TH.smeter_icon);

  for(int i=0 ; i<17 ; i++)
  {
    uint16_t color = i >= strength ? TH.smeter_bar_empty : i < 10 ? TH.smeter_bar : TH.smeter_bar_plus;
    spr.fillRect(15 + x + i * 4, 2 + y, 2, 12, color);
  }

  // FM pilot: split the meter into two rows
  if(stereo) spr.fillRect(15 + x, 7 + y, 4 * 17 - 2, 2, bg);
}

void drawLayoutDefault(const char *statusLine1, const char *statusLine2)
{
  uint16_t panel = panelColor();

  // Status bar: S-meter, indicators, band & mode chips, WiFi, battery
  spr.fillRect(0, 0, 320, TOPBAR_H, panel);
  spr.drawFastHLine(0, TOPBAR_H - 1, 320, shadeColor(panel, 1));

  drawBarSMeter(getStrength(rssi), 1, 1, (currentMode==FM) && rx.getCurrentPilot(), panel);
  drawSaveIndicator(SAVE_OFFSET_X, 2);
  drawBleIndicator(BLE_OFFSET_X, 2);

  int cx = CHIP_X;
  cx += drawChip(getCurrentBand()->bandName, cx, 1, panel, TH.band_text, true) + 4;
  cx += drawChip(ft8GetDisplayModeDesc(), cx, 1, TH.mode_text, panel, false) + 4;
  if(muteOn(MUTE_MAIN))
    drawChip("MUTE", cx, 1, TH.box_off_text, TH.box_off_bg, true);

  bool has_voltage = drawBattery(BATT_OFFSET_X, 1);
  drawWiFiIndicator(has_voltage ? WIFI_OFFSET_X : BATT_OFFSET_X - 13, 1, panel);

  if(switchThemeEditor())
  {
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(TH.text_warn);
    spr.drawString(TH.name, 319, TOPBAR_H + 2, 2);
  }

  // Frequency, units, and optionally a highlighted digit
  spr.setFreeFont(&Orbitron_Light_24);
  drawFrequency(
    currentFrequency,
    FREQ_OFFSET_X, DEF_FREQ_Y,
    FUNIT_OFFSET_X, DEF_FUNIT_Y,
    currentCmd == CMD_FREQ ? getFreqInputPos() + (pushAndRotate ? 0x80 : 0) : 100
  );

  // Station or channel name if present, otherwise signal quality
  if(*getStationName() == 0xFF)
    drawLongStationName(getStationName() + 1, MENU_OFFSET_X + 1 + 76 + MENU_DELTA_X + 2, DEF_NAME_Y + 4);
  else if(*getStationName())
    drawStationName(getStationName(), RDS_OFFSET_X + 40, DEF_NAME_Y);
  else
  {
    char text[40];
    sprintf(text, "RSSI %u dBuV    SNR %u dB", rssi, snr);
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(TH.text_muted);
    spr.drawString(text, RDS_OFFSET_X + 40, DEF_NAME_Y + 6, 2);
  }

  // Left-side menu/info bar
  drawSideBar(currentCmd, MENU_OFFSET_X, MENU_OFFSET_Y, MENU_DELTA_X);

  if(currentCmd == CMD_SCAN)
  {
    drawScanGraphs(isSSB()? (currentFrequency + currentBFO/1000) : currentFrequency);
  }
  else if(!drawWiFiStatus(statusLine1, statusLine2, STATUS_OFFSET_X, STATUS_OFFSET_Y))
  {
    // Show radio text if present, else show frequency scale
    if(*getRadioText() || *getProgramInfo())
      drawRadioText(STATUS_OFFSET_Y, STATUS_OFFSET_Y + 25);
    else
      drawScale(isSSB()? (currentFrequency + currentBFO/1000) : currentFrequency);
  }
}
