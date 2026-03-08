# XIAO ESP32-S3 Sense Retro Camera

A compact digital camera implementation using the Seeed Studio XIAO ESP32-S3 Sense development board and a 2.0" TFT LCD display (ST7789VW). Features high-resolution photo capture, AVI video recording with audio, and a live viewfinder with FreeRTOS multitasking for robust operation.

## Features

- **High-Resolution Photo Capture:** Saves HD JPEG images (FRAMESIZE_HD: 1280×720) to the SD card.
- **Video Recording:** Records MJPEG video with PCM audio in AVI container format (FRAMESIZE_HVGA: 480×320, 7-10 FPS, 16 kHz mono audio via built-in PDM microphone).
- **Live Viewfinder:** Real-time camera feed (320×240 RGB565) displayed on the 2.0" TFT display with up to 320×216 pixel preview area.
- **Hardware Mirroring:** Toggle horizontal image mirroring using the built-in Boot button (selfie mode support).
- **Robust Multitasking:** Uses FreeRTOS dual-task architecture for concurrent camera feed processing, SD card monitoring, and real-time UI updates.
- **Performance Monitoring:** Live FPS counter and SD card status indicator on screen.
- **Error Handling:** Comprehensive error detection with on-screen notifications for camera failures, SD card issues, and write errors.
- **PSRAM Optimization:** Efficient use of ESP32-S3 PSRAM for frame buffering and AVI index storage.

## Hardware Required

- **Microcontroller:** Seeed Studio XIAO ESP32-S3 Sense (with OV2640 camera & SD Card expansion board)
- **Display:** 2.0" TFT LCD Display Module (ST7789VW driver, 240×320 resolution, SPI interface)
- **Buttons:** 
  - 1× Dual-leg shutter button (for photo/video control)
  - 1× Boot button on XIAO board (for image mirroring)
- **Storage:** MicroSD Card (FAT32 formatted)
- **Power:** 3.7V Li-ion battery or USB-C power (via XIAO board)
- **Connecting Wires & Solder** (for perfboard assembly)

## Pin Configuration

<div align="center">
  <img src="images/connections.png" width="600" alt="Connection Diagram">
  <br><br>
  <table>
    <tr>
      <td valign="top" align="center">
        <h3>TFT Display (ST7789VW)</h3>
        <table>
          <tr>
            <th>TFT Pin</th>
            <th>XIAO ESP32-S3 Pin</th>
          </tr>
          <tr>
            <td>GND</td>
            <td>GND</td>
          </tr>
          <tr>
            <td>VCC</td>
            <td>3.3V</td>
          </tr>
          <tr>
            <td>SCL</td>
            <td>GPIO 7</td>
          </tr>
          <tr>
            <td>SDA</td>
            <td>GPIO 9</td>
          </tr>
          <tr>
            <td>RESET</td>
            <td>GPIO 3</td>
          </tr>
          <tr>
            <td>DC</td>
            <td>GPIO 2</td>
          </tr>
          <tr>
            <td>CS</td>
            <td>GPIO 1</td>
          </tr>
        </table>
      </td>
      <td width="20"></td> <td valign="top" align="center">
        <h3>Shutter Button & Control</h3>
        <table>
          <tr>
            <th>Button Function</th>
            <th>XIAO ESP32-S3 Pin</th>
          </tr>
          <tr>
            <td>Shutter Button 1</td>
            <td>GPIO 4 (Output)</td>
          </tr>
          <tr>
            <td>Shutter Button 2</td>
            <td>GPIO 5 (Input)</td>
          </tr>
          <tr>
            <td>Boot Button (Mirroring)</td>
            <td>GPIO 0 (Built-in)</td>
          </tr>
        </table>
      </td>
    </tr>
  </table>
</div>

## Assembly & Battery Power

To create a portable retro camera, components are soldered onto a perfboard and powered by a 3.7V 18650 Li-ion battery.

**Important Power Requirement:**
When the Seeed Studio XIAO ESP32-S3 is powered via the battery connector, the 5V output is disabled. Therefore, the **TFT Display VCC and LED pins must be connected to the 3.3V pin** instead of 5V to ensure proper operation on battery power.

## Installation & Setup

### PlatformIO Installation (Recommended)

1. **Install Visual Studio Code** with the PlatformIO extension
2. **Clone/Open Project:** Open the project folder in VS Code
3. **Configure Board:** PlatformIO automatically detects the `seeed_xiao_esp32s3` board from `platformio.ini`
4. **Configure Libraries:** Required libraries are specified in `platformio.ini`:
   - `lovyan03/LovyanGFX` (Display driver)
   - `espressif/esp32-camera` (Camera support)
5. **Build:** Run `PlatformIO: Build` from the command palette
6. **Upload:** Connect XIAO ESP32-S3 via USB-C and run `PlatformIO: Upload`
7. **Monitor:** Open the serial monitor at 115200 baud to view debug output

### Alternative: Arduino IDE Setup

1. **Install Arduino IDE:** Version 2.0 or later
2. **Add Board Manager URL:** In Settings, add:
   - https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
