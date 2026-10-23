# Technical overview

How Mochi stores animations, plays them, and decides what to show. Line numbers drift, so this document names functions instead; search for them in `Mochi_EN/Mochi_EN.ino` (or the identical `Mochi_TR/Mochi_TR.ino`).

## Storage layout

The 4MB flash is split by `partitions.csv`:

| Partition | Offset | Size | Contents |
|---|---|---|---|
| nvs | 0x9000 | 20KB | ESP32 system data |
| factory (app) | 0x10000 | 1.5MB | firmware |
| storage | 0x190000 | ~2.4MB | LittleFS: faces and config |

LittleFS is mounted at boot. If mounting fails (blank or corrupt partition), the firmware formats it once and continues; the web UI shows "formatted this boot".

Files on LittleFS:

- `/faces/<name>.bin`: one file per face, MOC2 or MOC3 (below). Names are limited to `A-Z a-z 0-9 _ -`, max 32 characters (`sanitizeFaceName`).
- `/config.json`: all settings. Writes go to `/config.tmp` first and are then renamed, so a power cut during a save can't leave a half-written file. After a successful load the file is copied to `/config.bak`, and that backup is used if `/config.json` fails to parse.

## Animation format

Every face file starts with an 8-byte header:

| Byte | Field | Value |
|---|---|---|
| 0-3 | magic | `MOC2` (raw) or `MOC3` (compressed) |
| 4 | frame_count | 1-255 |
| 5 | bytes_per_row | 16 (128 px / 8) |
| 6 | height | 64 |
| 7 | format | MOC3: `3` (RLE + delta). MOC2: ignored, usually `0` |

The firmware rejects anything that is not 128x64 (`loadRawMoc2`, `loadCompressedMoc3`, `validateFaceFile`), so a bad file can't make it allocate a huge buffer or draw out of bounds.

### Pixels

A frame is 1024 bytes: 64 rows of 16 bytes. Each byte holds 8 horizontal pixels, least significant bit first (bit 0 = leftmost pixel). A set bit is a lit pixel. This is the XBM layout, so a frame goes to the display with a single `u8g2.drawXBM(0, 0, 128, 64, frame)`.

### Frame timing

Every frame has a 1-byte delay in units of 10 ms (`1` = 10 ms, `255` = 2.55 s). At playback the delay is divided by the global speed multiplier (0.3x-3x) and never goes below 20 ms.

### MOC2 (raw)

```
header | delay0 frame0[1024] | delay1 frame1[1024] | ...
```

File size = `8 + frame_count × 1025`. Simple, but large: a 60-frame face is about 60KB.

### MOC3 (delta + RLE)

```
header | delay0 len0_lo len0_hi data0[len0] | delay1 len1_lo len1_hi data1[len1] | ...
```

Each frame is compressed in two steps:

1. **Delta:** XOR the frame with the previous one (the first frame is XORed with all zeros). Pixels that did not change become `0x00`. Face animations change only small areas between frames, so most of the delta is zeros.
2. **RLE:** encode the delta as `(count, value)` byte pairs, `count` 1-255. A frame that doesn't change at all compresses to 5 pairs (10 bytes) instead of 1024.

`len` is the compressed length as a little-endian 16-bit value. Decoding reverses this: expand the pairs, then XOR with the previous decoded frame.

Across the bundled faces, MOC3 is about 36% of the MOC2 size: all 53 take ≈820KB instead of ≈2.3MB, which leaves most of the ~2.4MB LittleFS free. All files in `Faces/bins/` are MOC3.

### Decompression happens at load time

`loadCompressedMoc3` decodes every frame into RAM when a face is loaded, using the MOC2 layout. Playback then just indexes into that buffer, so drawing costs no CPU for decompression. The trade-off: **RAM, not file size, limits animation length**. A loaded face takes `frame_count × 1025` bytes of heap whatever its file size (100 frames ≈ 100KB).

