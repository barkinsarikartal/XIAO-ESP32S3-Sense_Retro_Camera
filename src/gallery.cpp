#include "shared.h"

// ================= GALLERY FUNCTIONS =================

// Scan SD root for .jpg or .avi files into PSRAM-backed array.
// Call freeGalleryFiles() before re-scanning or exiting gallery.
void scanGalleryFiles(bool videosOnly) {
  freeGalleryFiles();
  if (!globalSDState.isMounted) return;

  const char *ext = videosOnly ? ".avi" : ".jpg";

  // Allocate pointer array in PSRAM (2000 * 4 = 8KB — negligible)
  galleryFiles = (char**)ps_malloc(GALLERY_MAX_FILES * sizeof(char*));
  if (!galleryFiles) {
    Serial.println("[GAL] PSRAM alloc failed for file index");
    return;
  }

  int count = 0;
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    File root = SD.open("/");
    if (root) {
      File f = root.openNextFile();
      while (f && count < GALLERY_MAX_FILES) {
        if (!f.isDirectory()) {
          String name = f.name();
          if (name.endsWith(ext)) {
            galleryFiles[count] = (char*)ps_malloc(GALLERY_NAME_LEN);
            if (galleryFiles[count]) {
              // f.name() may or may not include leading '/' depending on
              // ESP32 Arduino core version. Normalize to always have '/'.
              if (name.startsWith("/")) {
                strncpy(galleryFiles[count], name.c_str(), GALLERY_NAME_LEN - 1);
              } else {
                snprintf(galleryFiles[count], GALLERY_NAME_LEN, "/%s", name.c_str());
              }
              galleryFiles[count][GALLERY_NAME_LEN - 1] = '\0';
              count++;
            }
          }
        }
        f = root.openNextFile();
      }
      root.close();
    }
    xSemaphoreGive(spiMutex);
  }

  galleryFileCount = count;

  // Sort descending by embedded number (newest/highest first).
  // Filenames follow patterns like /hd_pic_42.jpg, /vid_7.avi.
  // Extract trailing number for numeric comparison so pic_10 > pic_9.
  if (galleryFileCount > 1) {
    // Simple insertion sort — gallery sizes are small (hundreds of files)
    for (int i = 1; i < galleryFileCount; i++) {
      char *key = galleryFiles[i];
      // Extract number from key
      const char *p = key + strlen(key) - 1;
      while (p > key && *p != '_') p--;
      int keyNum = (p > key) ? atoi(p + 1) : 0;

      int j = i - 1;
      while (j >= 0) {
        const char *q = galleryFiles[j] + strlen(galleryFiles[j]) - 1;
        while (q > galleryFiles[j] && *q != '_') q--;
        int jNum = (q > galleryFiles[j]) ? atoi(q + 1) : 0;
        if (jNum < keyNum) {
          galleryFiles[j + 1] = galleryFiles[j];
          j--;
        } else {
          break;
        }
      }
      galleryFiles[j + 1] = key;
    }
  }

  galleryIndex = 0;
  Serial.printf("[GAL] Found %d %s files\n", galleryFileCount, videosOnly ? "video" : "photo");
}

// Release gallery file index memory.
void freeGalleryFiles() {
  if (galleryFiles) {
    for (int i = 0; i < galleryFileCount; i++) {
      if (galleryFiles[i]) free(galleryFiles[i]);
    }
    free(galleryFiles);
    galleryFiles = nullptr;
  }
  galleryFileCount = 0;
  galleryIndex = 0;
}

