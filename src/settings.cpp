#include "shared.h"

// ================= CAMERA SETTINGS NVS =================
void loadCameraSettings() {
  Preferences p;
  p.begin("cam", true);  // read-only
  camSettings.brightness    = p.getInt("bright", 1);
  camSettings.contrast      = p.getInt("contr",  0);
  camSettings.saturation    = p.getInt("sat",    2);
  camSettings.ae_level      = p.getInt("ae",     0);
  camSettings.wb_mode       = p.getInt("wb",     0);
  camSettings.special_effect = p.getInt("fx",    0);
  camSettings.hmirror       = p.getInt("hmirr",  1);
  camSettings.vflip         = p.getInt("vflip",  0);
  camSettings.jpeg_quality  = p.getInt("jpgq",  12);
  camSettings.timelapse_interval = p.getInt("tlint", 10);
  camSettings.rec_max_seconds    = p.getInt("reclim", 0);
  p.end();
  Serial.println("[NVS] Camera settings loaded.");
}

void saveCameraSettings() {
  Preferences p;
  p.begin("cam", false);  // read-write
  p.putInt("bright", camSettings.brightness);
  p.putInt("contr",  camSettings.contrast);
  p.putInt("sat",    camSettings.saturation);
  p.putInt("ae",     camSettings.ae_level);
  p.putInt("wb",     camSettings.wb_mode);
  p.putInt("fx",     camSettings.special_effect);
  p.putInt("hmirr",  camSettings.hmirror);
  p.putInt("vflip",  camSettings.vflip);
  p.putInt("jpgq",   camSettings.jpeg_quality);
  p.putInt("tlint",  camSettings.timelapse_interval);
  p.putInt("reclim", camSettings.rec_max_seconds);
  p.end();
  Serial.println("[NVS] Camera settings saved.");
}

// Applies CameraSettings to the active sensor. Called after every initCamera.
// Mirror is always driven from camSettings.hmirror (no separate runtime toggle).
void applySettings(sensor_t *s) {
  if (!s) return;
  s->set_brightness(s, camSettings.brightness);
  s->set_contrast(s, camSettings.contrast);
  s->set_saturation(s, camSettings.saturation);
  s->set_ae_level(s, camSettings.ae_level);
  s->set_wb_mode(s, camSettings.wb_mode);
  s->set_special_effect(s, camSettings.special_effect);
  s->set_hmirror(s, camSettings.hmirror);
  s->set_vflip(s, camSettings.vflip);

  // OV2640 SDE saturation boost — direct register access, bypasses the API ceiling of +2.
  // The esp-idf set_saturation() API only sets U_SAT/V_SAT to ~0x68 at level +2, which is
  // noticeably less saturated than a modern phone camera. We override those values AND
  // ensure the SDE enable mask has bit 0 (saturation) and bit 1 (saturation boost) set,
  // otherwise the U_SAT/V_SAT values are silently ignored by the DSP.
  //
  // SDE control byte 0x00 bitmask:
  //   bit 0 = Saturation + Hue enable
  //   bit 1 = Saturation boost enable
  //   bit 2 = Brightness + Contrast enable
  // We set 0x07 to keep sat+boost+brightness/contrast all active.
  //
  // Register sequence via indirect BPADDR(0x7C) / BPDATA(0x7D):
  //   1. Switch to DSP bank (0xFF = 0x00)
  //   2. Write enable mask → address 0x00, data 0x07
  //   3. Write U_SAT       → address 0x03, data 0x88
  //   4. Write V_SAT       → address 0x04, data 0x88
  s->set_reg(s, 0xff, 0xff, 0x00);  // bank: DSP
  s->set_reg(s, 0x7c, 0xff, 0x00);  // SDE indirect address → control/enable
  s->set_reg(s, 0x7d, 0xff, 0x07);  // enable: sat + sat_boost + bright/contrast
  s->set_reg(s, 0x7c, 0xff, 0x03);  // SDE indirect address → U_SAT
  s->set_reg(s, 0x7d, 0xff, 0x88);  // U_SAT = 0x88 = 136  (API max ≈ 0x68 = 104, ~30% boost)
  s->set_reg(s, 0x7c, 0xff, 0x04);  // SDE indirect address → V_SAT
  s->set_reg(s, 0x7d, 0xff, 0x88);  // V_SAT = 0x88 = 136
}

// ================= MENU DRAW =================

// Draw the main menu (Gallery / WiFi / Settings / Exit).
void drawMenuMain() {
  static const char* const labels[MENU_MAIN_ITEMS] = {
    "Gallery", "WiFi Transfer", "Settings", "Timelapse", "Exit"
  };
  // Accent colours per item for visual variety
  static const uint16_t accents[MENU_MAIN_ITEMS] = {
    TFT_CYAN, TFT_YELLOW, TFT_ORANGE, TFT_GREEN, 0x7BEF  // cyan, yellow, orange, green, grey
  };

  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);

    // Title
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("MENU", tft.width() / 2, 18);

    // Menu items — tighter spacing for 5 items
    int yStart = 45;
    int itemH = 35;
    for (int i = 0; i < MENU_MAIN_ITEMS; i++) {
      int y = yStart + i * itemH;
      if (i == menuMainSelection) {
        tft.fillRoundRect(50, y, 220, 30, 8, accents[i]);
        tft.setTextSize(2);
        tft.setTextColor(TFT_BLACK);
      } else {
        tft.drawRoundRect(50, y, 220, 30, 8, 0x4208);
        tft.setTextSize(2);
        tft.setTextColor(0x7BEF);
      }
      tft.drawString(labels[i], tft.width() / 2, y + 15);
    }

    // Footer hint
    tft.setTextSize(1);
    tft.setTextColor(0x4208);
    tft.drawString("Click: Select  Long: Exit", tft.width() / 2, 225);

    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }
}