The web UI preview does the same decoding in the browser (`loadPreview`); the device only streams the raw file.

### Converter

`Faces/gif_to_bin.py` writes both formats (MOC3 by default). It flattens transparency onto black, resizes to 128x64 (`stretch`, `contain` or `cover`), thresholds to 1 bit, and turns GIF frame durations into delay units. It refuses inputs over 255 frames unless `--allow-trim` is given, and validates every file it writes by decoding it again (`validate_bin_file`).

## Event handling

### Two animation slots

- `currentAnim`: the default face. It stays loaded in RAM.
- `eventAnim`: one shared slot for everything temporary: reactions, idle animations, random events, tests.

`activeAnim` points to whichever is on screen. When an event finishes, `returnToDefaultFaceSafe` just points `activeAnim` back at `currentAnim`, without reading the file again. Earlier versions had seven separate slots; merging them into one reduced heap fragmentation.

### States

`enterMood` switches between these states:

| State | Entered when |
|---|---|
| `MOOD_IDLE` | default face playing (idle animations also run in this state) |
| `MOOD_POKED` | a short tap ends |
| `MOOD_PETTING` | a touch lasts longer than `longPressMs` (default 200 ms) |
| `MOOD_DEEP_PETTING` | the same touch reaches `deepPettingMs` (default 3000 ms) |
| `MOOD_PETTING_AFTER` | the finger is released after petting |
| `MOOD_RANDOM_EVENT` | a random event is due |
| `MOOD_ACCEL_EVENT` | an accelerometer reaction fires |
| `MOOD_TEST_EVENT` | a "Test" button in the web UI |

### Priority and scheduling

- **User input wins.** Touch and accelerometer reactions can interrupt anything, including idle and random animations.
- **Automatic animations are polite.** Idle animations and random events start only when the default face has been on screen for 8 s (`AUTO_DEFAULT_DWELL_MS`) and at least 15 s have passed since the last automatic animation ended (`AUTO_EVENT_COOLDOWN_MS`). They never queue up.
- **Idle animations** start at a random time between `idleEventMinSec` and `idleEventMaxSec`. Each entry has its own loop count or fixed duration.
- **Random events** start at a random time between `randomEventMinSec` and `randomEventMaxSec`, and not while an idle animation is playing.
- **No repeats:** every random pick avoids the last 4 faces from the same list when possible (`RANDOM_HISTORY_SIZE`).

### While WiFi is on

The WiFi stack takes a large share of the heap. With WiFi on:

- idle animations and random events are paused,
- touch and accelerometer reactions only use faces with at most 30 frames (poke, shake) or 45 frames (petting),
- a face that is too big for the remaining heap is skipped, and the default face keeps playing.

All of this goes through `playEventFaceSafe`, the single entry point for temporary animations.

## Touch

The TTP223 output is read with the ADC (`readTouchActive`), not as a digital input:

- sampled every 15 ms, so tight animation loops can't flood the ADC,
- hysteresis: pressed above 2200, released below 1200 (12-bit scale), so noise near one threshold can't toggle it,
- then debounced in software (`touchDebounceMs`, default 35 ms).

The ADC is used because on the prototype board GPIO1's digital input no longer recognized the sensor's ~2.9 V as HIGH. Reading it through the ADC works on healthy boards too.

`handleTouch` turns touches into gestures:

| Gesture | Default | Action |
|---|---|---|
| tap shorter than `longPressMs` | < 200 ms | poke reaction |
| hold | ≥ 200 ms | petting reaction |
| keep holding | ≥ 3000 ms | deep petting reaction |
| fast taps | 10 within 7 s | WiFi on/off |
| fast taps | 5 within 5 s | battery level on screen |

The WiFi and battery gestures overlap (the first 5 of 10 WiFi taps also satisfy the battery gesture). The battery gesture therefore waits for a short pause after the last tap, and WiFi wins if both match. The tap that completes a gesture is not also counted as a poke.

