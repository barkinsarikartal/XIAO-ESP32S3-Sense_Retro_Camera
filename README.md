# XIAO ESP32-S3 Sense Retro Camera

A compact retro-styled digital camera built on the Seeed Studio XIAO ESP32-S3 Sense. Captures high-resolution photos and MJPEG/PCM audio video, with a live viewfinder on a 2.0" TFT display. Runs a multi-task FreeRTOS architecture for robust, concurrent operation.

---

## Features

- **High-Resolution Photo Capture** — HD JPEG images (1280×720) saved to SD card
- **Video Recording with Audio** — MJPEG video at 480×320, 10 FPS, with 16 kHz mono PCM audio via the built-in PDM microphone, saved as `.avi`
- **Live Viewfinder** — Real-time 320×240 RGB565 camera preview on the TFT display
- **Hardware Mirroring** — Toggle horizontal flip (selfie mode) via the built-in Boot button
- **FPS Counter & SD Status** — Live overlay showing frame rate and SD card health
- **Reliable AVI Encoding** — Accurate `idx1` index table built from actual chunk write order; `microSecPerFrame` derived from declared target FPS, not a variable end-of-recording average
- **A/V Synchronisation** — I2S DMA geometry tuned to drain exactly one video frame's worth of audio per read (`AUDIO_BYTES_PER_FRAME = 3200` bytes, 10×320-byte DMA slots); fixes cumulative drift
- **Atomic Multi-Core Synchronisation** — `std::atomic<int>` for all inter-core display-buffer state, preventing SMP data races
- **PSRAM Efficiency** — AVI metadata arrays (frame sizes, audio sizes, chunk order) allocated once at boot and `memset` per recording session, eliminating heap fragmentation

---

## Hardware

| Component | Part |
|---|---|
| Microcontroller | Seeed Studio XIAO ESP32-S3 Sense (OV2640 + SD card expansion) |
| Display | 2.0" TFT LCD — ST7789VW, 240×320, SPI |
| Shutter button | 1× tactile button (GPIO 4 / GPIO 5) |
| Mirror button | Built-in Boot button (GPIO 0) |
| Storage | MicroSD card, FAT32 formatted |
| Power | 3.7 V Li-ion battery or USB-C |

---

## Pin Configuration

<div align="center">
  <img src="images/connections.png" width="600" alt="Connection Diagram">
  <br><br>
  <table>
    <tr>
      <td valign="top" align="center">
        <h3>TFT Display (ST7789VW)</h3>
        <table>
          <tr><th>TFT Pin</th><th>GPIO</th></tr>
          <tr><td>GND</td><td>GND</td></tr>
          <tr><td>VCC</td><td>3.3 V</td></tr>
          <tr><td>SCL (SCK)</td><td>GPIO 7</td></tr>
          <tr><td>SDA (MOSI)</td><td>GPIO 9</td></tr>
          <tr><td>RESET</td><td>GPIO 3</td></tr>
          <tr><td>DC</td><td>GPIO 2</td></tr>
          <tr><td>CS</td><td>GPIO 1</td></tr>
        </table>
      </td>
      <td width="20"></td>
      <td valign="top" align="center">
        <h3>Buttons & Control</h3>
        <table>
          <tr><th>Function</th><th>GPIO</th></tr>
          <tr><td>Shutter — leg 1</td><td>GPIO 4 (OUTPUT)</td></tr>
          <tr><td>Shutter — leg 2</td><td>GPIO 5 (INPUT_PULLUP)</td></tr>
          <tr><td>Mirror toggle</td><td>GPIO 0 (Boot, built-in)</td></tr>
          <tr><td>SD Card CS</td><td>GPIO 21</td></tr>
          <tr><td>Mic CLK (PDM)</td><td>GPIO 42</td></tr>
          <tr><td>Mic DATA (PDM)</td><td>GPIO 41</td></tr>
          <tr><td colspan="2"><em>TFT and SD share SPI2_HOST (MISO: GPIO 8)</em></td></tr>
        </table>
      </td>
    </tr>
  </table>
</div>

---

## Installation & Setup

### PlatformIO (Recommended)

1. Install **Visual Studio Code** with the PlatformIO extension
2. Clone / open this folder in VS Code
3. PlatformIO auto-detects `seeed_xiao_esp32s3` from `platformio.ini`
4. **Build:** `PlatformIO: Build`
5. **Upload:** Connect XIAO via USB-C → `PlatformIO: Upload`
6. **Monitor:** Serial monitor at **115200 baud**

