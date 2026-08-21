# Audio tuning

Luna exposes a serial console so the microphone can be tuned for your room
**without reflashing**. Open Serial Monitor at **115200 baud**, type a
command, press Enter.

---

## Command reference

### Diagnostics

| Command | What it does |
|---|---|
| `help` / `?` | List all commands |
| `status` | Current settings, live levels, clip/limiter counts, heap, PSRAM |
| `meter` | Toggle the live RMS/peak meter |
| `test` | Guided distance test at 20 cm / 50 cm / 1 m |
| `calibrate` | Re-measure the room noise floor |
| `reset` | Clear wake and audio counters |

### Levels

| Command | Default | Purpose |
|---|---|---|
| `gain <0.25-8.0>` | `2.5` | Recording loudness sent to the server |
| `wakegain <1-20>` | `10.0` | Gain for the wake-word engine **only** |
| `knee <0.5-1.0>` | `0.73` | Soft-limiter threshold |

### Voice activity detection

| Command | Default | Purpose |
|---|---|---|
| `noise <1.2-4.0>` | `1.9` | How far above the noise floor speech must rise to start |
| `cont <0.40-0.95>` | `0.70` | Hysteresis for staying in speech |
| `silence <300-3000>` | `800` | Milliseconds of silence that end an utterance |
| `minspeech <40-400>` | `90` | Minimum voiced duration accepted |
| `wait <2000-30000>` | `10000` | How long to wait for you to start talking after the wake word |
| `caltime <200-2000>` | `500` | Noise-measurement duration |

### Persistence

| Command | What it does |
|---|---|
| `save` | Store current settings in NVS flash (survives reboot) |
| `load` | Reload the last saved settings |
| `defaults` | Restore built-in defaults (type `save` to keep them) |
| `norm` | Toggle clip normalization — **leave off**, it hurt recognition |
| `oled` | Toggle OLED animation, to A/B test its effect on wake rate |

---

## Recommended starting values

| Setting | Quiet room | Noisy room | Why |
|---|---|---|---|
| `gain` | 2.5 | 2.0 | Gain lifts noise along with speech |
| `noise` | 1.9 | 2.4 | Needs more separation from a higher floor |
| `cont` | 0.70 | 0.78 | Keeps the end gate above room noise |
| `silence` | 800 | 900 | Allows slightly longer pauses |
| `minspeech` | 90 | 140 | Rejects short background bursts |

Noisy-room sequence:

```
gain 2.0
noise 2.4
cont 0.78
silence 900
minspeech 140
calibrate
save
```

---

## Tuning procedure

1. Silence the room, then run `calibrate`.
2. Run `meter`.
3. Speak at your intended distance, in a normal voice.
4. Adjust `gain` until the loudest speech peaks around **15,000-22,000** with `clip=0`.
5. Run `calibrate` again — the noise floor scales with gain.
6. Turn `meter` off and run `save`.

> **Always recalibrate after changing gain.** VAD thresholds are derived from
> the measured floor, and that floor moves when gain moves.

---

## Reading a healthy turn

```
[audio] speech start rms=1021 noise=269 gate=512 cont=358 preroll=300ms
[audio] trimmed 550ms of trailing silence
[audio] samples=39424 seconds=2.46 speech=1 silenceStop=1 peakRMS=1736
[converse] try1 code=200
[tts] audio starts at 163ms (65KB buffered)
[tts] done 210KB in 6845ms underruns=0 peak=22885 (69% of full scale)
[net] session released, heap=126548
```

| Field | Healthy value |
|---|---|
| `speech=` | `1` — VAD detected you |
| `silenceStop=` | `1` — ended cleanly, not by hitting the length cap |
| `underruns=` | `0` — playback never starved |
| `peak=` | 12,000-24,000 (35-75 % of full scale) |
| `heap=` | ~126 KB after release — a TLS leak shows as ~73 KB |

---

## A note on the live meter

While idle, the microphone runs on the **wake** gain profile, but the gate
shown by `meter` is computed in **recording** units. The two differ by
`wakeGain / micGain`, so an idle meter can read `SPEECH` in a silent room.

Trust `status` -> **Noise RMS** and the `test` command, which both measure on
the recording profile.

---

## Troubleshooting

| Symptom | Where to look |
|---|---|
| Wake word never fires | Serial should show `wakenet9` and `detect start`. If not, Partition Scheme is not ESP SR 16M. |
| "Didn't hear you" after every wake | Compare `gate` with `peakRMS` in the speech-start line. The live tracker self-corrects in ~100 ms. |
| Records the full 15 s | The continue gate landed below the room floor; look for `endpoint via SAFETY gate`. |
| Buzzing during playback | Check `underruns=`. If non-zero, the link is slower than the audio bitrate. |
| Microphone reads silence | Confirm `L/R` is tied to GND and the left slot is selected. |
| `[stt] code=-7` | Heap exhaustion from a held TLS session. |