// Draw the gallery type selection menu (Photos / Videos).
void drawGalleryTypeMenu() {
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);

    // Title
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN);
    tft.drawString("GALLERY", tft.width() / 2, 30);

    // Photos option
    tft.setTextSize(2);
    if (galleryTypeSelection == 0) {
      tft.fillRoundRect(60, 75, 200, 40, 8, TFT_CYAN);
      tft.setTextColor(TFT_BLACK);
    } else {
      tft.drawRoundRect(60, 75, 200, 40, 8, 0x4208);
      tft.setTextColor(0x7BEF);
    }
    tft.drawString("Photos", tft.width() / 2, 95);

    // Videos option
    tft.setTextSize(2);
    if (galleryTypeSelection == 1) {
      tft.fillRoundRect(60, 135, 200, 40, 8, TFT_CYAN);
      tft.setTextColor(TFT_BLACK);
    } else {
      tft.drawRoundRect(60, 135, 200, 40, 8, 0x4208);
      tft.setTextColor(0x7BEF);
    }
    tft.drawString("Videos", tft.width() / 2, 155);

    // Footer hint
    tft.setTextSize(1);
    tft.setTextColor(0x4208);
    tft.drawString("Click: Select  Long: Back", tft.width() / 2, 225);

    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }
}

// Display a single photo from the gallery.
// Reads JPEG file into dispBuf[0] (safe because taskCapture skips copy during gallery),
// then decodes with LovyanGFX drawJpg at 0.25f scale (HD 1280x720 -> 320x180).
void drawGalleryPhoto() {
  if (galleryFileCount == 0 || galleryIndex >= galleryFileCount) return;
  if (!globalSDState.isMounted) return;

  const char *filePath = galleryFiles[galleryIndex];

  // Read JPEG into dispBuf[0] (PSRAM, camera copy paused during gallery)
  uint8_t *jpgBuf = dispBuf[0];
  size_t jpgLen = 0;

  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    File f = SD.open(filePath, FILE_READ);
    if (f) {
      size_t fsize = f.size();
      if (fsize <= DISP_BUF_SIZE && jpgBuf) {
        jpgLen = f.read(jpgBuf, fsize);
      }
      f.close();
    }
    xSemaphoreGive(spiMutex);
  }

  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);

    // Header: index / total
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    char header[32];
    snprintf(header, sizeof(header), "%d / %d", galleryIndex + 1, galleryFileCount);
    tft.drawString(header, tft.width() / 2, 8);

    // Filename (strip leading /)
    tft.setTextColor(TFT_CYAN);
    const char *dispName = filePath;
    if (dispName[0] == '/') dispName++;
    tft.drawString(dispName, tft.width() / 2, 20);

    if (jpgLen > 0) {
      // HD (1280x720) at 0.25f = 320x180, drawn at y=30
      tft.drawJpg(jpgBuf, jpgLen, 0, 30, 320, 180, 0, 0, 0.25f);
    } else {
      tft.setTextSize(2);
      tft.setTextColor(TFT_RED);
      tft.drawString("Cannot display", tft.width() / 2, 120);
    }

    // Footer
    tft.setTextSize(1);
    tft.setTextColor(0x4208);
    tft.drawString("Click:Delete  Long:Back", tft.width() / 2, 225);

    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }
}

// Extract the Nth JPEG frame from an AVI file's MOVI list.
// Returns the JPEG data size written to buf, or 0 on failure.
// Caller must hold spiMutex (SD file access) and ensure buf is large enough.
size_t extractAVIFrame(File &aviFile, uint8_t *buf, size_t bufSize, int targetFrame) {
  if (!aviFile || !buf || bufSize == 0) return 0;

  aviFile.seek(AVI_HEADER_SIZE);  // skip AVI header, enter MOVI

  int videoFrameIdx = 0;
  uint8_t tag[4];

  while (aviFile.read(tag, 4) == 4) {
    uint32_t chunkSize = 0;
    if (aviFile.read((uint8_t*)&chunkSize, 4) != 4) break;

    // Stop at idx1 marker
    if (tag[0] == 'i' && tag[1] == 'd' && tag[2] == 'x' && tag[3] == '1') break;

    // "00dc" = video frame (JPEG)
    if (tag[0] == 0x30 && tag[1] == 0x30 && tag[2] == 0x64 && tag[3] == 0x63) {
      if (videoFrameIdx == targetFrame && chunkSize > 0 && chunkSize <= bufSize) {
        size_t rd = aviFile.read(buf, chunkSize);
        return (rd == chunkSize) ? chunkSize : 0;
      }
      videoFrameIdx++;
    }

    // Skip this chunk's data (seek past it)
    aviFile.seek(aviFile.position() + chunkSize);
  }

  return 0;  // target frame not found
}

