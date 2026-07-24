# neonrift

An **ESP32 control panel** firmware. It auto‑connects to the strongest known Wi‑Fi network (or falls back to
a self‑hosted setup portal), mounts a [LittleFS](https://github.com/littlefs-project/littlefs) filesystem,
advertises itself on the LAN over mDNS (`http://neonrift.local/`), and serves a single‑page web dashboard
backed by an **async REST API** with **live WebSocket telemetry** and **over‑the‑air firmware updates**.

Built on the [ESP32Async](https://github.com/ESP32Async) `ESPAsyncWebServer` / `AsyncTCP` stack and
[ArduinoJson 7](https://arduinojson.org/), using [PlatformIO](https://platformio.org/) for builds.

---

## Features

- **Wi‑Fi provisioning / captive portal** — on boot, scans and connects to the strongest known network
  (15 s timeout, falls through the rest). If none connect, it starts a `neonrift-setup` SoftAP with a
  captive‑portal DNS so you can add a network from a phone — no re‑flashing.
- **Persistent credentials (NVS)** — networks added via the portal are stored in flash (`Preferences`) and
  merged with optional compile‑time seeds. Multiple networks supported.
- **Live telemetry over WebSocket** — `/ws` pushes a full device snapshot every 2 s; the dashboard updates
  in real time (heap, RSSI, uptime, clock) and gracefully falls back to polling if the socket drops.
- **OTA firmware updates** — upload a new `firmware.bin` from the browser (`POST /update`); the device
  flashes and reboots itself.
- **NTP clock** — syncs UTC time after connecting; real timestamps in `/health` and `/info`.
- **GPIO / LED control** — toggle the onboard LED via the API and UI (a template for real control endpoints).
- **Optional token auth** — set an API token to protect every mutating endpoint; open by default.
- **REST API + static dashboard** served from LittleFS, with CORS enabled.
- **mDNS / zeroconf** — reachable at `http://neonrift.local/`.
- **Auto filesystem upload** — a PlatformIO post‑upload hook re‑flashes the LittleFS image after each
  firmware upload (see [Build hooks](#build-hooks)).

---

## Hardware & requirements

| | |
|---|---|
| **Board** | `esp32dev` (generic ESP32 dev module) |
| **Framework** | Arduino |
| **Platform** | `espressif32` |
| **Filesystem** | LittleFS |
| **Serial monitor** | 115200 baud |

**Toolchain:** [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
(`pio`) or the PlatformIO VS Code extension.

Library dependencies are resolved automatically from `platformio.ini`
(`ESP32Async/ESPAsyncWebServer`, `ESP32Async/AsyncTCP`, `bblanchon/ArduinoJson@^7`). The WebSocket, captive
portal (`DNSServer`), credential store (`Preferences`), and OTA (`Update`) all use libraries bundled with the
Arduino‑ESP32 core — no extra dependencies.

---

## Repository layout

```
neonrift/
├── platformio.ini          # board, framework, libs, build hooks
├── src/
│   └── main.cpp            # firmware: Wi-Fi/portal, NVS, mDNS, WS, OTA, routes
├── include/
│   ├── secrets.h.example   # template for local config (tracked)
│   └── secrets.h           # your Wi-Fi seeds / API token (git-ignored)
├── data/                   # LittleFS image contents (web dashboard)
│   ├── index.html
│   ├── app.js
│   └── style.css
├── scripts/
│   └── auto_uploadfs_if_changed.py   # post-upload: rebuild + flash filesystem
└── test/                   # (PlatformIO) unit tests
```

---

## Configuration (`include/secrets.h`)

All local config is optional and **kept out of source control**. Copy the template and edit it:

```bash
cp include/secrets.h.example include/secrets.h
```

```cpp
// Seed networks (merged with anything saved via the portal; up to 3)
#define WIFI_SEED_SSID1 "MyNetwork"
#define WIFI_SEED_PASS1 "MyPassword"

// Optional: require this token on all mutating endpoints
// #define API_TOKEN "change-me"
```

With **no** `secrets.h` and nothing saved in NVS, the device boots straight into the setup portal. `secrets.h`
is git‑ignored, so credentials never enter the repo.

> ℹ️ Earlier revisions hard‑coded Wi‑Fi credentials in `src/main.cpp`. Those have been moved to the
> git‑ignored `secrets.h`. If your repo was ever pushed, rotate that password — it's still in git history.

---

## Build, flash & run

```bash
pio run                  # build firmware
pio run -t uploadfs      # build + flash the LittleFS image (the web UI)
pio run -t upload        # flash firmware (also auto-runs uploadfs — see below)
pio device monitor       # serial console @ 115200
```

On boot the serial log shows the Wi‑Fi scan, the chosen network and IP (or the setup‑portal address),
the LittleFS contents, and the mDNS URL. Then open `http://neonrift.local/` or `http://<device-ip>/`.

**First‑time / new network setup:** connect to the `neonrift-setup` Wi‑Fi network, let the captive portal
open (or browse to any URL), pick your network, enter the password, and save — the device reboots and joins it.

### Build hooks

`extra_scripts` in `platformio.ini` runs [`scripts/auto_uploadfs_if_changed.py`](scripts/auto_uploadfs_if_changed.py)
as a **post‑upload** action: after every `pio run -t upload` it (re)builds and flashes the LittleFS image so
firmware and web UI stay in sync. It hashes `data/` into a stamp file; the "skip if unchanged" optimization
is currently commented out, so the filesystem is uploaded on every run.

---

## HTTP API

Responses are JSON with permissive CORS headers. When an `API_TOKEN` is configured, the **mutating**
endpoints (POST/DELETE) require `Authorization: Bearer <token>` or a `?token=` query parameter.

| Method | Path | Auth | Description |
|--------|------|:----:|-------------|
| `GET` | `/` | — | Web dashboard |
| `GET` | `/health` | — | Liveness + clock |
| `GET` | `/info` | — | Full device / memory / Wi‑Fi / time snapshot |
| `GET` | `/api/scan` | — | Scan for nearby networks |
| `GET` | `/api/wifi` | — | List saved SSIDs (no passwords) + current connection |
| `POST` | `/api/wifi` | ✔ | `{ "ssid", "pass" }` → save to NVS and reboot to connect |
| `DELETE` | `/api/wifi?ssid=…` | ✔ | Forget a saved network |
| `GET` | `/api/led` | — | Onboard LED state |
| `POST` | `/api/led` | ✔ | `{ "on": true\|false }` → set LED |
| `POST` | `/update` | ✔ | Multipart `update` field → OTA flash + reboot |
| `WS` | `/ws` | — | Live telemetry; pushes the `/info` payload every 2 s |

### Examples

```bash
curl http://neonrift.local/health
curl http://neonrift.local/info | jq

# Add a network (with auth enabled)
curl -X POST http://neonrift.local/api/wifi \
  -H 'Authorization: Bearer change-me' -H 'Content-Type: application/json' \
  -d '{"ssid":"HomeWifi","pass":"hunter2"}'

# Toggle the LED
curl -X POST http://neonrift.local/api/led -H 'Content-Type: application/json' -d '{"on":true}'

# OTA update
curl -X POST http://neonrift.local/update -F update=@.pio/build/esp32dev/firmware.bin
```

`GET /info` (abridged):

```json
{
  "ok": true, "ap_mode": false,
  "chip_id": "0123456789ABCDEF", "sdk": "v4.4.x", "cpu_freq_mhz": 240,
  "uptime_s": 12, "heap_free": 250000, "flash_size": 4194304,
  "led": false, "time_synced": true, "time_iso": "2026-06-21T10:00:00Z",
  "wifi": { "status": 3, "ssid": "HomeWifi", "rssi": -57, "ip": "192.168.1.42", "mac": "AA:BB:CC:DD:EE:FF" }
}
```

---

## Web dashboard

The single page in [`data/`](data/) (no build step) shows **live status** over the WebSocket, an **LED**
toggle, a **Wi‑Fi** manager (saved networks, scan, add/forget), an **OTA** uploader with a progress bar, and
a collapsible raw‑JSON view. An optional API‑token field is stored in `localStorage` and sent with requests.
Edit anything under `data/` and re‑run `pio run -t uploadfs` (or `upload`) to update it.

---

## Roadmap / further ideas

- **Live charts** — plot heap / RSSI / uptime history from the WebSocket stream.
- **Generic GPIO endpoints** — read/write arbitrary pins (with an allowlist) beyond the onboard LED.
- **Sensor integrations** — temperature/humidity/etc. surfaced through `/info` and the dashboard.
- **HTTPS / signed OTA** — verify firmware images before applying.
- **Config export/import** — back up and restore the NVS network list.

---

## License

No license file is present; treat as all‑rights‑reserved unless the author states otherwise.
