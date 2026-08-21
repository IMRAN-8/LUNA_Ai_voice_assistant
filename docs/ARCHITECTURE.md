# Architecture

## One voice interaction

1. **WakeNet** detects "Hi ESP" fully on-device (or the button is held)
2. ESP-SR pauses; the microphone switches from the **wake** gain profile to the **recording** profile
3. Luna waits up to 10 s for speech. Nothing is recorded yet — audio only circulates through a 300 ms pre-roll ring
4. Speech starts: the pre-roll is flushed into the clip so the first consonant survives
5. Speech ends: ~800 ms of silence closes the utterance, trailing silence is trimmed, a WAV header is written
6. The WAV is uploaded to `POST /api/converse`
7. The server transcribes with **Whisper** first, then calls **`gpt-audio-mini`** with that transcript as grounding
8. The 24 kHz PCM reply is peak-normalized, resampled to 16 kHz, wrapped in a WAV header, and returned with the text in the `X-Reply` header
9. The ESP32 reads `X-Reply`, starts the OLED typing animation, and begins playback after a 64 KB pre-buffer while the rest still downloads
10. The microphone reopens, ESP-SR resumes, the eyes return to idle

If `/api/converse` fails, the firmware automatically falls back to
`/api/stt` -> `/api/chat` -> `/api/tts`.

---

## Why Whisper runs before the audio model

`gpt-audio-mini` has its own internal speech understanding, and on quiet or
noisy audio it will confidently answer a **different question** — in one
observed case replying in Bengali about "galaxy fan" when the user asked
"what is the color of sun?".

Whisper large-v3-turbo is markedly more accurate. It runs first (~0.3 s) and
its transcript is injected into the **system** prompt as ground truth. It is
deliberately not placed in the user message, because the model would then
read the instruction aloud.

Whisper also drives command detection for `remember` and `reset all`.

---

## Audio path

| Stage | Detail |
|---|---|
| Driver | `ESP_I2S` — required by ESP-SR; the legacy `driver/i2s.h` cannot coexist with it |
| Microphone | `I2S_NUM_1` RX, raw 32-bit slots, `I2S_STD_SLOT_LEFT` |
| Speaker | `I2S_NUM_0` TX, 16 kHz, 16-bit, mono |
| Sample rate | 16 kHz — Whisper's native rate and WakeNet's required rate |
| Gain | One floating-point multiply applied to the full 24-bit value; `micGain 1.0` reproduces the legacy `(W>>11) x 1.6` |
| Limiting | Soft `tanh` knee at 73 % of full scale, never a hard corner |
| Recording | Up to 15 s **of speech** in PSRAM; leading silence never enters the buffer |
| Playback | 256 KB PSRAM ring, starts after a 64 KB pre-buffer |

### Why the whole VAD scales with gain

The gate rails were originally absolute int16 constants. Raising `micGain`
lifted the measured noise floor but not the rails, so the gate saturated and
the continue gate could land *below* the room floor — the recording then ran
to the full length cap and never ended.

Every rail is now multiplied by `micGain`, so the tuning stays valid at any
gain.

### Underrun behaviour

The download runs at roughly 30 KB/s while playback consumes 31.2 KB/s, so
the ring can only drain. When it runs dry the player writes **real silence**
to I2S. Writing nothing would leave the TX DMA repeating its last buffer,
which is heard as buzzing. After a dry spell it rebuilds a 16 KB cushion
before resuming, so a weak link produces one short gap instead of continuous
stutter.

---

## Server endpoints

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/api/converse` | Fast path — audio in, audio out |
| `POST` | `/api/stt` | Speech-to-text (fallback) |
| `POST` | `/api/chat` | Text chat (fallback + memory mode) |
| `POST` | `/api/tts` | Text-to-speech (fallback) |
| `GET` | `/api/memory` | All stored facts as JSON |
| `POST` | `/api/memory` | Add a fact |
| `DELETE` | `/api/memory/:id` | Delete a fact |
| `GET` | `/memory` | Browser UI for viewing and editing memory |
| `GET` | `/health` | Uptime probe |

`/api/converse` returns the spoken reply as a WAV body and the text in the
`X-Reply` header (URL-encoded). Memory mode returns **HTTP 204** with
`X-Reply: MEMORY_MODE`.

### OpenRouter audio requires streaming

Audio output **must** be requested with `stream: true` and
`audio.format: "pcm16"`. A non-streaming request is rejected with
`400 "Audio output requires stream: true"`.

The base64 chunks arriving in `delta.audio.data` are accumulated **as a
string** and decoded once at the end. Chunk boundaries are not guaranteed to
be 4-character aligned, so decoding each chunk separately corrupts the PCM.

---

## Memory

Facts are stored in Postgres and injected into every request as a single
system block. An AI judge decides whether a statement is worth saving, and
inserts use case-insensitive de-duplication.

Memory always runs through the text endpoints, because the judge needs a
transcript and an audio model never returns the user's own words verbatim.

Saying **"reset all"** clears every stored fact.

---

## Task placement

| Task | Core | Priority |
|---|---|---|
| SR feed | 0 | 5 |
| Display | 0 | 1 |
| TTS playback | 0 | 6 |
| Main loop, audio capture, networking | 1 | — |

Wi-Fi modem sleep is disabled so DTIM wakeups cannot preempt the SR feed
task, and I2C runs at 400 kHz to shorten blocking OLED writes.