// Display a single video item with thumbnail background + overlaid play icon and info.
// Extracts frame 12 (0-indexed) from the AVI as thumbnail. Falls back to plain icon
// if extraction fails.
void drawGalleryVideoItem() {
  if (galleryFileCount == 0 || galleryIndex >= galleryFileCount) return;

  const char *filePath = galleryFiles[galleryIndex];

  // Try to extract a thumbnail frame (frame 12 ≈ 1.2s into video at 10fps)
  uint8_t *thumbBuf = dispBuf[0];  // safe — camera copy paused during gallery
  size_t thumbLen = 0;
  float sizeMB = 0;

  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    File aviFile = SD.open(filePath, FILE_READ);
    if (aviFile) {
      sizeMB = aviFile.size() / 1048576.0f;
      thumbLen = extractAVIFrame(aviFile, thumbBuf, DISP_BUF_SIZE, 12);
      aviFile.close();
    }
    xSemaphoreGive(spiMutex);
  }

  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);

    // Background: thumbnail or plain black
    if (thumbLen > 0) {
      // HVGA (480x320) at 0.5f = 240x160, centered on 320x240 screen
      tft.drawJpg(thumbBuf, thumbLen, 40, 20, 240, 160, 0, 0, 0.5f);
    }

    // Header: index / total (top bar, on a dark strip)
    tft.fillRect(0, 0, 320, 16, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    char header[32];
    snprintf(header, sizeof(header), "%d / %d", galleryIndex + 1, galleryFileCount);
    tft.drawString(header, tft.width() / 2, 8);

    // Play icon (triangle) with dark surround for visibility
    int cx = tft.width() / 2;
    int cy = 100;  // center of play icon
    // Dark circle behind play button
    tft.fillCircle(cx + 2, cy, 32, tft.color565(0, 0, 0));
    tft.drawCircle(cx + 2, cy, 33, tft.color565(60, 60, 60));
    // Play triangle
    tft.fillTriangle(cx - 12, cy - 18, cx - 12, cy + 18, cx + 20, cy, TFT_CYAN);

    // Filename on a dark background strip (strip leading /)
    const char *dispName = filePath;
    if (dispName[0] == '/') dispName++;
    int nameW = strlen(dispName) * 12 + 16;  // approximate width at textSize 2
    int nameX = (320 - nameW) / 2;
    if (nameX < 0) nameX = 0;
    tft.fillRect(nameX, 138, nameW, 22, tft.color565(0, 0, 0));
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(dispName, tft.width() / 2, 149);

    // File size on a dark background strip
    char sizeStr[16];
    snprintf(sizeStr, sizeof(sizeStr), "%.1f MB", sizeMB);
    int sizeW = strlen(sizeStr) * 6 + 12;  // approximate width at textSize 1
    int sizeX = (320 - sizeW) / 2;
    tft.fillRect(sizeX, 164, sizeW, 14, tft.color565(0, 0, 0));
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    tft.drawString(sizeStr, tft.width() / 2, 171);

    // Footer
    tft.fillRect(0, 218, 320, 22, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(0x4208);
    tft.drawString("Click:Play  Long:Back", tft.width() / 2, 225);

    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }
}

