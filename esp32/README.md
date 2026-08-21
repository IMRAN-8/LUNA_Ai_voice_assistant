# Luna firmware

Arduino sketch for the ESP32-S3-WROOM-1 (N16R8).

```
Luna_ESP32_Code/
  Luna_ESP32_Code.ino    the sketch
  secrets.h.example      credentials template
```

## Before compiling

```bash
cp Luna_ESP32_Code/secrets.h.example Luna_ESP32_Code/secrets.h
```

Fill in `WIFI_SSID`, `WIFI_PASSWORD`, and `SERVER_BASE_URL`.
`secrets.h` is gitignored.

## Required libraries

- `Adafruit GFX Library`
- `Adafruit SH110X`
- `ESP_I2S` and `ESP_SR` (bundled with esp32 core 3.x)

## Board settings

See [../docs/SETUP.md](../docs/SETUP.md). **PSRAM must be `OPI PSRAM` and
Partition Scheme must be `ESP SR 16M`**, or the wake word will not load.

## Tuning

The sketch has a serial console at 115200 baud — see
[../docs/TUNING.md](../docs/TUNING.md).