### Arduino IDE

1. Board Manager URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. Install **esp32 by Espressif Systems** (v3.x+)
3. Board: **XIAO_ESP32S3** | PSRAM: **OPI PSRAM**
4. Install **LovyanGFX** via Library Manager
5. Open `src/main.cpp`, upload

---

## Usage

### Taking a Photo
- **Short press** (< 1 s) the shutter button
- Camera switches to HD mode, captures, saves → `/pic_N.jpg`
- Screen shows "HOLD ON…" then "PIC SAVED #N"

### Recording Video
- **Long press** (> 1 s) the shutter button to start
- Records MJPEG + PCM audio → `/vid_N.avi`
- Screen shows blinking red indicator + elapsed time HH:MM:SS
- **Press again** to stop (minimum 2 s of recording)

### Image Mirroring
- Press the **Boot button** (GPIO 0) to toggle horizontal flip

### SD Card Status
- **Green dot** (bottom-right) = SD ready
- **Red dot** = card missing, full, or error
- Recording auto-stops if card is removed or full

---

## Technical Details

### Recording Configuration

| Setting | Live Preview | Photo | Video |
|---|---|---|---|
| Format | RGB565 | JPEG | JPEG |
| Resolution | QVGA 320×240 | HD 1280×720 | HVGA 480×320 |
| Target FPS | — | — | 10 |
| JPEG Quality | — | 12 | 12 |

### A/V Synchronisation

Audio timing is pinned exactly to video frame rate:

```
AUDIO_SAMPLE_RATE       = 16 000 Hz
TARGET_FPS              = 10
AUDIO_SAMPLES_PER_FRAME = 1 600
AUDIO_BYTES_PER_FRAME   = 3 200

I2S DMA: dma_frame_num=160 → 320 bytes/slot
         3200 / 320 = 10 slots = exactly 1 video frame interval
```

Every video frame always receives one audio chunk (zero-padded if the DMA hasn't filled completely). `microSecPerFrame` in the AVI header is written from `TARGET_FPS`, not a variable post-recording average.

### File Storage

| File | Path pattern | Counter storage |
|---|---|---|
| Photo | `/pic_N.jpg` | EEPROM |
| Video | `/vid_N.avi` | EEPROM |

### FreeRTOS Task Architecture

| Task | Core | Priority | Stack | Responsibility |
|---|---|---|---|---|
| taskSDMonitor | 0 | 1 | 4 KB | SD health, free space, removal detection |
| taskRecorder | 0 | 4 | 8 KB | SD write, I2S audio read, AVI chunk output |
| taskDisplay | 1 | 3 | 4 KB | TFT frame rendering (ping-pong buffer) |
| taskCapture | 1 | 5 | 8 KB | Camera capture, state machine, UI events |
| taskInput | 1 | 2 | 2 KB | Button debounce, input event dispatch |

**Synchronisation primitives:**
- `spiMutex` — serialises all SPI bus access (TFT + SD share `SPI2_HOST`)
- `std::atomic<int>` — dispWriteSlot, dispReadSlot, dispRendering (inter-core display state)
- `recFrameQueue` — decouples capture from SD write (3-slot PSRAM pool)
- `recPoolFree` semaphore — backpressure: slows capture when SD is slow

---

## Assembly & Power Notes

Components are soldered on perfboard, powered by a 3.7 V 18650 Li-ion cell.

> **Important:** When powered via the battery connector, the XIAO's 5 V pin is disabled. Connect the **TFT VCC and LED to 3.3 V** to ensure correct operation on battery power.

---

## Troubleshooting

| Symptom | Check |
|---|---|
| "Cam init error" in serial | USB power ≥ 500 mA; OV2640 ribbon cable seated |
| Inverted colours on TFT | Toggle `pcfg.invert` in `initCamera()` |
| SD not detected | FAT32 format; < 32 GB recommended; check GPIO 21 CS |
| AVI won't play | Use VLC; some players don't support MJPEG in AVI |

---

## Performance

| Metric | Typical value |
|---|---|
| Viewfinder FPS | 15–25 FPS |
| Photo save time | 3–5 seconds |
| Video recording FPS | 9–10 FPS |
| RAM usage | ~14% of 320 KB |
| Flash usage | ~19% of 3.3 MB |

---

## License

MIT License — see `LICENSE` for details.
