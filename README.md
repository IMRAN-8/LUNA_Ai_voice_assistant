# Luna — ESP32-S3 AI Voice Assistant

**Version 2.0 · Stable Release**

Luna is a self-contained AI voice assistant built on an **ESP32-S3-WROOM-1 (N16R8)**.
She wakes on an **offline wake word**, answers in a natural voice, remembers facts
about you, and shows animated eyes on an OLED while she listens and thinks.

No audio leaves the device until the wake word fires.

---

## What is new in v2.0

v1.1 ran on a classic ESP32 DevKit and was limited to **2-second voice input**.
v2.0 is a full rebuild on the ESP32-S3.

| | v1.1 | **v2.0** |
|---|---|---|
| Board | ESP32 DevKit | **ESP32-S3-WROOM-1 N16R8** (8 MB PSRAM) |
| Wake word | none — button only | **Offline WakeNet9 ("Hi ESP")** |
| Max voice input | **2 s** | **15 s of speech** (PSRAM buffered) |
| Pipeline | STT → LLM → TTS (3 calls) | **1 call** `/api/converse` (audio in → audio out) |
| Endpointing | fixed length | **Live VAD** + pre-roll + silence trim |
| Playback | download, then play | **Streaming ring buffer**, starts in ~0.15 s |
| Audio tuning | hardcoded | **Runtime serial console** + NVS persistence |
| Memory | basic | System-prompt injection + AI fact judge |

---

## Features

- **Offline wake word** — ESP-SR WakeNet9 runs fully on-device
- **Single-call voice pipeline** — `gpt-audio-mini` via OpenRouter, with an automatic 3-call fallback
- **Long-term memory** — Supabase/Postgres, injected as a system block, with an AI relevance filter
- **Animated OLED eyes** — idle animation, listening/thinking/speaking moods, typing effect
- **Live voice activity detection** — adaptive noise floor, 300 ms pre-roll, automatic endpointing
- **Runtime audio tuning** — change gain and VAD over serial, no reflash, saved to flash
- **Push-to-talk fallback** — hardware button always works, even if the wake word fails to load
- **Graceful degradation** — falls back to STT/chat/TTS if the fast path fails

---

## System overview

```
        +-----------------------------+                +--------------------------+
        |   ESP32-S3-WROOM-1 N16R8    |   POST /api/   |    Render (Node.js)      |
        |  INMP441  MAX98357A  SH1106 |   converse     |     Luna server v2       |
        |  WakeNet9 "Hi ESP" offline  | -------------> |                          |
        |                             | <------------- |                          |
        +-----------------------------+  WAV + X-Reply +------------+-------------+
                                                                    |
                              +-------------------------+-----------+-----------+
                              v                         v                       v
                  +---------------------+   +---------------------+   +-------------------+
                  |     OpenRouter      |   |    Groq Whisper     |   | Supabase Postgres |
                  |   gpt-audio-mini    |   | command detection   |   |      memory       |
                  |  audio in -> audio  |   |  + STT fallback     |   |                   |
                  +---------------------+   +---------------------+   +-------------------+
```

---

## Repository layout

```
esp32/Luna_ESP32_Code/
  Luna_ESP32_Code.ino     Firmware: WakeNet, dual I2S, VAD, streaming playback, OLED
  secrets.h.example       Credentials template -> copy to secrets.h
server/
  server.js               /api/converse fast path, legacy endpoints, memory API
  package.json            Dependencies
  .env.example            Environment variable template
docs/
  HARDWARE.md             Pin map, wiring, bill of materials, power
  SETUP.md                Arduino IDE, Render, Supabase, UptimeRobot
  TUNING.md               Serial console reference and audio tuning procedure
  ARCHITECTURE.md         Request flow and design decisions
```

---

## Quick start

**1. Server**

```bash
cd server
npm install
cp .env.example .env      # fill in your keys
npm start
```

Or deploy to Render and set the same variables under **Environment**.

**2. Firmware**

```bash
cd esp32/Luna_ESP32_Code
cp secrets.h.example secrets.h   # fill in Wi-Fi + your server URL
```

Open in Arduino IDE with these settings — **all of them are required**:

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB (128Mb) |
| PSRAM | **OPI PSRAM** |
| Partition Scheme | **ESP SR 16M** |
| USB CDC On Boot | Enabled |
| CPU Frequency | 240 MHz |

Flash via the USB-C port marked **COM**, then say **"Hi ESP"**.

Full instructions: [`docs/SETUP.md`](docs/SETUP.md)

---

## Hardware

| Component | Role |
|---|---|
| ESP32-S3-WROOM-1 N16R8 | 16 MB flash, 8 MB PSRAM — PSRAM is required by ESP-SR |
| INMP441 | I2S MEMS microphone |
| MAX98357A | I2S class-D amplifier |
| Speaker | 3 W, 8 Ω (4 Ω is louder) |
| SH1106 1.3" OLED | I2C display, address 0x3C |
| Push button | Push-to-talk fallback |

Pin map and wiring: [`docs/HARDWARE.md`](docs/HARDWARE.md)

---

## Security

- `secrets.h` (Wi-Fi credentials, server URL) is **gitignored** and never committed
- All server keys are **environment variables** — no key is ever hardcoded or shipped to the device
- The firmware holds no API keys; it only knows your server URL

---

## Known limitations

- Wake-word hit rate is ~85–95 % — the practical ceiling for a single microphone with no array or beamforming
- The wake phrase is fixed to **"Hi ESP"**; a custom phrase requires a commercially trained WakeNet model
- Reliable 1 m recognition depends on room acoustics, noise, and enclosure design
- Wi-Fi reconnect and OTA updates are not implemented yet

---

## License

MIT — see [LICENSE](LICENSE).
---
Author : 
Imran Hosen
```
GitHub: https://github.com/IMRAN-8
```
