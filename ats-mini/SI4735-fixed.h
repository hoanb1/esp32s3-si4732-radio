#include <SI4735.h>

class SI4735_fixed: public SI4735
{
  public:
    // Fixing SI4735::getRdsPI() bug where it only returns BLOCKAL
    uint16_t getRdsPI(void)
    {
      if(getRdsReceived() && getRdsNewBlockA())
        return (currentRdsStatus.resp.BLOCKAH << 8) + currentRdsStatus.resp.BLOCKAL;
      else
        return 0x0000;
    }

    // Fixing SI4735::getRdsProgramType() bug where it only returns three lower bits
    uint8_t getRdsProgramTypeX(void)
    {
      uint16_t blockB = (currentRdsStatus.resp.BLOCKBH << 8) + currentRdsStatus.resp.BLOCKBL;
      return (blockB >> 5) & 0x1F;
    }

    // Fixing SI4735::getRdsText2A() which does not follow version bit
    char *getRdsText2A(void)
    {
      return getRdsVersionCode()? NULL : SI4735::getRdsText2A();
    }

    // Fixing SI4735::getRdsText2B() which does not follow version bit
    char *getRdsText2B(void)
    {
      return getRdsVersionCode()? SI4735::getRdsText2B() : NULL;
    }

    // Only one kind of text is available
    inline char *getRdsProgramInformation(void)
    {
      return getRdsVersionCode()? SI4735::getRdsText2B() : SI4735::getRdsText2A();
    }

    // Only one kind of text is available
    inline char *getRdsStationInformation(void)
    {
      return getRdsVersionCode()? SI4735::getRdsText2B() : SI4735::getRdsText2A();
    }

  void seekStationProgress(void (*showFunc)(uint16_t f), bool (*stopSeeking)(), uint8_t up_down)
  {
    si47x_frequency freq;
    long elapsed_seek = millis();

    // seek command does not work for SSB
    if (lastMode == SSB_CURRENT_MODE)
      return;

    seekStation(up_down, 0);
    do
    {
      delay(maxDelaySetFrequency);
      getStatus(0, 0);
      delay(maxDelaySetFrequency);
      freq.raw.FREQH = currentStatus.resp.READFREQH;
      freq.raw.FREQL = currentStatus.resp.READFREQL;
      currentWorkFrequency = freq.value;
      if (showFunc != NULL)
        showFunc(freq.value);
      if (stopSeeking != NULL)
        if (stopSeeking())
          return;

    } while (!currentStatus.resp.VALID && !currentStatus.resp.BLTF && (millis() - elapsed_seek) < maxSeekTime);
  }

    // Reliable SI4732 SSB patch download waiting for CTS after every 8-byte frame
    bool downloadPatch(const uint8_t *ssb_patch_content, const uint16_t ssb_patch_content_size)
    {
      for(uint16_t offset=0 ; offset<ssb_patch_content_size ; offset+=8)
      {
        Wire.beginTransmission(deviceAddress);

        for(uint16_t i=0 ; i<8 ; i++)
          Wire.write(pgm_read_byte_near(ssb_patch_content + (i + offset)));

        Wire.endTransmission();
        waitToSend();
      }

      delayMicroseconds(250);
      return true;
    }

    // Load SSB patch and ensure currentTune is AM_TUNE_FREQ before setSSBConfig()
    void loadPatch(const uint8_t *ssb_patch_content, const uint16_t ssb_patch_content_size, uint8_t ssb_audiobw)
    {
      queryLibraryId();
      patchPowerUp();
      delay(50);
      downloadPatch(ssb_patch_content, ssb_patch_content_size);
      // Critical fix: patchPowerUp() switches SI4732 to FUNC=1 (AM/SSB), so update currentTune
      // before setSSBConfig(), otherwise setSSBConfig() returns early when switching from FM!
      this->currentTune = AM_TUNE_FREQ;
      this->currentFrequencyParams.arg.FREEZE = 0;
      this->currentFrequencyParams.arg.FAST = 1;
      this->currentFrequencyParams.arg.DUMMY1 = 0;
      this->currentFrequencyParams.arg.ANTCAPH = 0;
      this->currentFrequencyParams.arg.ANTCAPL = 1;
      setSSBConfig(ssb_audiobw, 1, 0, 1, 0, 1);
      delay(25);
    }

    // Query SI4732 GET_REV hardware register to verify SSB patch ID (0xA902 when loaded, 0x0000 if unpatched)
    uint16_t getPatchId()
    {
      getFirmware();
      return ((uint16_t)firmwareInfo.resp.PATCHH << 8) | (uint16_t)firmwareInfo.resp.PATCHL;
    }
};
