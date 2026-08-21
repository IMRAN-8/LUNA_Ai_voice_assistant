# Setup

## 1. Database — Supabase

1. Create a project at [supabase.com](https://supabase.com).
2. Open **Project Settings → Database** and copy the connection string.
3. Keep it for `DATABASE_URL`.

The `memories` table is created automatically on first boot.

## 2. API keys

| Key | Where | Used for |
|---|---|---|
| `GROQ_API_KEY` | [console.groq.com/keys](https://console.groq.com/keys) | Whisper — command detection and STT fallback |
| `OPENROUTER_API_KEY` | [openrouter.ai/keys](https://openrouter.ai/keys) | `gpt-audio-mini` (voice) and `gpt-4o-mini` (text + memory judge) |

## 3. Server — Render

1. Push this repository to GitHub.
2. Create a **Web Service** on Render pointing at the repo.
   - Root directory: `server`
   - Build command: `npm install`
   - Start command: `npm start`
3. Under **Environment**, add:

```
DATABASE_URL=...
GROQ_API_KEY=...
OPENROUTER_API_KEY=...
```

4. Deploy, then confirm:

```
GET https://your-service.onrender.com/health   ->  OK
```

### Keeping it warm

Render's free tier spins down when idle, which adds ~50 s to the first request.
Add an [UptimeRobot](https://uptimerobot.com) HTTP monitor on `/health` every
5 minutes.

## 4. Firmware — Arduino IDE

Install **esp32 by Espressif, version 3.x** (required for the `ESP_SR` library).

```bash
cd esp32/Luna_ESP32_Code
cp secrets.h.example secrets.h
```

Edit `secrets.h`:

```c
#define WIFI_SSID       "your-network"
#define WIFI_PASSWORD   "your-password"
#define SERVER_BASE_URL "https://your-service.onrender.com"
```

> The ESP32-S3 is 2.4 GHz only. It will not see a 5 GHz network.
> `SERVER_BASE_URL` must have **no trailing slash**.

### Board settings — all of these are required

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB (128Mb) |
| **PSRAM** | **OPI PSRAM** |
| **Partition Scheme** | **ESP SR 16M** |
| USB CDC On Boot | Enabled |
| CPU Frequency | 240 MHz |
| Core Debug Level | Info |

> `ESP SR 16M` is what flashes `srmodels.bin`, the wake-word model.
> Without it `ESP_SR.begin()` fails and only the button works.
> An IDE update can silently reset these — re-check after upgrading.

Flash through the USB-C port marked **COM**.

If the board is not detected: hold **BOOT**, press and release **RST**,
release **BOOT**, then upload.

## 5. First boot

Open Serial Monitor at **115200 baud**. A healthy boot looks like:

```
[boot] PSRAM total=8388608 free=...
[wifi] connected
[audio] boot floor=...
[boot] wake word ready
[boot] running. Say "Hi ESP" or hold the button.
```

| Problem | Cause |
|---|---|
| `PSRAM total=0` | PSRAM is not set to OPI PSRAM |
| `ESP_SR.begin() failed` | Partition Scheme is not ESP SR 16M |
| Stuck on `Connecting...` | Wrong credentials, or a 5 GHz network |
| Nothing in Serial Monitor | Wrong port, wrong baud, or USB CDC On Boot disabled |

Then say **"Hi ESP"** (pronounced *hi-ee-ess-pee*), wait for the eyes to
change, and speak.

Next: [TUNING.md](TUNING.md) to calibrate the microphone for your room.
