# Remote access (Cloudflare Tunnel)

Radio web UI: https://esp32.hoan.uk (set up 2026-11-22). **No Cloudflare Access login**: the owner chose to leave it open (only a few friends know the URL). Anyone with the URL can use the UI, including the TX controls. To lock it down, add a Zero Trust self-hosted app for esp32.hoan.uk (the API token on the Pi can only read Access, not create apps).

- Connector: Pi 192.168.3.24, systemd unit `cloudflared`, config `/etc/cloudflared/config.yml`, tunnel `81a21ff3-31ae-49e6-aa79-558b060968cd` (shared with the other hoan.uk hostnames).
- Ingress rule `esp32.hoan.uk -> http://192.168.3.39` (the board, LAN), placed before the final `http_status:404`. Backup: `config.yml.bak-<timestamp>`.
- DNS: `cloudflared tunnel --origincert /home/pi/.cloudflared/cert.pem route dns <tunnel> esp32.hoan.uk` (the `--origincert` flag is needed because the service runs as root).
- Check: `cloudflared tunnel --config /etc/cloudflared/config.yml ingress validate`, then `curl -sI https://esp32.hoan.uk`.
