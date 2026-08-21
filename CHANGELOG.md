# Changelog

All notable changes to this project are documented here.
This project follows [Semantic Versioning](https://semver.org/).

---

## [2.0.0] — 2026-08-21

Full rebuild on the ESP32-S3. The 2-second input limit of v1.1 is gone.

### Added
- **Offline wake word** — ESP-SR WakeNet9 (`wn9_hiesp`, phrase "Hi ESP") running fully on-device
- **`/api/converse` fast path** — audio in to audio out in a single upstream call via `gpt-audio-mini`
- **Streaming playback** — 256 KB PSRAM ring buffer; audio starts after a 64 KB pre-buffer instead of waiting for the whole clip
- **Live voice activity detection** — adaptive noise floor, 300 ms pre-roll so the first consonant survives, automatic endpointing, trailing-silence trim
- **Runtime tuning console** — adjust gain, VAD, and timing over serial with no reflash; values persist in NVS
- **Distance test and live meter** — guided measurement at 20 cm / 50 cm / 1 m
- **Animated OLED eyes** — idle animation plus listening, thinking, speaking, and error moods
- **Memory relevance judge** — an AI filter decides which statements are worth storing
- **Automatic fallback** — reverts to STT → chat → TTS if the fast path fails

### Changed
- Board: ESP32 DevKit to **ESP32-S3-WROOM-1 N16R8**
- Max voice input: **2 s to 15 s of speech**, buffered in PSRAM
- Audio driver: legacy `driver/i2s.h` to **`ESP_I2S`** (required by ESP-SR)
- Recording gain: single floating-point path; VAD thresholds now scale with gain
- Memory: injected as one system block instead of fake user turns
- Display task pinned to Core 0; audio and networking on Core 1

### Fixed
- **VAD coupling bug** — gate rails were absolute constants, so raising gain silently broke speech detection
- **Under-gain** — roughly 20.8 dB of headroom was being wasted on the recording path
- **Audio output failure** — OpenRouter requires `stream: true` for audio; without it the model returned no audio
- **Quiet playback** — server-side peak normalization to about -3 dBFS
- **Buzzing on weak Wi-Fi** — the playback task now feeds silence during an underrun instead of letting the I2S DMA repeat a stale buffer
- **Wrong-topic and wrong-language replies** — Whisper's transcript is now passed to the audio model as grounding
- **Memory mode** — fact capture is button-controlled instead of a fixed 1.5 s window
- **TLS heap exhaustion** — the session is released after every turn

### Security
- Wi-Fi credentials and server URL moved out of the sketch into a gitignored `secrets.h`

---

## [1.1.0]

### Added
- AI chat via GPT-4o-mini (OpenRouter)
- Long-term memory using Supabase
- Speech-to-text via Groq Whisper
- Text-to-speech via Google TTS
- OLED display for messages and status
- Node.js server on Render, kept warm by UptimeRobot
- Memory add, view, and reset

### Limitation
- Voice input limited to 2 seconds
