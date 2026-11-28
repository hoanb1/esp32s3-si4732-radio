# ATS Mini

![](docs/source/_static/esp32-si4732-ui-theme.jpg)

A fork of the ATS Mini firmware for SI4732 receivers, with FT8/FT4 decoding and a built-in web UI.

| Build profile | Board | Notes |
|---|---|---|
| `v3s` | ESP32-S3 N16R8 (8 MB PSRAM, 16 MB flash) | receive only |
| `classic` | ESP32-WROOM, 4 MB flash | receive, plus FT8/WSPR transmit with an external Si5351, PA, filter and relay (amateur licence required) |

Wiring is defined in [`ats-mini/Board.h`](ats-mini/Board.h). Build with `arduino-cli compile --profile <profile> ats-mini`.

Based on the following sources:

* Volos Projects:    https://github.com/VolosR/TEmbedFMRadio
* PU2CLR, Ricardo:   https://github.com/pu2clr/SI4735
* Ralph Xavier:      https://github.com/ralphxavier/SI4735
* Goshante:          https://github.com/goshante/ats20_ats_ex
* G8PTN, Dave:       https://github.com/G8PTN/ATS_MINI

## Releases

Check out the [Releases](https://github.com/hoanb1/esp32s3-si4732-radio/releases) page.

## Documentation

The hardware, software and flashing documentation is available at <https://esp32-si4732.github.io/ats-mini/>

## Discuss

* [GitHub Discussions](https://github.com/esp32-si4732/ats-mini/discussions) - the best place for feature requests, observations, sharing, etc.
* [TalkRadio Telegram Chat](https://t.me/talkradio/174172) - informal space to chat in Russian and English.
* [Si4732 Mini Receiver All Bands](https://www.facebook.com/share/g/18hjHo4HEe/) - Facebook group (unofficial).
