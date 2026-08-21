# Hardware

## Bill of materials

| Component | Notes |
|---|---|
| ESP32-S3-WROOM-1 **N16R8** | 16 MB flash + **8 MB OPI PSRAM**. PSRAM is mandatory — ESP-SR will not start without it. |
| INMP441 | I2S MEMS microphone, 24-bit data in a 32-bit slot |
| MAX98357A | I2S class-D mono amplifier |
| Speaker | 3 W 8 Ω works; **4 Ω is noticeably louder** on the same amplifier |
| SH1106 1.3" OLED | I2C, 128x64, address `0x3C` |
| Push button | Momentary, push-to-talk fallback |

Board used here is the **YD-style dual USB-C** variant: one port is native USB
(GPIO19/20), the other is a CH343P USB-to-UART bridge marked **COM**.

---

## Pin map

### INMP441 microphone

| INMP441 | ESP32-S3 |
|---|---|
| SCK (BCLK) | **GPIO4** |
| WS (LRCL) | **GPIO5** |
| SD (DOUT) | **GPIO6** |
| L/R | **GND** (selects the left slot) |
| VDD | 3V3 |
| GND | GND |

> `L/R` must go to GND. The firmware reads `I2S_STD_SLOT_LEFT`; the right slot is silent on this wiring.

### MAX98357A amplifier

| MAX98357A | ESP32-S3 |
|---|---|
| BCLK | **GPIO15** |
| LRC | **GPIO16** |
| DIN | **GPIO7** |
| VIN | **5V** |
| GND | GND |
| SD | leave unconnected |
| GAIN | leave unconnected |

> Speaker output is **bridged** — do not connect either speaker terminal to ground.

### SH1106 OLED

| OLED | ESP32-S3 |
|---|---|
| SDA | **GPIO8** |
| SCL | **GPIO9** |
| VCC | 3V3 |
| GND | GND |

### Button

| Button | ESP32-S3 |
|---|---|
| One side | **GPIO21** |
| Other side | GND |

Uses `INPUT_PULLUP`, active LOW. No external resistor required.

---

## Pins you must not use

| Pin(s) | Reason |
|---|---|
| **GPIO35, 36, 37** | Reserved for the octal flash/PSRAM on N16R8 |
| **GPIO19, 20** | Native USB D-/D+ |
| **GPIO0, 45, 46** | Strapping pins |
| **GPIO43, 44** | UART0 / TX-RX LEDs |
| GPIO48 | Onboard WS2812 RGB LED (if the pads are bridged) |

---

## Power

During development, the **COM** USB-C port powers everything.

For battery operation:

```
18650 cell
   |
TP4056 charger (with protection)
   |
power switch
   |
MT3608 boost, adjusted to exactly 5.0 V
   |-- ESP32-S3  5V pin
   '-- MAX98357A VIN

ESP32-S3 3V3 --> INMP441 VDD, OLED VCC
All grounds tied together
```

> **Never** feed raw 18650 voltage into the `3V3` pin. It reaches 4.2 V when
> fully charged and will damage the board. The onboard 5 V to 3.3 V LDO is
> rated for about 1 A.

**Recommended:** place a 100 µF electrolytic and a 0.1 µF ceramic capacitor
directly across the MAX98357A `VIN`/`GND`. Current peaks during playback can
otherwise sag the rail and cause audible noise.

---

## Layout tips

- Keep I2S wiring short and away from speaker leads
- Keep the microphone away from the speaker to reduce mechanical feedback
- Keep the INMP441 sound port unobstructed and facing the user
- Use one solid common ground