// Draw the delete confirmation dialog.
void drawDeleteConfirm() {
  if (galleryFileCount == 0 || galleryIndex >= galleryFileCount) return;

  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);

    // Title
    tft.setTextSize(2);
    tft.setTextColor(TFT_RED);
    tft.drawString("DELETE FILE?", tft.width() / 2, 30);

    // Filename
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    const char *dispName = galleryFiles[galleryIndex];
    if (dispName[0] == '/') dispName++;
    tft.drawString(dispName, tft.width() / 2, 60);

    // First option: Cancel (photos) or Play (videos)
    tft.setTextSize(2);
    bool isVideo = (galleryTypeSelection == 1);
    if (deleteSelection == 0) {
      tft.fillRoundRect(60, 90, 200, 40, 8, isVideo ? TFT_CYAN : TFT_GREEN);
      tft.setTextColor(TFT_BLACK);
    } else {
      tft.drawRoundRect(60, 90, 200, 40, 8, 0x4208);
      tft.setTextColor(0x7BEF);
    }
    tft.drawString(isVideo ? "Play" : "Cancel", tft.width() / 2, 110);

    // Delete option
    tft.setTextSize(2);
    if (deleteSelection == 1) {
      tft.fillRoundRect(60, 150, 200, 40, 8, TFT_RED);
      tft.setTextColor(TFT_WHITE);
    } else {
      tft.drawRoundRect(60, 150, 200, 40, 8, 0x4208);
      tft.setTextColor(0x7BEF);
    }
    tft.drawString("Delete", tft.width() / 2, 170);

    // Footer
    tft.setTextSize(1);
    tft.setTextColor(0x4208);
    tft.drawString("Long press: Back", tft.width() / 2, 225);

    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }
}