## Accelerometer

The MMA8452Q (I2C address 0x1C) raises hardware interrupts, configured active-high:

- **INT1 → GPIO2:** transient (shake) events. This pin is also a wake source.
- **INT2 → GPIO10:** orientation changes.

The firmware sorts events into six reaction lists, each with its own cooldown:

| Reaction | Cooldown |
|---|---|
| soft shake | 3 s |
| hard shake | 5 s |
| acceleration | 8 s |
| free-fall | 8 s |
| orientation | 4 s |
| pick-up (long stillness → movement → tilt) | 10 s |

Thresholds are set in the web UI. If an interrupt line fires more than 30 times per second (loose wire, floating pin), the firmware detaches the interrupts and falls back to polling over I2C (`handleAccelPollingFallback`), so the rest of the device stays responsive.

## Sleep

With no touch for `sleepTimeoutMinutes`, `enterSleepMode` plays an optional sleep animation, turns off WiFi and the display, and raises the accelerometer threshold to "hard shake" so ordinary movement doesn't wake it.

| Mode | Wake sources | Notes |
|---|---|---|
| Light | touch (GPIO1 high), hard shake (GPIO2 high) | wakes in milliseconds, keeps RAM |
| Deep | touch (GPIO1 high), hard shake (GPIO2 high) | lowest power; waking reboots the chip but skips the opening animation |
| Hybrid | light first, then deep after `sleepDeepAfterMinutes` (timer) | |

On waking, the display is re-initialized (some SH1106 boards come back mirrored otherwise), an optional wake animation plays, and the first touch is ignored until the finger is lifted, so waking up doesn't also count as a poke.

In deep sleep the ADC is off and touch wake depends on a *digital* HIGH on GPIO1. On a board where the sensor doesn't reach the digital threshold (like the prototype), only a hard shake wakes it from deep sleep reliably.

## Battery

`readBatteryVoltage` averages 8 `analogReadMilliVolts` samples on GPIO0 and multiplies by `BATTERY_DIVIDER_RATIO × BATTERY_CALIBRATION`. Readings go through an exponential filter (75% old, 25% new) every 2 s.

- **Percentage** comes from a single-cell LiPo discharge table (`batteryVoltageToPercent`): 3.30 V = 0%, 3.70 V = 20%, 3.85 V = 50%, 4.00 V = 80%, 4.20 V = 100%, linear in between. It's an estimate; voltage sags under load.
- **Warnings** appear on screen once each time the level falls past 50, 30, 20, 10 and 5%.
- **Charging** is guessed from a rising voltage trend, because the TP4056 CHRG/STDBY pins aren't connected.

The firmware does not cut power at low voltage; deep-discharge protection is the job of the battery's protection circuit (see [testing.md](testing.md)).

## Web API

All endpoints are served only while WiFi is on.

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | web UI (gzip) |
| GET | `/api/faces` | face list with frame count, size, format |
| GET | `/api/preview?name=` | raw face file for the browser preview |
| POST | `/api/upload` | upload a `.bin` (validated before it replaces anything) |
| POST | `/api/setactive?name=` | set the default face |
| POST | `/api/delete?name=`, `/api/deleteall` | delete faces |
| GET / POST | `/api/config` | read / save settings (values are clamped to valid ranges) |
| GET | `/api/config/export` | download `config.json` |
| POST | `/api/test?kind=&name=` | play a reaction without saving |
| POST | `/api/test-accel?type=&name=` | play an accelerometer reaction |
| GET | `/api/accel/live` | live accelerometer readings |
| GET | `/api/wifi/status`, `/api/wifi/scan` | WiFi status and scan |
| POST | `/api/wifi/connect` | save home WiFi credentials |
| GET | `/api/sys`, `/api/battery` | system and battery status |
| POST | `/api/restart`, `/api/sleeptest` | restart / enter sleep in 1 s |
| GET | `/api/script` | download the converter script, if it's on LittleFS |
