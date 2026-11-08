# Heimao Intercom

An ESP32-S3 handheld that is simultaneously an **SA818S analog radio**, a **network voice terminal** (LAN direct / P2P / MQTT relay), a **6-band EQ**, a **99-channel library**, with an embedded web panel and an Android push-to-talk app.

> **Licence notice**
> This repository is **source-available**, not an OSI open-source project.
> Personal use, study, modification and non-commercial redistribution are free.
> **Any commercial use requires a written licence** — see [COMMERCIAL.md](COMMERCIAL.md), mail@baiai.org.
> Code licence: [PolyForm Noncommercial 1.0.0](LICENSE). The `flasher/` directory is a GPL-3.0 derivative with its own licence.

## Components

| Part | Description |
|---|---|
| Firmware | ESP-IDF 5.4.1, ESP32-S3-N16R8 (16 MB flash / 8 MB PSRAM), SA818S radio, ES8311 codec, front panel |
| Web panel | Served by the device; HTTP and HTTPS (self-signed) |
| Android app | Kotlin + Jetpack Compose; discovery, control lease, PTT, settings |
| Flasher | Windows single-file EXE; keep-config or full erase; multiple COM ports |

## Downloads

From [Releases](../../releases):

- `黑猫对讲-<version>.apk` — Android app (Android 7.0+)
- `黑猫互联-full.bin` — complete image for USB flashing
- `黑猫互联-app-OTA.bin` — OTA application image
- `黑猫互联刷机工具-<version>.exe` — Windows flasher (**no embedded firmware**; select the full image)
- `SHA256SUMS.txt`

The public flasher does not bundle firmware because it is GPL-3.0 while the firmware is licensed separately.

## Quick start

1. Flash `full.bin` with the flasher (keep-config or factory mode).
2. Connect to the setup hotspot `黑猫智能手台-XXXXXX`, open `http://192.168.4.1/`, join your Wi-Fi.
3. Open `http://<board-ip>/` for the panel; use `https://` when the browser needs the microphone.
4. Install the APK and connect to the same board.

## Build

| Part | Command | Docs |
|---|---|---|
| Firmware | `cd firmware && idf.py build` (ESP-IDF 5.4.1) | [docs/build/firmware.md](docs/build/firmware.md) |
| App | `cd app && ./gradlew :app:assembleDebug` (JDK 21, SDK 37) | [docs/build/android.md](docs/build/android.md) |
| Flasher | `flasher/build.ps1` (Python 3.11) | [docs/build/flasher.md](docs/build/flasher.md) |

## Documentation

[docs/getting-started.md](docs/getting-started.md) · [docs/hardware.md](docs/hardware.md) · [docs/PINMAP.md](docs/PINMAP.md) · [docs/architecture.md](docs/architecture.md) · [docs/api/http-api.md](docs/api/http-api.md) · [docs/api/audio-ws.md](docs/api/audio-ws.md) · [docs/flashing.md](docs/flashing.md) · [docs/troubleshooting.md](docs/troubleshooting.md)

## Licence and contributing

- Code: PolyForm Noncommercial 1.0.0 — personal development free, commercial use by written licence.
- Commercial: [COMMERCIAL.md](COMMERCIAL.md) · mail@baiai.org
- Contributions: [CONTRIBUTING.md](CONTRIBUTING.md) — a CLA is required before merging.
- Third-party notices: [THIRD_PARTY.md](THIRD_PARTY.md) · [NOTICE](NOTICE)
- Trademarks: [TRADEMARK.md](TRADEMARK.md)

## Disclaimer

Amateur radio / self-hosted network experiment. You are responsible for complying with local frequency, power and licensing rules. The software is provided “as is”, without warranty of any kind.
