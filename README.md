# Mochi

A small desk companion that shows animated faces on a 128x64 OLED, running on an ESP32-C3. It reacts to touch, petting and shaking; faces, reactions and settings are managed from a web UI you open on your phone.

## Features

- 50+ animated faces (`Faces/`)
- Touch reactions: poke, petting, deep petting
- Accelerometer reactions: shake, flip, free-fall
- Random idle and event animations, avoiding recent repeats
- Web UI: face gallery, upload/delete, reaction assignments, settings, config import/export (UI is in Turkish)
- WiFi AP mode (opened with a quick tap combo) and home WiFi connection
- Battery level and low-battery notification
- Light / deep / hybrid sleep modes
- MOC2 (raw) and MOC3 (delta + RLE compressed) animation formats

## Hardware

- ESP32-C3 SuperMini
- 1.3" SH1106 SPI OLED
- TTP223 touch sensor
- MMA8452Q accelerometer
- LiPo battery + TP4056 charger board
- 2x 330k resistors (battery measurement)

## Wiring

| Module | Module pin | ESP32-C3 |
|---|---|---|
| SH1106 OLED | VCC | 3V3 |
| | GND | GND |
| | SCK | GPIO4 |
| | MOSI | GPIO6 |
| | RES | GPIO3 |
| | DC | GPIO5 |
| | CS | GPIO7 |
| TTP223 | VCC | 3V3 |
| | GND | GND |
| | OUT | GPIO1 |
| MMA8452Q | VCC | 3V3 |
| | GND | GND |
| | SDA | GPIO20 |
| | SCL | GPIO21 |
| | INT1 | GPIO2 |
| | INT2 | GPIO10 |
| TP4056 | B+ / B- | LiPo + / - |
| | OUT+ | 5V |
| | OUT- | GND |
| Battery divider | OUT+ → 330k → GPIO0 → 330k → GND | GPIO0 |

## Setup

### 1. Arduino IDE

- Board: **ESP32C3 Dev Module** (esp32 core)
- USB CDC On Boot: **Enabled**
- Flash: 4MB. `partitions.csv` sits in the sketch folder, so it is picked up automatically (1.5MB app + ~2.4MB LittleFS).

Libraries (Library Manager):

- U8g2
- ArduinoJson (v7)
- SparkFun MMA8452Q

### 2. Configure

In `Mochi.ino`:

- **WiFi password:** change `AP_PASSWORD` (default `example1`). It must be at least 8 characters or the access point will not start.
- **Battery calibration:** resistor tolerances and ADCs differ between boards, so calibrate your own. Measure the battery with a multimeter, compare it with the voltage shown in the web UI, and adjust:
  - `BATTERY_DIVIDER_RATIO` — divider ratio (2.0 for two equal resistors in theory)
  - `BATTERY_CALIBRATION` — fine correction multiplier
  - `BATTERY_MIN_VOLTAGE` / `BATTERY_MAX_VOLTAGE` — voltages shown as 0% and 100%

### 3. Flash

Open `Mochi.ino`, select the board and upload.

### 4. Upload faces

1. Tap Mochi quickly several times (default: 10 taps within 7 seconds). WiFi turns on.
2. Connect your phone to the `R2D2` network and open `192.168.4.1` in a browser.
3. Upload the `.bin` files from `Faces/bins/` through the UI.

## Making your own faces

`Faces/gif_to_bin.py` converts GIF/PNG/JPG files to the `.bin` format Mochi reads. Requires Pillow:

```sh
pip install pillow
cd Faces
python gif_to_bin.py                       # convert every GIF in the folder to ./bins
python gif_to_bin.py love.gif smirk.gif    # specific files
python gif_to_bin.py --preview -t 120 *.gif
```

Default output is 128x64 MOC3. See `python gif_to_bin.py --help` for all options.

## Project layout

```
Mochi.ino          firmware
Mochi_html.h       web UI (gzip, PROGMEM)
partitions.csv     flash partition table
mochi_config.json  sample config (can be imported from the UI)
Faces/             face GIFs, .bin outputs and converter
```

## License

The code is released under the [MIT](LICENSE) license. The MIT license does not cover the animations in `Faces/`.

The GIF animations are property of Dasai and are included here for personal use with the Watcher Mochi project. All rights to the animations belong to Dasai.