// Silent AVI video playback on TFT.
// Parses MOVI LIST for 00dc (JPEG) chunks, skips 01wb (audio) chunks.
// Uses dispBuf[0] as frame read buffer (safe — camera copy is paused).
// Runs in taskDisplay context; consumes encoder events for pause/stop.
void playVideoOnTFT() {
  if (galleryFileCount == 0 || galleryIndex >= galleryFileCount) {
    appState = STATE_GALLERY_VIDEOS;
    galleryNeedsRedraw = true;
    return;
  }

  const char *filePath = galleryFiles[galleryIndex];

  File aviFile;
  bool opened = false;
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    aviFile = SD.open(filePath, FILE_READ);
    opened = (bool)aviFile;
    xSemaphoreGive(spiMutex);
  }

  if (!opened) {
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      tft.fillScreen(TFT_BLACK);
      tft.setTextDatum(middle_center);
      tft.setTextSize(2);
      tft.setTextColor(TFT_RED);
      tft.drawString("Cannot open file", tft.width() / 2, 110);
      tft.setTextDatum(top_left);
      xSemaphoreGive(spiMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    appState = STATE_GALLERY_VIDEOS;
    galleryNeedsRedraw = true;
    return;
  }

  Serial.printf("[GAL] Playing: %s\n", filePath);

  // Clear screen for playback
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    xSemaphoreGive(spiMutex);
  }

  // Read microSecPerFrame from AVI header (offset 32, 4 bytes LE) so playback
  // speed matches the actual recording rate, even if capture FPS varied.
  uint32_t microSecPerFrame = 0;
  uint32_t frameTimeMs = 1000 / TARGET_FPS;  // fallback: 100ms
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    aviFile.seek(32);
    if (aviFile.read((uint8_t*)&microSecPerFrame, 4) == 4 &&
        microSecPerFrame > 0 && microSecPerFrame < 1000000) {
      frameTimeMs = microSecPerFrame / 1000;
      Serial.printf("[GAL] AVI microSecPerFrame=%u → %u ms/frame\n",
                    microSecPerFrame, frameTimeMs);
    } else {
      Serial.printf("[GAL] Bad microSecPerFrame (%u), using default %u ms\n",
                    microSecPerFrame, frameTimeMs);
    }
    // Seek to MOVI chunk data start (past header)
    aviFile.seek(AVI_HEADER_SIZE);
    xSemaphoreGive(spiMutex);
  }

  uint8_t *frameBuf = dispBuf[0];  // reuse display buffer (camera copy paused)
  bool paused = false;
  bool stopped = false;
  int frameNum = 0;
  int unknownChunks = 0;

  while (!stopped && appState == STATE_VIDEO_PLAYING) {
    // Check for input events (pause/stop)
    InputEvent ev;
    while (xQueueReceive(inputEventQueue, &ev, 0) == pdTRUE) {
      if (ev.type == INPUT_ENC_CLICK) {
        paused = !paused;
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50))) {
          if (paused) {
            // Draw pause indicator (two vertical bars)
            tft.fillRect(145, 5, 30, 20, TFT_BLACK);
            tft.fillRect(148, 7, 8, 16, TFT_WHITE);
            tft.fillRect(160, 7, 8, 16, TFT_WHITE);
          } else {
            // Clear pause indicator on resume
            tft.fillRect(145, 5, 30, 20, TFT_BLACK);
          }
          xSemaphoreGive(spiMutex);
        }
      } else if (ev.type == INPUT_ENC_LONG) {
        stopped = true;
      }
    }

    if (paused) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    uint32_t frameStart = millis();

    // ── Single mutex lock: read chunk header + data + draw ──
    // Batching eliminates 3 extra FreeRTOS context switches per frame.
    uint8_t tag[4];
    uint32_t chunkSize = 0;
    size_t bytesRead = 0;
    bool endOfData = false;
    bool drewFrame = false;  // only apply frame timing after a video frame

    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      bytesRead = aviFile.read(tag, 4);
      if (bytesRead == 4) {
        aviFile.read((uint8_t*)&chunkSize, 4);
      }

      if (bytesRead < 4) {
        endOfData = true;
      } else if (tag[0] == 'i' && tag[1] == 'd' && tag[2] == 'x' && tag[3] == '1') {
        endOfData = true;
      }
      // "00dc" = video frame (JPEG)
      else if (tag[0] == 0x30 && tag[1] == 0x30 && tag[2] == 0x64 && tag[3] == 0x63) {
        unknownChunks = 0;
        if (chunkSize > 0 && chunkSize <= DISP_BUF_SIZE && frameBuf) {
          size_t rd = aviFile.read(frameBuf, chunkSize);
          if (rd == chunkSize) {
            tft.drawJpg(frameBuf, chunkSize, 40, 40, 240, 160, 0, 0, 0.5f);
            frameNum++;
            drewFrame = true;
            // Frame counter overlay
            tft.setTextSize(1);
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.setCursor(4, 4);
            tft.printf("F:%d", frameNum);
          }
        } else {
          aviFile.seek(aviFile.position() + chunkSize);
        }
      }
      // "01wb" = audio chunk — skip
      else if (tag[0] == 0x30 && tag[1] == 0x31 && tag[2] == 0x77 && tag[3] == 0x62) {
        unknownChunks = 0;
        aviFile.seek(aviFile.position() + chunkSize);
      }
      // Unknown chunk — skip
      else {
        unknownChunks++;
        if (unknownChunks > 10) {
          Serial.println("[GAL] Too many unknown chunks, aborting playback.");
          endOfData = true;
        } else {
          aviFile.seek(aviFile.position() + chunkSize);
        }
      }
      xSemaphoreGive(spiMutex);
    }

    if (endOfData) break;

    // Frame timing — only after rendering a video frame.
    // Audio/unknown chunks loop back immediately with no delay so they
    // don't double the effective per-frame interval.
    if (drewFrame) {
      uint32_t elapsed = millis() - frameStart;
      if (elapsed < frameTimeMs) {
        vTaskDelay(pdMS_TO_TICKS(frameTimeMs - elapsed));
      }
    }
  }

  // Close file and return to video list
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    aviFile.close();
    xSemaphoreGive(spiMutex);
  }

  Serial.printf("[GAL] Playback ended. Frames: %d, stopped: %d\n", frameNum, (int)stopped);

  if (stopped) {
    // User pressed ENC_LONG during playback → offer delete
    deleteSelection = 0;
    appState = STATE_DELETE_CONFIRM;
  } else {
    // Natural EOF — return to video list
    appState = STATE_GALLERY_VIDEOS;
  }
  galleryNeedsRedraw = true;
  if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
}