// Draw the camera settings menu.
// Shows all 9 settings with current values; the highlighted item is shown
// with inverted colours. In editing mode, the value flashes with a different
// background to indicate it can be changed with CW/CCW.
void drawSettingsMenu() {
  // Human-readable labels matching CameraSettings struct field order
  static const char* const names[SETTINGS_COUNT] = {
    "Bright", "Contr", "Satur", "AE Lv",
    "WB", "FX", "Mirror", "Flip", "JPEG Q",
    "TL Int", "Rec Lim", "Rst Cnt"
  };
  // White balance mode labels
  static const char* const wbLabels[] = {
    "Auto", "Sunny", "Cloud", "Office", "Home"
  };
  // Special effect labels
  static const char* const fxLabels[] = {
    "None", "Neg", "Gray", "Red", "Green", "Blue", "Sepia"
  };

  // Read current values from camSettings into an array for uniform rendering
  int vals[SETTINGS_COUNT] = {
    camSettings.brightness, camSettings.contrast, camSettings.saturation,
    camSettings.ae_level, camSettings.wb_mode, camSettings.special_effect,
    camSettings.hmirror, camSettings.vflip, camSettings.jpeg_quality,
    camSettings.timelapse_interval, camSettings.rec_max_seconds,
    resetCounterConfirm  // 0=No, 1=Yes
  };

  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);

    // Title
    tft.setTextSize(2);
    tft.setTextColor(TFT_ORANGE);
    tft.drawString("SETTINGS", tft.width() / 2, 14);

    // Visible window: show up to 7 items at a time with scrolling
    int visibleItems = 7;
    int itemH = 25;
    int yStart = 34;

    // Calculate scroll window so the selected item stays visible
    int scrollTop = 0;
    if (settingsIndex >= visibleItems) {
      scrollTop = settingsIndex - visibleItems + 1;
    }

    for (int v = 0; v < visibleItems && (scrollTop + v) < SETTINGS_COUNT; v++) {
      int i = scrollTop + v;
      int y = yStart + v * itemH;
      bool selected = (i == settingsIndex);
      bool editing = selected && settingsEditing;

      // Background
      if (editing) {
        tft.fillRoundRect(4, y, 312, 22, 4, TFT_YELLOW);
      } else if (selected) {
        tft.fillRoundRect(4, y, 312, 22, 4, TFT_CYAN);
      }

      // Label (left)
      tft.setTextDatum(middle_left);
      tft.setTextSize(1);
      tft.setTextColor((selected || editing) ? TFT_BLACK : TFT_WHITE);
      tft.drawString(names[i], 10, y + 11);

      // Value (right)
      tft.setTextDatum(middle_right);
      char valBuf[16];
      if (i == 11) {
        // Counter reset — show No/Yes in edit mode, P:N V:N otherwise
        if (editing) {
          snprintf(valBuf, sizeof(valBuf), "%s", vals[i] ? "YES" : "No");
        } else {
          snprintf(valBuf, sizeof(valBuf), "P:%d V:%d", pictureNumber, videoNumber);
        }
      } else if (i == 4) {
        // WB mode — show label
        snprintf(valBuf, sizeof(valBuf), "%s", wbLabels[constrain(vals[i], 0, 4)]);
      } else if (i == 5) {
        // FX — show label
        snprintf(valBuf, sizeof(valBuf), "%s", fxLabels[constrain(vals[i], 0, 6)]);
      } else if (i == 6 || i == 7) {
        // Boolean — show On/Off
        snprintf(valBuf, sizeof(valBuf), "%s", vals[i] ? "On" : "Off");
      } else if (i == 9 || i == 10) {
        // Time-based — format seconds to human-readable
        if (i == 10 && vals[i] == 0) {
          snprintf(valBuf, sizeof(valBuf), "Off");
        } else if (vals[i] < 60) {
          snprintf(valBuf, sizeof(valBuf), "%ds", vals[i]);
        } else {
          snprintf(valBuf, sizeof(valBuf), "%dm", vals[i] / 60);
        }
      } else {
        snprintf(valBuf, sizeof(valBuf), "%d", vals[i]);
      }
      tft.setTextColor((selected || editing) ? TFT_BLACK : TFT_GREEN);
      tft.drawString(valBuf, 310, y + 11);
    }

    // Scroll indicators
    tft.setTextDatum(middle_center);
    tft.setTextSize(1);
    tft.setTextColor(0x4208);
    if (scrollTop > 0) {
      tft.drawString("^", tft.width() / 2, yStart - 6);
    }
    if (scrollTop + visibleItems < SETTINGS_COUNT) {
      tft.drawString("v", tft.width() / 2, yStart + visibleItems * itemH + 2);
    }

    // Footer hint
    tft.setTextColor(0x4208);
    if (settingsEditing) {
      tft.drawString("Turn: Value  Click: OK  Long: Cancel", tft.width() / 2, 225);
    } else {
      tft.drawString("Turn: Browse  Click: Edit  Long: Back", tft.width() / 2, 225);
    }

    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }
}
