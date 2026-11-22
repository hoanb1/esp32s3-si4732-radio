# Remote access (Cloudflare Tunnel)

Radio web UI: https://esp32.hoan.uk (set up 2026-11-22). **No Cloudflare Access login**: the owner chose to leave it open (only a few friends know the URL). Anyone with the URL can use the UI, including the TX controls. To lock it down, add a Zero Trust self-hosted app for esp32.hoan.uk (the API token on the Pi can only read Access, not create apps).

- Connector: Pi 192.168.3.24, systemd unit `cloudflared`, config `/etc/cloudflared/config.yml`, tunnel `81a21ff3-31ae-49e6-aa79-558b060968cd` (shared with the other hoan.uk hostnames).
- Ingress rule `esp32.hoan.uk -> http://192.168.3.39` (the board, LAN), placed before the final `http_status:404`. Backup: `config.yml.bak-<timestamp>`.
- DNS: `cloudflared tunnel --origincert /home/pi/.cloudflared/cert.pem route dns <tunnel> esp32.hoan.uk` (the `--origincert` flag is needed because the service runs as root).
- Check: `cloudflared tunnel --config /etc/cloudflared/config.yml ingress validate`, then `curl -sI https://esp32.hoan.uk`.

## URL parameters
The page applies these query parameters once on load (in this order); unknown or invalid values are ignored. TX, enable, send and auto are never available through the URL.

| param | meaning |
|---|---|
| `band` | band name before the bracket, case-insensitive, e.g. `20m ham` (`band=20m+ham`), `40m ham`, `vhf fm` |
| `mode` | `usb`, `lsb`, `am`, `fm`, `cw`, `ft8`, `ft4` |
| `freq` | kHz (MHz-with-decimals style is not used; for `mode=fm` the value is still kHz, e.g. `98500`) |
| `bw` | bandwidth index (0..) or label of the current mode, e.g. `2.2k` |
| `vol` | 0..63 |
| `audio` | `1` starts web audio; browsers keep it paused until the first click on the page |

Example: https://esp32.hoan.uk/?band=20m+ham&mode=usb&freq=14074&vol=40&audio=1

Audio over https failed before because the page opened `ws://` (blocked as mixed content); it now uses `wss://` when loaded over https.