3. **Install ESP32 Package:** Board Manager > Search "esp32" > Install "esp32 by Espressif Systems" (v3.x or later)
4. **Select Board:** Tools > Board > esp32 > "XIAO_ESP32S3"
5. **Configure PSRAM:** Tools > PSRAM > "OPI PSRAM"
6. **Install LovyanGFX:** Library Manager > Search "LovyanGFX" > Install latest version
7. **Select Sketch:** Open `src/main.cpp` (or copy to Arduino IDE as `.ino`)
8. **Upload:** Connect board and upload

## Usage

### Taking a Photo
- **Action:** Short press (< 1 second) the shutter button (GPIO 5)
- **Process:** Camera switches to HD mode (FRAMESIZE_HD), captures image, saves to SD card
- **Display:** Shows "HOLD ON..." dialog while saving; "PIC SAVED #X" confirmation after success
- **Result:** High-resolution JPEG saved as `/hd_pic_N.jpg`

### Recording Video
- **Action:** Long press (> 1 second) the shutter button (GPIO 5)
- **Process:** Camera switches to video mode (FRAMESIZE_HVGA, 10 FPS), begins AVI encoding with audio capture from the built-in PDM microphone
- **Display:** 
  - Clears screen and enters recording mode
  - Shows blinking red recording indicator (top-left)
  - Displays elapsed time in HH:MM:SS format (top-left)
  - Video preview scaled to fit screen
- **Stop Recording:** Press shutter button again (requires > 2 seconds of recording)
- **Result:** MJPEG video with PCM audio saved as `/vid_N.avi`
- **Note:** Recording pauses live preview; FPS counter shows actual recording frame rate

### Image Mirroring
- **Action:** Press the small "Boot" button (GPIO 0) on the XIAO board
- **Result:** Camera feed horizontally flips (hardware mirroring)
- **Use:** Toggle between normal and selfie mode

### SD Card Monitoring
- **Automatic Monitoring:** Dedicated FreeRTOS task monitors SD card status every 2 seconds
- **Status Indicator:** Green dot (bottom-right of screen) = SD ready; Red dot = card missing/full
- **Recording Safety:** If SD card is removed or becomes full during recording, recording automatically stops with warning message

## Technical Details

### Display & Camera Configuration

| Setting | Live Preview | Photo Mode | Video Mode |
|---------|---|---|---|
| **Pixel Format** | RGB565 | JPEG | JPEG |
| **Resolution** | FRAMESIZE_QVGA (320×240) | FRAMESIZE_HD (1280×720) | FRAMESIZE_HVGA (480×320) |
| **Display Area** | 320×216 pixels + 24px FPS bar | N/A | ~240×160 scaled preview |
| **JPEG Quality** | 0 (lossless RGB) | 12 (high quality) | 12 (high quality) |

### File System & Storage

- **Photo Files:** Sequential naming: `/hd_pic_0.jpg`, `/hd_pic_1.jpg`, etc.
- **Video Files:** Sequential naming: `/vid_0.avi`, `/vid_1.avi`, etc.
- **Counter Persistence:** Picture and video counters stored in EEPROM (survives power loss)

### Video Encoding Details

- **Format:** MJPEG (Motion JPEG) video + PCM audio inside AVI container
- **Video Resolution:** 480×320 at 10 FPS (actual FPS auto-adjusted based on system load)
- **Audio:** 16 kHz, 16-bit, mono PCM via built-in PDM microphone
- **Duration Support:** PSRAM allocation supports ~36000 frames (~1 hour at 10 FPS)

### Multitasking Architecture

- **Task 1 (Core 0):** SD Card Monitor
  - Priority: Low (1)
  - Stack: 4 KB
  - Duty: Monitor SD card status, free space, removal detection
  
- **Task 2 (Core 1):** Camera & UI
  - Priority: High (5)
  - Stack: 8 KB
  - Duty: Camera capture, screen rendering, button handling, photo/video save

- **Synchronization:** FreeRTOS mutexes protect concurrent SD card and display access

## Interface & On-Screen Feedback

### Startup Screen
- "GETTING READY" message with progress
- Indicates initialization of camera, display, and SD card
- Shows credit: "github@barkinsarikartal"

### Live Viewfinder
- 320×216 pixel camera feed with RGB565 rendering
- Bottom status bar (24 pixels):
  - FPS counter (bottom-left, green text)
  - SD card status dot (bottom-right: green = ready, red = error)

## Troubleshooting

### Camera Not Initializing
- Check USB power supply (requires > 500 mA during capture)
- Verify GPIO pin connections to OV2640 camera module
- Check Serial output for "Cam init error" message

### Display Shows Inverted Colors
- Edit `src/main.cpp` line with `pcfg.invert = true/false`
- Some ST7789VW modules differ in color polarity
- Test with `pcfg.invert = false` if colors appear inverted

## Performance Notes

- **Live FPS:** Typically 15–25 FPS on 320×240 RGB565 (depends on how long the microcontroller has been running)
- **Photo Capture:** ~3–5 seconds total (mode switch + warm-up + save)
- **Video Recording:** 7–10 FPS achieved (limited by SD card write speed and JPEG encoding)

## License

This project is licensed under the MIT License. See the LICENSE file for details.
