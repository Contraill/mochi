# Testing

## Automated (CI)

Every push and pull request runs [`.github/workflows/ci.yml`](../.github/workflows/ci.yml):

| Job | What it checks |
|---|---|
| Firmware build (EN, TR) | Both sketches compile for ESP32-C3 and fit the 1.5MB app partition. |
| Converter | `gif_to_bin.py` unit tests, a GIF → MOC2/MOC3 round trip, and decoding of every file in `Faces/bins/`. |
| Web UI | The English and Turkish UIs run in headless Chromium against a mock device: settings load into the form, saving sends the right values (clamped to valid ranges), reaction lists save only with their own button, config import posts the file and reloads, and the English UI shows no Turkish text. |

Run them locally:

```sh
pip install pillow pytest playwright
playwright install chromium
pytest tests
```

The web UI tests use a mock server, not the real firmware. They cover what the browser sends and shows, not how the firmware stores it. The firmware side of saving is covered by the manual checks below.

## Manual (on the device)

These need real hardware. Open the serial monitor at 115200 baud; the firmware logs every gesture, and prints a status line every 5 s with the raw touch ADC value (`touchAdcLast`), free heap and the current state.

### Sleep and wake

Set the sleep timeout to 1 minute in the System tab. The **Sleep Test (1 s)** button skips the wait.

| # | Mode | Action | Expected |
|---|---|---|---|
| S1 | Light | wait for timeout | sleep animation (if set), screen off, WiFi off |
| S2 | Light | tap once | wakes within ~0.5 s, wake animation, default face; the waking tap does **not** trigger a poke |
| S3 | Light | shake hard | wakes |
| S4 | Light | shake gently / move slowly | stays asleep |
| S5 | Deep | wait for timeout, then tap | restarts: wake animation (if set), **no** opening animation, default face |
| S6 | Deep | shake hard | restarts like S5, then plays a hard shake reaction |
| S7 | Hybrid (deep after 1 min) | wait > 2 min, then tap | restarts like S5 (went from light to deep) |
| S8 | Hybrid | tap within the first minute of sleep | wakes without a restart (still in light sleep); the serial log has no new `[BOOT]` lines |
| S9 | any | keep a finger on the sensor when the timeout passes | does not go to sleep |
| S10 | any | sleep with WiFi on | WiFi turns off; it stays off after waking |

If S5 fails but S6 works, the touch sensor's output doesn't reach a digital HIGH on GPIO1 (see the Touch section in [technical.md](technical.md)).

### False positives: touch

| # | Action | Expected |
|---|---|---|
| T1 | leave untouched for 10 min, watch the log | `touchAdcLast` stays well below 1200; no `[TAP]` lines, no reactions |
| T2 | put a phone or charger cable next to the sensor | no touches registered |
| T3 | single quick tap | exactly one `[TAP]` line and one poke |
| T4 | hold 1 s | petting, no poke |
| T5 | hold 4 s | petting, then deep petting |
| T6 | 10 taps within 7 s | WiFi toggles; the battery overlay does **not** appear |
| T7 | 5 taps, then stop | battery overlay after a short pause; WiFi unchanged |
| T8 | run on battery and on USB | same results (USB noise can shift the ADC) |

If one tap counts twice, raise **Debounce ms** (try 50-70).

### False positives: accelerometer

Use **Live Sensor Test** in the System tab to see the readings.

| # | Action | Expected |
|---|---|---|
| A1 | leave on a desk for 10 min | no reactions |
| A2 | tap the desk next to it | no hard shake reaction |
| A3 | type on a keyboard on the same desk | no reactions |
| A4 | carry it while walking | at most occasional soft shake reactions, never back to back (cooldowns) |
| A5 | shake gently / hard | soft / hard reaction, each at most once per cooldown (3 s / 5 s) |
| A6 | drop it about 20 cm onto a cushion | free-fall reaction |
| A7 | turn it on its side | orientation reaction |
| A8 | wiggle a loose INT wire on purpose | after a burst, the log shows the interrupts being disabled and polling taking over; touch keeps working |

Raise the thresholds in the System tab if A1-A3 trigger.

### Battery and charging

| # | Check | Expected |
|---|---|---|
| B1 | measure the battery with a multimeter and compare with the web UI | within ±0.05 V after calibration (`BATTERY_DIVIDER_RATIO`, `BATTERY_CALIBRATION`) |
| B2 | 5 taps | the on-screen percentage matches the web UI |
| B3 | run down from full | warnings appear once each at 50, 30, 20, 10 and 5% |
| B4 | plug in the TP4056 USB | the charging icon appears within ~1 min; disappears after unplugging |
| B5 | measure the battery at the end of charging | ≤ 4.20 V; TP4056 LED switches to "done" |
| B6 | touch the TP4056 and battery while charging | warm at most, never hot |
| B7 | check the TP4056 board | it has the DW01 protection chip (the version with OUT+/OUT− pads), or the cell has its own protection board. The firmware does **not** cut power at low voltage. |
| B8 | check the charge current resistor (R_PROG) | charge current ≤ the cell's 1C rating (stock 1.2 kΩ = 1 A; too much for cells under 1000 mAh) |
| B9 | plug USB into the ESP32 while the battery is connected | no current flows back into the battery from USB 5V (your board needs a diode on its 5V line; measure if unsure) |

The voltage divider draws about 6 µA from the battery at all times, even in deep sleep.

### Saving and loading settings (on the device)

| # | Action | Expected |
|---|---|---|
| W1 | change a few settings, save, press **Restart** | all values are still set after the reboot |
| W2 | tick reactions without saving, reload the page | ticks are gone; the "not saved" warning was shown |
| W3 | **Download Config**, change settings, **Upload Config** with the downloaded file | settings return to the downloaded values |
| W4 | upload a config with out-of-range values (e.g. `"wifiTimeoutMinutes": 999`) | value is clamped (120), the device keeps working |
| W5 | upload a broken JSON file | error message, current settings unchanged |
| W6 | pull the power right after saving, then power on | settings are either the old or the new values, never lost (`/config.tmp` → rename, `/config.bak` fallback) |
| W7 | upload a non-face `.bin` (any random file) | rejected; existing faces untouched |
