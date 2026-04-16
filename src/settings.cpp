#include "shared.h"

void loadCameraSettings() {
  Preferences p;
  p.begin("cam", true);
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
  camSettings.flashlight_on      = p.getInt("flash", 0);
  p.end();
  Serial.println("[NVS] Camera settings loaded.");
}

void saveCameraSettings() {
  Preferences p;
  p.begin("cam", false);
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
  p.putInt("flash",  camSettings.flashlight_on);
  p.end();
  Serial.println("[NVS] Camera settings saved.");
}

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

  // OV2640 special effects (Sepia, tints, Gray, Negative) work by writing fixed
  // U/V values into the same SDE registers the boost below touches. Writing the
  // boost after set_special_effect() overwrites those targets and silently
  // neutralises the effect. Only boost saturation when FX is "None".
  if (camSettings.special_effect == 0) {
    // Direct SDE writes via BPADDR(0x7C) / BPDATA(0x7D) on the DSP bank push
    // saturation past the esp-idf API ceiling (~0x68 at level +2).
    //   enable byte 0x00: bit0=sat+hue, bit1=sat boost, bit2=bright+contrast
    //   U_SAT/V_SAT 0x88 = 136 (~30% above API max)
    s->set_reg(s, 0xff, 0xff, 0x00);
    s->set_reg(s, 0x7c, 0xff, 0x00);
    s->set_reg(s, 0x7d, 0xff, 0x07);
    s->set_reg(s, 0x7c, 0xff, 0x03);
    s->set_reg(s, 0x7d, 0xff, 0x88);
    s->set_reg(s, 0x7c, 0xff, 0x04);
    s->set_reg(s, 0x7d, 0xff, 0x88);
  }
}

void drawMenuMain() {
  static const char* const labels[MENU_MAIN_ITEMS] = {
    "Gallery", "WiFi Transfer", "Settings", "Timelapse", "Exit"
  };
  static const uint16_t accents[MENU_MAIN_ITEMS] = {
    TFT_CYAN, TFT_YELLOW, TFT_ORANGE, TFT_GREEN, 0x7BEF
  };

  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);

    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("MENU", tft.width() / 2, 18);

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

    tft.setTextSize(1);
    tft.setTextColor(0x4208);
    tft.drawString("Click: Select  Long: Exit", tft.width() / 2, 225);

    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }
}

void drawSettingsMenu() {
  static const char* const names[SETTINGS_COUNT] = {
    "Bright", "Contr", "Satur", "AE Lv",
    "WB", "FX", "Mirror", "Flip", "JPEG Q",
    "TL Int", "Rec Lim", "Light", "Rst Cnt"
  };
  static const char* const wbLabels[] = {
    "Auto", "Sunny", "Cloud", "Office", "Home"
  };
  static const char* const fxLabels[] = {
    "None", "Neg", "Gray", "Red", "Green", "Blue", "Sepia"
  };

  int vals[SETTINGS_COUNT] = {
    camSettings.brightness, camSettings.contrast, camSettings.saturation,
    camSettings.ae_level, camSettings.wb_mode, camSettings.special_effect,
    camSettings.hmirror, camSettings.vflip, camSettings.jpeg_quality,
    camSettings.timelapse_interval, camSettings.rec_max_seconds,
    camSettings.flashlight_on, resetCounterConfirm
  };

  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);

    tft.setTextSize(2);
    tft.setTextColor(TFT_ORANGE);
    tft.drawString("SETTINGS", tft.width() / 2, 14);

    int visibleItems = 7;
    int itemH = 25;
    int yStart = 34;

    int scrollTop = 0;
    if (settingsIndex >= visibleItems) {
      scrollTop = settingsIndex - visibleItems + 1;
    }

    for (int v = 0; v < visibleItems && (scrollTop + v) < SETTINGS_COUNT; v++) {
      int i = scrollTop + v;
      int y = yStart + v * itemH;
      bool selected = (i == settingsIndex);
      bool editing = selected && settingsEditing;

      if (editing) {
        tft.fillRoundRect(4, y, 312, 22, 4, TFT_YELLOW);
      } else if (selected) {
        tft.fillRoundRect(4, y, 312, 22, 4, TFT_CYAN);
      }

      tft.setTextDatum(middle_left);
      tft.setTextSize(1);
      tft.setTextColor((selected || editing) ? TFT_BLACK : TFT_WHITE);
      tft.drawString(names[i], 10, y + 11);

      tft.setTextDatum(middle_right);
      char valBuf[16];
      if (i == 12) {
        // Reset counters — action-type setting
        if (editing) {
          snprintf(valBuf, sizeof(valBuf), "%s", vals[i] ? "YES" : "No");
        } else {
          snprintf(valBuf, sizeof(valBuf), "P:%d V:%d", pictureNumber, videoNumber);
        }
      } else if (i == 4) {
        snprintf(valBuf, sizeof(valBuf), "%s", wbLabels[constrain(vals[i], 0, 4)]);
      } else if (i == 5) {
        snprintf(valBuf, sizeof(valBuf), "%s", fxLabels[constrain(vals[i], 0, 6)]);
      } else if (i == 6 || i == 7 || i == 11) {
        // Mirror, Flip, Light — on/off toggles
        snprintf(valBuf, sizeof(valBuf), "%s", vals[i] ? "On" : "Off");
      } else if (i == 9 || i == 10) {
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

    tft.setTextDatum(middle_center);
    tft.setTextSize(1);
    tft.setTextColor(0x4208);
    if (scrollTop > 0) {
      tft.drawString("^", tft.width() / 2, yStart - 6);
    }
    if (scrollTop + visibleItems < SETTINGS_COUNT) {
      tft.drawString("v", tft.width() / 2, yStart + visibleItems * itemH + 2);
    }

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
