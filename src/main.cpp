#include "shared.h"

// ================= NVS NAMESPACES =================
// "cnt" — file counters (pictureNumber, videoNumber)
// "cam" — camera sensor settings (CameraSettings struct)

// ================= ENCODER STATE TABLE =================
// Quadrature state table — DRAM_ATTR ensures ISR-safe access during flash operations.
// Index = (prevState << 2) | currState, where state = (CLK << 1) | DT.
// +1 = CW step, -1 = CCW step, 0 = invalid/bounce (ignored).
static const DRAM_ATTR int8_t ENC_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

// ================= GLOBAL VARIABLE DEFINITIONS =================
// All globals declared as extern in shared.h are defined here (except
// AVI writer globals in avi_writer.cpp and webServer in wifi_server.cpp).

LGFX tft;
SemaphoreHandle_t spiMutex;
volatile AppState appState = STATE_IDLE;
EventGroupHandle_t appEvents;
QueueHandle_t inputEventQueue = NULL;
SDStatus globalSDState = {false, false, "NO CARD"};
CameraSettings camSettings = { 1, 0, 2, 0, 0, 0, 1, 0, 12, 10, 0 };
camera_config_t config;
int pictureNumber = 0;
int videoNumber = 0;
unsigned long recordingStartTime = 0;
File videoFile;
TaskHandle_t taskDisplayHandle = NULL;

// Display double buffer
uint8_t *dispBuf[2] = {nullptr, nullptr};
volatile size_t dispLen[2] = {0, 0};
std::atomic<int> dispWriteSlot{0};
std::atomic<int> dispReadSlot{-1};
std::atomic<int> dispRendering{-1};

// Recorder pool
uint8_t *recPool[REC_POOL_SIZE] = {nullptr, nullptr, nullptr};
QueueHandle_t recFrameQueue = NULL;
SemaphoreHandle_t recPoolFree = NULL;
int recPoolWriteIdx = 0;

// Audio
int16_t *audioRecBuf = nullptr;
i2s_chan_handle_t i2s_rx_handle = NULL;

// Gallery state
char **galleryFiles = nullptr;
int galleryFileCount = 0;
int galleryIndex = 0;
int galleryTypeSelection = 0;
int deleteSelection = 0;
volatile bool galleryNeedsRedraw = false;

// Menu state
int menuMainSelection = 0;
int settingsIndex = 0;
bool settingsEditing = false;
int resetCounterConfirm = 0;

// Encoder ISR state — DRAM_ATTR keeps these in internal SRAM so ISRs
// can access them safely even during flash cache misses (e.g. SD writes).
DRAM_ATTR volatile uint8_t encPrevState = 3;   // both HIGH at boot (INPUT_PULLUP default)
DRAM_ATTR volatile int8_t  encAccum     = 0;   // direction accumulator between detents
DRAM_ATTR volatile uint32_t encSwPressMs = 0;  // timestamp of SW button press edge

// Debug counters: ISR increments, taskInput reads and prints the delta.
// Safe because uint32 reads/writes are atomic on Xtensa; no mutex needed.
#if DEBUG_INPUT
volatile uint32_t dbgEncCw    = 0;
volatile uint32_t dbgEncCcw   = 0;
volatile uint32_t dbgEncClick = 0;
volatile uint32_t dbgEncLong  = 0;
#endif

// ================= TASK PROTOTYPES (file-local) =================
static void taskSDMonitor(void *pvParameters);
static void taskCapture(void *pvParameters);
static void taskDisplay(void *pvParameters);
static void taskRecorder(void *pvParameters);
static void taskInput(void *pvParameters);
static void savePhotoHighRes();
static void drawStatusBar(float fps);
static void encoderISR();
static void encSwISR();
static void runTimelapse();

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
#if DEBUG_BOOT_DELAY
  delay(8000);  // Long delay for Serial Monitor attachment during debugging
#else
  delay(1000);
#endif

  WiFi.mode(WIFI_OFF);
  esp_bt_controller_disable();

  // Boot button (GPIO 0) no longer used — mirror and WiFi are in encoder menu.
  pinMode(SHUTTER_BTN_PIN_1, OUTPUT);
  pinMode(SHUTTER_BTN_PIN_2, INPUT_PULLUP);
  digitalWrite(SHUTTER_BTN_PIN_1, LOW);

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);

  spiMutex = xSemaphoreCreateMutex();
  appEvents = xEventGroupCreate();
  inputEventQueue = xQueueCreate(16, sizeof(InputEvent));
  recFrameQueue = xQueueCreate(REC_POOL_SIZE, sizeof(RecFrame));
  recPoolFree = xSemaphoreCreateCounting(REC_POOL_SIZE, REC_POOL_SIZE);

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, SD_CS_PIN);
  delay(100);

  // TFT
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(middle_center);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(3);
  tft.drawString("GETTING READY", tft.width() / 2, 75);
  tft.setTextSize(3);
  tft.drawString("PLEASE WAIT..", tft.width() / 2, 115);
  tft.setTextSize(2);
  tft.drawString("github@barkinsarikartal", tft.width() / 2, 200); // mini ad here :)
  tft.setTextDatum(top_left);

  // Load file counters from NVS
  {
    Preferences p;
    p.begin("cnt", true);  // read-only
    pictureNumber = p.getInt("pic", 0);
    videoNumber   = p.getInt("vid", 0);
    p.end();
  }

  // Load camera settings from NVS
  loadCameraSettings();

  Serial.printf("Initial image no: %d, video no: %d\n", pictureNumber, videoNumber);

  // CAMERA CONFIG
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;

  initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);

  allocateBuffers();

  delay(2000);

  // Task: SD Card Monitor (Core 0, Priority 1)
  xTaskCreatePinnedToCore(taskSDMonitor, "SD_Mon", 4096, NULL, 1, NULL, 0);

  // Task: Button/Input Handler (Core 0, Priority 2)
  xTaskCreatePinnedToCore(taskInput, "Input", 4096, NULL, 2, NULL, 0);

  // Task: AVI Recorder / SD Writer (Core 0, Priority 4)
  xTaskCreatePinnedToCore(taskRecorder, "Recorder", 8192, NULL, 4, NULL, 0);

  // Task: TFT Display (Core 1, Priority 3)
  xTaskCreatePinnedToCore(taskDisplay, "Display", 16384, NULL, 3, &taskDisplayHandle, 1);

  // Task: Camera Capture (Core 1, Priority 6 - highest)
  xTaskCreatePinnedToCore(taskCapture, "Capture", 8192, NULL, 6, NULL, 1);

  // Attach encoder interrupts AFTER tasks are created so the queue exists.
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT),  encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_SW),  encSwISR,  CHANGE);
  Serial.println("Encoder ISRs attached (CLK+DT+SW).");

  Serial.println("Cam is ready. Tasks launched.");
}

// ================= LOOP =================
void loop() {
  vTaskDelete(NULL);
}

// ================= BUFFER ALLOCATION =================
void allocateBuffers() {
  for (int i = 0; i < 2; i++) {
    dispBuf[i] = (uint8_t*)ps_malloc(DISP_BUF_SIZE);
    if (!dispBuf[i]) Serial.printf("[ERR] dispBuf[%d] alloc failed!\n", i);
  }
  for (int i = 0; i < REC_POOL_SIZE; i++) {
    recPool[i] = (uint8_t*)ps_malloc(REC_BUF_SIZE);
    if (!recPool[i]) Serial.printf("[ERR] recPool[%d] alloc failed!\n", i);
  }
  audioRecBuf = (int16_t*)ps_malloc(AUDIO_REC_BUF_SIZE);
  if (!audioRecBuf) Serial.println("[ERR] audioRecBuf alloc failed!");

  // AVI metadata arrays: allocated ONCE at boot to prevent PSRAM heap fragmentation.
  // startAVI() only clears them with memset; endAVI() never frees them.
  avi_frame_sizes = (uint32_t*)ps_malloc(MAX_AVI_FRAMES * sizeof(uint32_t));
  if (!avi_frame_sizes) Serial.println("[ERR] avi_frame_sizes alloc failed!");
  avi_audio_sizes = (uint32_t*)ps_malloc(MAX_AVI_FRAMES * sizeof(uint32_t));
  if (!avi_audio_sizes) Serial.println("[ERR] avi_audio_sizes alloc failed!");
  avi_chunk_order = (uint8_t*)ps_malloc(MAX_AVI_CHUNKS);
  if (!avi_chunk_order) Serial.println("[ERR] avi_chunk_order alloc failed!");

  Serial.printf("[MEM] Free PSRAM after boot alloc: %u bytes\n",
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// ================= HELPER: TFT MESSAGE =================
// Shows a centred message on the TFT (acquires spiMutex internally).
// If durationMs > 0, blocks the calling task for that duration.
void showTFTMessage(const char *msg, uint16_t color, int durationMs) {
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillRect(0, 80, 320, 60, TFT_BLACK);
    tft.setTextDatum(middle_center);
    tft.setTextSize(2);
    tft.setTextColor(color);
    tft.drawString(msg, tft.width() / 2, 110);
    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }
  if (durationMs > 0) vTaskDelay(pdMS_TO_TICKS(durationMs));
}

// ================= HELPER: EMERGENCY EXIT =================
// Exits any menu/gallery state back to IDLE. Reverts uncommitted settings edits,
// frees gallery memory, reinits camera and clears the screen.
void emergencyExitToIdle() {
  Serial.println("[MENU] Emergency exit -> IDLE");
  if (settingsEditing) {
    loadCameraSettings();
    settingsEditing = false;
  }
  freeGalleryFiles();
  initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    xSemaphoreGive(spiMutex);
  }
  appState = STATE_IDLE;
}

// ================= TASK: SD MONITOR (Core 0, Priority 1) =================
static void taskSDMonitor(void *pvParameters) {
  while (true) {
    // WiFi mode: lightweight SD presence check only.
    // Skip remount attempts (SD.end/begin) to avoid colliding with active downloads.
    // Update globalSDState so web routes return correct errors if SD removed.
    // Update TFT status so user can see SD state without browser.
    if (appState == STATE_WIFI_MODE) {
      if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        bool wasMounted = globalSDState.isMounted;
        if (SD.cardType() == CARD_NONE || SD.totalBytes() == 0) {
          globalSDState.isMounted = false;
          globalSDState.isFull = false;
          globalSDState.errorMsg = "SD REMOVED";
        } else {
          globalSDState.isMounted = true;
          uint64_t freeBytes = SD.totalBytes() - SD.usedBytes();
          globalSDState.isFull = (freeBytes < 10 * 1024 * 1024);
          globalSDState.errorMsg = globalSDState.isFull ? "SD FULL" : "READY";
        }
        xSemaphoreGive(spiMutex);

        // SD state changed — update TFT bottom bar
        if (wasMounted != globalSDState.isMounted) {
          if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(200))) {
            tft.fillRect(0, 210, 320, 30, TFT_BLACK);
            tft.setTextDatum(middle_center);
            tft.setTextSize(1);
            if (!globalSDState.isMounted) {
              tft.setTextColor(TFT_RED);
              tft.drawString("! SD CARD REMOVED !", tft.width() / 2, 220);
              Serial.println("[WIFI] SD card removed during WiFi mode.");
            } else {
              tft.setTextColor(0x7BEF);
              tft.drawString("Press to exit", tft.width() / 2, 220);
              Serial.println("[WIFI] SD card re-inserted during WiFi mode.");
            }
            tft.setTextDatum(top_left);
            xSemaphoreGive(spiMutex);
          }
        }
      }
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    // During recording: skip SD polling entirely. SD removal and space
    // exhaustion are caught by taskRecorder's write error path, which sends
    // EVT_SD_STOP. Polling here would compete for spiMutex with the recorder
    // on every cycle, reducing effective write throughput.
    if (appState == STATE_RECORDING || appState == STATE_STOPPING) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {

      bool currentMount = false;
      bool currentFull = false;
      String msg = "";

      if (SD.cardType() == CARD_NONE || SD.totalBytes() == 0) {
        SD.end();
        if (!SD.begin(SD_CS_PIN, SPI, 20000000)) {
          currentMount = false;
          msg = "NO SD CARD";
        }
        else {
          currentMount = true;
        }
      }
      else {
        currentMount = true;
      }

      if (currentMount) {
        uint64_t total = SD.totalBytes();
        uint64_t used = SD.usedBytes();
        uint64_t freeSpace = total - used;

        if (freeSpace < 10 * 1024 * 1024) {
          currentFull = true;
          msg = "SD FULL";
        }
        else {
          currentFull = false;
          msg = "READY";
        }
      }

      globalSDState.isMounted = currentMount;
      globalSDState.isFull = currentFull;
      globalSDState.errorMsg = msg;

      xSemaphoreGive(spiMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ================= TASK: INPUT HANDLER (Core 0, Priority 2) =================
//
// Queue-based input pipeline.
// All physical inputs are translated into InputEvent and pushed to inputEventQueue.
// Encoder ISRs push CW/CCW/CLICK/LONG directly; shutter button is polled here.
//
// State machine:
//   IDLE -> ENC_CLICK -> MENU_MAIN -> (Gallery | WiFi | Settings | Exit)
//   ENC_LONG = universal "go back one level"
//   SHUTTER short = photo (IDLE), stop recording, or emergency exit from any menu
//   SHUTTER long  = start recording (from IDLE or any menu — auto-exits first)
//
// Boot button disabled in Phase 10 — mirror toggle and WiFi entry handled via encoder menu.
static void taskInput(void *pvParameters) {
  bool lastShutter = HIGH;
  unsigned long shutterPressTime = 0;

  auto sendEvent = [](InputEventType t) {
    InputEvent e = { t };
    xQueueSend(inputEventQueue, &e, 0);
#if DEBUG_INPUT
    static const char* const names[] = {
      "BTN_SHORT", "BTN_LONG",
      "ENC_CW",   "ENC_CCW",  "ENC_CLICK",  "ENC_LONG"
    };
    Serial.printf("[INPUT] %s\n", (t < 6) ? names[t] : "?");
#endif
  };

  while (true) {
    bool shutterNow = digitalRead(SHUTTER_BTN_PIN_2);

    // ── SHUTTER press edge ──
    if (lastShutter == HIGH && shutterNow == LOW) {
      shutterPressTime = millis();
    }

    // ── SHUTTER release edge ──
    if (lastShutter == LOW && shutterNow == HIGH) {
      unsigned long dur = millis() - shutterPressTime;

      if (dur < 1000) {
        // Short press
        if (appState == STATE_IDLE) {
          // Take photo
          bool canSave = false;
          String errMsg = "";
          if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100))) {
            if (!globalSDState.isMounted) errMsg = "NO SD CARD!";
            else if (globalSDState.isFull)  errMsg = "SD FULL!";
            else canSave = true;
            xSemaphoreGive(spiMutex);
          }
          if (canSave) {
            xEventGroupSetBits(appEvents, EVT_TAKE_PHOTO);
            sendEvent(INPUT_BTN_SHORT);
          } else {
            showTFTMessage(errMsg.c_str(), TFT_RED, 1000);
          }
        } else if (appState == STATE_RECORDING && millis() - recordingStartTime > 2000) {
          // Stop recording
          xEventGroupSetBits(appEvents, EVT_STOP_RECORDING);
          sendEvent(INPUT_BTN_SHORT);
        } else if (appState == STATE_TIMELAPSE) {
          // Safe exit: use intermediate state so runTimelapse() handles camera cleanup.
          // Don't call emergencyExitToIdle() — it would race with camera ops in runTimelapse.
          appState = STATE_MENU_MAIN;
        } else if (appState == STATE_MENU_MAIN || appState == STATE_SETTINGS ||
                   appState == STATE_GALLERY_TYPE || appState == STATE_GALLERY_PHOTOS ||
                   appState == STATE_GALLERY_VIDEOS || appState == STATE_DELETE_CONFIRM ||
                   appState == STATE_VIDEO_PLAYING) {
          // Emergency exit from any gallery/menu state -> IDLE
          emergencyExitToIdle();
        }
      }
      // Long press released: timelapse needs explicit handling (not in held isMenu list)
      else if (appState == STATE_TIMELAPSE) {
        appState = STATE_MENU_MAIN;
      }
      // Other long presses handled by held detection below
    }

    // ── SHUTTER held long (>1s) -> start recording ──
    // Works from IDLE or any gallery state (exits gallery first, reinits camera)
    {
      AppState cs = appState;
      bool isMenu = (cs == STATE_MENU_MAIN || cs == STATE_SETTINGS ||
                     cs == STATE_GALLERY_TYPE || cs == STATE_GALLERY_PHOTOS ||
                     cs == STATE_GALLERY_VIDEOS || cs == STATE_DELETE_CONFIRM ||
                     cs == STATE_VIDEO_PLAYING);
      if (shutterNow == LOW && (cs == STATE_IDLE || isMenu) &&
          (millis() - shutterPressTime > 1000) && shutterPressTime != 0) {
        // Exit menu/gallery first if needed
        if (isMenu) {
          Serial.println("[MENU] Shutter long press -> exit menu, start recording");
          emergencyExitToIdle();
        }

        bool cardReady = false;
        String errorMsg = "";
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100))) {
          if (!globalSDState.isMounted) errorMsg = "NO SD CARD!";
          else if (globalSDState.isFull)  errorMsg = "SD FULL!";
          else cardReady = true;
          xSemaphoreGive(spiMutex);
        }
        if (cardReady) {
          sendEvent(INPUT_BTN_LONG);
          xEventGroupSetBits(appEvents, EVT_START_RECORDING);
          // wait for state change to avoid re-trigger
          while (appState == STATE_IDLE && digitalRead(SHUTTER_BTN_PIN_2) == LOW) {
            vTaskDelay(pdMS_TO_TICKS(50));
          }
          shutterPressTime = 0;  // re-arm
        } else {
          showTFTMessage(errorMsg.c_str(), TFT_RED, 1000);
          shutterPressTime = 0;
        }
      }
    }

    // ── Process encoder events from ISR queue ──
    // Unified consumer: all encoder-based state transitions handled here.
    // Skipped during video playback and timelapse — those modes consume events directly.
    if (appState != STATE_VIDEO_PLAYING && appState != STATE_TIMELAPSE) {
      InputEvent ev;
      while (xQueueReceive(inputEventQueue, &ev, 0) == pdTRUE) {
        if (ev.type != INPUT_ENC_CW && ev.type != INPUT_ENC_CCW &&
            ev.type != INPUT_ENC_CLICK && ev.type != INPUT_ENC_LONG) continue;

        switch (appState) {
          case STATE_IDLE:
            if (ev.type == INPUT_ENC_CLICK) {
              // Menu is always accessible regardless of SD state.
              // SD checks happen per-item (Gallery, WiFi) inside STATE_MENU_MAIN.
              menuMainSelection = 0;
              galleryNeedsRedraw = true;
              enterMenuMain();
            }
            break;

          case STATE_MENU_MAIN:
            if (ev.type == INPUT_ENC_CW) {
              menuMainSelection = (menuMainSelection + 1) % MENU_MAIN_ITEMS;
              galleryNeedsRedraw = true;
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
            } else if (ev.type == INPUT_ENC_CCW) {
              menuMainSelection = (menuMainSelection + MENU_MAIN_ITEMS - 1) % MENU_MAIN_ITEMS;
              galleryNeedsRedraw = true;
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
            } else if (ev.type == INPUT_ENC_CLICK) {
              switch (menuMainSelection) {
                case 0: // Gallery — requires SD
                  if (!globalSDState.isMounted) {
                    showTFTMessage("SD REQUIRED!", TFT_RED, 1500);
                    galleryNeedsRedraw = true;
                    if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                    break;
                  }
                  galleryTypeSelection = 0;
                  galleryNeedsRedraw = true;
                  appState = STATE_GALLERY_TYPE;
                  if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                  break;
                case 1: // WiFi — requires SD
                  if (!globalSDState.isMounted) {
                    showTFTMessage("SD REQUIRED!", TFT_RED, 1500);
                    galleryNeedsRedraw = true;
                    if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                    break;
                  }
                  startWiFiMode();
                  break;
                case 2: // Settings — no SD needed
                  settingsIndex = 0;
                  settingsEditing = false;
                  galleryNeedsRedraw = true;
                  appState = STATE_SETTINGS;
                  if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                  break;
                case 3: // Timelapse — requires SD
                  if (!globalSDState.isMounted) {
                    showTFTMessage("SD REQUIRED!", TFT_RED, 1500);
                    galleryNeedsRedraw = true;
                    if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                    break;
                  }
                  appState = STATE_TIMELAPSE;
                  galleryNeedsRedraw = true;
                  if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                  break;
                case 4: // Exit
                  exitMenuToIdle();
                  break;
              }
            } else if (ev.type == INPUT_ENC_LONG) {
              exitMenuToIdle();
            }
            break;

          case STATE_WIFI_MODE:
            if (ev.type == INPUT_ENC_CLICK || ev.type == INPUT_ENC_LONG) {
              stopWiFiMode();
            }
            break;

          case STATE_GALLERY_TYPE:
            if (ev.type == INPUT_ENC_CW || ev.type == INPUT_ENC_CCW) {
              galleryTypeSelection = 1 - galleryTypeSelection;
              galleryNeedsRedraw = true;
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
            } else if (ev.type == INPUT_ENC_CLICK) {
              if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
                tft.fillRect(0, 200, 320, 20, TFT_BLACK);
                tft.setTextDatum(middle_center);
                tft.setTextSize(1);
                tft.setTextColor(TFT_YELLOW);
                tft.drawString("Scanning SD card...", tft.width() / 2, 210);
                tft.setTextDatum(top_left);
                xSemaphoreGive(spiMutex);
              }
              scanGalleryFiles(galleryTypeSelection == 1);
              if (galleryFileCount > 0) {
                galleryIndex = 0;
                galleryNeedsRedraw = true;
                appState = (galleryTypeSelection == 0) ? STATE_GALLERY_PHOTOS : STATE_GALLERY_VIDEOS;
              } else {
                if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
                  tft.fillRect(0, 200, 320, 20, TFT_BLACK);
                  tft.setTextDatum(middle_center);
                  tft.setTextSize(1);
                  tft.setTextColor(TFT_RED);
                  tft.drawString("No files found", tft.width() / 2, 210);
                  tft.setTextDatum(top_left);
                  xSemaphoreGive(spiMutex);
                }
              }
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
            } else if (ev.type == INPUT_ENC_LONG) {
              freeGalleryFiles();
              galleryNeedsRedraw = true;
              menuMainSelection = 0;
              appState = STATE_MENU_MAIN;
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
            }
            break;

          case STATE_GALLERY_PHOTOS:
            if (ev.type == INPUT_ENC_CW) {
              if (galleryIndex < galleryFileCount - 1) {
                galleryIndex++;
                galleryNeedsRedraw = true;
                if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
              }
            } else if (ev.type == INPUT_ENC_CCW) {
              if (galleryIndex > 0) {
                galleryIndex--;
                galleryNeedsRedraw = true;
                if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
              }
            } else if (ev.type == INPUT_ENC_CLICK) {
              deleteSelection = 0;
              galleryNeedsRedraw = true;
              appState = STATE_DELETE_CONFIRM;
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
            } else if (ev.type == INPUT_ENC_LONG) {
              freeGalleryFiles();
              galleryNeedsRedraw = true;
              appState = STATE_GALLERY_TYPE;
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
            }
            break;

          case STATE_GALLERY_VIDEOS:
            if (ev.type == INPUT_ENC_CW) {
              if (galleryIndex < galleryFileCount - 1) {
                galleryIndex++;
                galleryNeedsRedraw = true;
                if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
              }
            } else if (ev.type == INPUT_ENC_CCW) {
              if (galleryIndex > 0) {
                galleryIndex--;
                galleryNeedsRedraw = true;
                if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
              }
            } else if (ev.type == INPUT_ENC_CLICK) {
              // Play video directly
              appState = STATE_VIDEO_PLAYING;
              galleryNeedsRedraw = true;
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
            } else if (ev.type == INPUT_ENC_LONG) {
              freeGalleryFiles();
              galleryNeedsRedraw = true;
              appState = STATE_GALLERY_TYPE;
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
            }
            break;

          case STATE_DELETE_CONFIRM:
            if (ev.type == INPUT_ENC_CW || ev.type == INPUT_ENC_CCW) {
              deleteSelection = 1 - deleteSelection;
              galleryNeedsRedraw = true;
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
            } else if (ev.type == INPUT_ENC_CLICK) {
              if (galleryTypeSelection == 1 && deleteSelection == 0) {
                // Videos: "Play" selected
                appState = STATE_VIDEO_PLAYING;
                galleryNeedsRedraw = true;
                if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
              } else if (deleteSelection == 1 && galleryFileCount > 0) {
                // Delete the file (photos or videos)
                bool ok = false;
                if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
                  ok = SD.remove(galleryFiles[galleryIndex]);
                  xSemaphoreGive(spiMutex);
                }
                Serial.printf("[GAL] Delete %s: %s\n", galleryFiles[galleryIndex], ok ? "OK" : "FAIL");
                if (ok) {
                  free(galleryFiles[galleryIndex]);
                  for (int i = galleryIndex; i < galleryFileCount - 1; i++) {
                    galleryFiles[i] = galleryFiles[i + 1];
                  }
                  galleryFileCount--;
                  if (galleryIndex >= galleryFileCount && galleryIndex > 0) galleryIndex--;
                }
                // Return to gallery or type menu if empty
                if (galleryFileCount > 0) {
                  appState = (galleryTypeSelection == 0) ? STATE_GALLERY_PHOTOS : STATE_GALLERY_VIDEOS;
                } else {
                  freeGalleryFiles();
                  appState = STATE_GALLERY_TYPE;
                }
                galleryNeedsRedraw = true;
                if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
              } else {
                // Photos: "Cancel" selected (deleteSelection == 0)
                appState = STATE_GALLERY_PHOTOS;
                galleryNeedsRedraw = true;
                if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
              }
            } else if (ev.type == INPUT_ENC_LONG) {
              // Cancel — go back
              appState = (galleryTypeSelection == 0) ? STATE_GALLERY_PHOTOS : STATE_GALLERY_VIDEOS;
              galleryNeedsRedraw = true;
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
            }
            break;

          case STATE_SETTINGS: {
            // Settings menu: browse or edit camera parameters
            if (!settingsEditing) {
              // Browsing mode: CW/CCW move highlight, CLICK enters edit
              if (ev.type == INPUT_ENC_CW) {
                settingsIndex = (settingsIndex + 1) % SETTINGS_COUNT;
                galleryNeedsRedraw = true;
                if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
              } else if (ev.type == INPUT_ENC_CCW) {
                settingsIndex = (settingsIndex + SETTINGS_COUNT - 1) % SETTINGS_COUNT;
                galleryNeedsRedraw = true;
                if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
              } else if (ev.type == INPUT_ENC_CLICK) {
                resetCounterConfirm = 0;  // safety: always start at "No"
                settingsEditing = true;
                galleryNeedsRedraw = true;
                if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
              } else if (ev.type == INPUT_ENC_LONG) {
                // Save and return to main menu
                saveCameraSettings();
                galleryNeedsRedraw = true;
                menuMainSelection = 0;
                appState = STATE_MENU_MAIN;
                if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
              }
            } else {
              // Editing mode: CW/CCW adjust value, CLICK confirms

              // Special case: counter reset (index 11) — action-type setting
              if (settingsIndex == 11) {
                if (ev.type == INPUT_ENC_CW || ev.type == INPUT_ENC_CCW) {
                  resetCounterConfirm = 1 - resetCounterConfirm;
                  galleryNeedsRedraw = true;
                  if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                } else if (ev.type == INPUT_ENC_CLICK) {
                  if (resetCounterConfirm == 1) {
                    // Execute counter reset
                    pictureNumber = 0;
                    videoNumber = 0;
                    { Preferences p; p.begin("cnt", false); p.putInt("pic", 0); p.putInt("vid", 0); p.end(); }
                    Serial.println("[NVS] File counters reset to 0.");

                    // Show warning on TFT
                    settingsEditing = false;
                    resetCounterConfirm = 0;
                    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
                      tft.fillScreen(TFT_BLACK);
                      tft.setTextDatum(middle_center);
                      tft.setTextSize(2);
                      tft.setTextColor(TFT_GREEN);
                      tft.drawString("Counters Reset!", tft.width() / 2, 40);
                      tft.setTextSize(1);
                      tft.setTextColor(TFT_YELLOW);
                      tft.drawString("P:0  V:0", tft.width() / 2, 70);
                      tft.setTextSize(2);
                      tft.setTextColor(TFT_RED);
                      tft.drawString("!! WARNING !!", tft.width() / 2, 110);
                      tft.setTextSize(1);
                      tft.setTextColor(TFT_WHITE);
                      tft.drawString("Gallery will show wrong", tft.width() / 2, 140);
                      tft.drawString("files after counter reset.", tft.width() / 2, 155);
                      tft.setTextColor(TFT_CYAN);
                      tft.drawString("Backup your photos/videos", tft.width() / 2, 180);
                      tft.drawString("via WiFi, then delete them", tft.width() / 2, 195);
                      tft.drawString("from SD card.", tft.width() / 2, 210);
                      tft.setTextDatum(top_left);
                      xSemaphoreGive(spiMutex);
                    }
                    vTaskDelay(pdMS_TO_TICKS(4000));
                    galleryNeedsRedraw = true;
                    if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                  } else {
                    // "No" selected — cancel
                    settingsEditing = false;
                    resetCounterConfirm = 0;
                    galleryNeedsRedraw = true;
                    if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                  }
                } else if (ev.type == INPUT_ENC_LONG) {
                  // Cancel
                  settingsEditing = false;
                  resetCounterConfirm = 0;
                  galleryNeedsRedraw = true;
                  if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                }
                break;  // handled — skip normal val logic
              }

              int *val = nullptr;
              int lo = 0, hi = 0;
              switch (settingsIndex) {
                case 0: val = &camSettings.brightness;    lo = -2; hi = 2; break;
                case 1: val = &camSettings.contrast;      lo = -2; hi = 2; break;
                case 2: val = &camSettings.saturation;    lo = -2; hi = 2; break;
                case 3: val = &camSettings.ae_level;      lo = -2; hi = 2; break;
                case 4: val = &camSettings.wb_mode;       lo = 0;  hi = 4; break;
                case 5: val = &camSettings.special_effect; lo = 0; hi = 6; break;
                case 6: val = &camSettings.hmirror;       lo = 0;  hi = 1; break;
                case 7: val = &camSettings.vflip;         lo = 0;  hi = 1; break;
                case 8: val = &camSettings.jpeg_quality;  lo = 4;  hi = 63; break;
                case 9: val = &camSettings.timelapse_interval; break;
                case 10: val = &camSettings.rec_max_seconds;  break;
              }
              if (val) {
                if (ev.type == INPUT_ENC_CW || ev.type == INPUT_ENC_CCW) {
                  bool cw = (ev.type == INPUT_ENC_CW);
                  if (settingsIndex == 9) {
                    // Timelapse interval: step through presets
                    static const int p[] = {5,10,15,30,60,120,300,600};
                    int n = 8, ci = 0;
                    for (int i = 0; i < n; i++) { if (p[i] >= *val) { ci = i; break; } if (i == n-1) ci = n-1; }
                    if (cw && ci < n-1) *val = p[ci+1];
                    else if (!cw && ci > 0) *val = p[ci-1];
                  } else if (settingsIndex == 10) {
                    // Rec duration limit: step through presets
                    static const int p[] = {0,30,60,120,300,600,900};
                    int n = 7, ci = 0;
                    for (int i = 0; i < n; i++) { if (p[i] >= *val) { ci = i; break; } if (i == n-1) ci = n-1; }
                    if (cw && ci < n-1) *val = p[ci+1];
                    else if (!cw && ci > 0) *val = p[ci-1];
                  } else {
                    // Linear range settings
                    if (cw) { if (*val < hi) (*val)++; }
                    else { if (*val > lo) (*val)--; }
                  }
                  galleryNeedsRedraw = true;
                  if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                } else if (ev.type == INPUT_ENC_CLICK) {
                  // Confirm edit — save to NVS immediately
                  settingsEditing = false;
                  saveCameraSettings();
                  galleryNeedsRedraw = true;
                  if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                } else if (ev.type == INPUT_ENC_LONG) {
                  // Cancel edit — revert to saved
                  loadCameraSettings();
                  settingsEditing = false;
                  galleryNeedsRedraw = true;
                  if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
                }
              }
            }
            break;
          }

          default:
            break;
        }
      }
    }

    lastShutter = shutterNow;

#if DEBUG_INPUT
    // Print ISR-generated encoder events (not routed through sendEvent).
    // Uses snapshot deltas so we don't miss rapid pulses.
    static uint32_t lastCw = 0, lastCcw = 0, lastClick = 0, lastLong = 0;
    uint32_t cw = dbgEncCw, ccw = dbgEncCcw, clk = dbgEncClick, lng = dbgEncLong;
    if (cw  != lastCw)  { Serial.printf("[ENC] CW  x%lu\n",  cw  - lastCw);  lastCw  = cw;  }
    if (ccw != lastCcw) { Serial.printf("[ENC] CCW x%lu\n",  ccw - lastCcw); lastCcw = ccw; }
    if (clk != lastClick) { Serial.printf("[ENC] CLICK\n");                  lastClick = clk; }
    if (lng != lastLong)  { Serial.printf("[ENC] LONG PRESS\n");              lastLong  = lng; }
#endif

    vTaskDelay(pdMS_TO_TICKS(20));  // 50 Hz polling
  }
}

// ================= ENCODER ISRs =================
// Placed in IRAM so they execute even during Flash cache misses (e.g. SD writes).
// Both ISRs push directly to inputEventQueue — the same queue taskInput uses —
// so all menu logic consumes from a single source.
static void IRAM_ATTR encoderISR() {
  // Read current 2-bit state: (CLK << 1) | DT
  uint8_t s = ((uint8_t)digitalRead(ENC_CLK) << 1) | (uint8_t)digitalRead(ENC_DT);

  // Lookup direction from state transition table
  int8_t dir = ENC_TABLE[(encPrevState << 2) | s];
  encPrevState = s;
  if (dir == 0) return;  // invalid transition (bounce) — discard silently

  encAccum += dir;

  // Emit event only at detent rest position (both pins HIGH = 0b11).
  // This absorbs all intermediate bounce and partial-rotation noise;
  // the accumulated direction tells us which way the knob actually moved.
  if (s == 0b11) {
    if (encAccum > 0) {
      InputEvent e = { INPUT_ENC_CW };
#if DEBUG_INPUT
      dbgEncCw = dbgEncCw + 1;
#endif
      BaseType_t woken = pdFALSE;
      xQueueSendFromISR(inputEventQueue, &e, &woken);
      if (woken) portYIELD_FROM_ISR();
    } else if (encAccum < 0) {
      InputEvent e = { INPUT_ENC_CCW };
#if DEBUG_INPUT
      dbgEncCcw = dbgEncCcw + 1;
#endif
      BaseType_t woken = pdFALSE;
      xQueueSendFromISR(inputEventQueue, &e, &woken);
      if (woken) portYIELD_FROM_ISR();
    }
    encAccum = 0;
  }
}

static void IRAM_ATTR encSwISR() {
  uint32_t now = millis();
  if (digitalRead(ENC_SW) == LOW) {
    // Falling edge — record press start time.
    encSwPressMs = now;
  } else {
    // Rising edge — reject contact bounce (< 30 ms press is physically impossible).
    uint32_t dur = now - encSwPressMs;
    if (dur < 30) return;
    InputEvent e;
    e.type = (dur > 500) ? INPUT_ENC_LONG : INPUT_ENC_CLICK;
#if DEBUG_INPUT
    if (e.type == INPUT_ENC_LONG) dbgEncLong  = dbgEncLong  + 1;
    else                          dbgEncClick = dbgEncClick + 1;
#endif
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(inputEventQueue, &e, &woken);
    if (woken) portYIELD_FROM_ISR();
  }
}

// ================= TASK: CAMERA CAPTURE (Core 1, Priority 6) =================
static void taskCapture(void *pvParameters) {
  int fail_count = 0;

  while (true) {
    // Skip capture when camera is deinited (WiFi mode, gallery states)
    {
      AppState cs = appState;
      if (cs == STATE_WIFI_MODE || cs == STATE_MENU_MAIN || cs == STATE_SETTINGS ||
          cs == STATE_GALLERY_TYPE || cs == STATE_GALLERY_PHOTOS ||
          cs == STATE_GALLERY_VIDEOS || cs == STATE_DELETE_CONFIRM ||
          cs == STATE_VIDEO_PLAYING || cs == STATE_TIMELAPSE) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
    }

    // ---- Handle state transition events ----
    EventBits_t bits = xEventGroupGetBits(appEvents);

    // TAKE PHOTO
    if (bits & EVT_TAKE_PHOTO) {
      xEventGroupClearBits(appEvents, EVT_TAKE_PHOTO);
      savePhotoHighRes();
      continue;
    }

    // START RECORDING
    if ((bits & EVT_START_RECORDING) && appState == STATE_IDLE) {
      xEventGroupClearBits(appEvents, EVT_START_RECORDING | EVT_STOP_RECORDING | EVT_SD_STOP);

      initCamera(VIDEO_MODE, VIDEO_RESOLUTION, REC_JPEG_QUALITY);

      bool started = false;
      if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
        String path = "/vid_" + String(videoNumber) + ".avi";
        // Skip past any existing files (prevents overwrite on NVS counter reset)
        while (SD.exists(path.c_str())) {
          videoNumber++;
          path = "/vid_" + String(videoNumber) + ".avi";
        }
        Serial.printf("Video Start: %s\n", path.c_str());
        videoFile = SD.open(path.c_str(), FILE_WRITE);
        if (videoFile) {
          videoNumber++;
          { Preferences p; p.begin("cnt", false); p.putInt("vid", videoNumber); p.end(); }
          startAVI(videoFile, 10, 480, 320);
          started = true;
        } else {
          Serial.println("Video file create failed");
        }
        xSemaphoreGive(spiMutex);
      }

      if (started) {
        initMicrophone();
        recPoolWriteIdx = 0;
        recordingStartTime = millis();
        appState = STATE_RECORDING;
        Serial.println("Recording started.");
      }

      if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
        tft.fillScreen(TFT_BLACK);
        xSemaphoreGive(spiMutex);
      }
      continue;
    }

    // STOP RECORDING
    if ((bits & (EVT_STOP_RECORDING | EVT_SD_STOP)) && appState == STATE_RECORDING) {
      bool wasSdStop = (bits & EVT_SD_STOP) != 0;
      xEventGroupClearBits(appEvents, EVT_STOP_RECORDING | EVT_SD_STOP);

      appState = STATE_STOPPING; // signals audio/recorder to stop, blocks taskDisplay

      // wait for recorder queue to drain (max 500ms)
      int drainTimeout = 50;
      while (uxQueueMessagesWaiting(recFrameQueue) > 0 && --drainTimeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      vTaskDelay(pdMS_TO_TICKS(50)); // let recorder finish last write

      deinitMicrophone();

      // close AVI file
      if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
        if (videoFile) endAVI(videoFile, 10, 480, 320);
        xSemaphoreGive(spiMutex);
      }

      // show saved/error message
      if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
        if (wasSdStop) {
          tft.fillScreen(TFT_BLACK);
          tft.setTextSize(2);
          tft.setTextColor(TFT_RED);
          tft.setCursor(20, 90);
          tft.print("REC STOPPED");
          tft.setCursor(20, 120);
          tft.print(globalSDState.isMounted ? "SD FULL!" : "SD REMOVED!");
        } else {
          tft.fillRoundRect(60, 85, 200, 50, 8, TFT_WHITE);
          tft.drawRoundRect(60, 85, 200, 50, 8, TFT_BLACK);
          tft.setTextSize(2);
          tft.setTextColor(TFT_BLACK);
          tft.setCursor(68, 102);
          tft.print("VIDEO SAVED #" + String(videoNumber - 1));
        }
        xSemaphoreGive(spiMutex);
      }
      Serial.println("Recording stopped.");
      vTaskDelay(pdMS_TO_TICKS(1000));  // keep notification visible

      initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);
      appState = STATE_IDLE;  // resume display after notification shown
      continue;
    }

    // ---- Normal frame capture ----
    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) {
      fail_count++;
      Serial.printf("Cam Fail: %d\n", fail_count);
      if (fail_count > 10) {
        if (appState == STATE_RECORDING) {
          xEventGroupSetBits(appEvents, EVT_STOP_RECORDING);
          Serial.println("Cam error! Requesting recording stop.");
        } else {
          if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
            tft.fillScreen(TFT_RED);
            tft.setTextSize(2);
            tft.drawString("CAM RESET...", 20, 110);
            xSemaphoreGive(spiMutex);
          }
          delay(100);
          initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);
        }
        fail_count = 0;
      }
      vTaskDelay(10);
      continue;
    }
    fail_count = 0;

    // Copy frame for display (double buffer ping-pong)
    // Skip during gallery states — dispBuf is reused for JPEG file decode.
    // Skip if taskDisplay is currently rendering this slot.
    {
      AppState cs = appState;
      if (cs == STATE_IDLE || cs == STATE_RECORDING) {
        int ws = dispWriteSlot.load();
        if (ws != dispRendering.load() && fb->len <= DISP_BUF_SIZE && dispBuf[ws]) {
          memcpy(dispBuf[ws], fb->buf, fb->len);
          dispLen[ws] = fb->len;
          dispReadSlot.store(ws);
          dispWriteSlot.store(1 - ws);
          if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
        }
      }
    }

    // Copy frame for recorder (if recording)
    if (appState == STATE_RECORDING && fb->format == PIXFORMAT_JPEG) {
      if (xSemaphoreTake(recPoolFree, 0) == pdTRUE) {
        int idx = recPoolWriteIdx;
        recPoolWriteIdx = (recPoolWriteIdx + 1) % REC_POOL_SIZE;
        if (recPool[idx] && fb->len <= REC_BUF_SIZE) {
          memcpy(recPool[idx], fb->buf, fb->len);
          RecFrame rf = { idx, (size_t)fb->len };
          xQueueSend(recFrameQueue, &rf, 0);
        } else {
          xSemaphoreGive(recPoolFree); // return slot if copy failed
        }
      }
    }

    // Return camera buffer immediately — this is the key to preventing FB-OVF
    esp_camera_fb_return(fb);

    // Auto-stop recording if duration limit is configured and exceeded
    if (appState == STATE_RECORDING && camSettings.rec_max_seconds > 0) {
      unsigned long elapsedSec = (millis() - recordingStartTime) / 1000;
      if (elapsedSec >= (unsigned long)camSettings.rec_max_seconds) {
        Serial.printf("[REC] Duration limit reached (%ds), auto-stopping.\n", camSettings.rec_max_seconds);
        xEventGroupSetBits(appEvents, EVT_STOP_RECORDING);
      }
    }

    vTaskDelay(1);
  }
}

// ================= TASK: TFT DISPLAY (Core 1, Priority 3) =================
static void taskDisplay(void *pvParameters) {
  uint32_t last_fps_time = 0;
  uint32_t frame_count = 0;
  float fps = 0;

  while (true) {
    // wait for a new frame notification (200ms timeout for FPS update even if no frames)
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));

    // Don't render during photo/stopping/wifi — let notifications stay on screen
    if (appState == STATE_PHOTO || appState == STATE_STOPPING || appState == STATE_WIFI_MODE) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Menu and gallery states — render on demand, skip camera preview
    if (appState == STATE_MENU_MAIN || appState == STATE_SETTINGS ||
        appState == STATE_GALLERY_TYPE || appState == STATE_GALLERY_PHOTOS ||
        appState == STATE_GALLERY_VIDEOS || appState == STATE_DELETE_CONFIRM) {
      if (galleryNeedsRedraw) {
        galleryNeedsRedraw = false;
        switch (appState) {
          case STATE_MENU_MAIN:      drawMenuMain(); break;
          case STATE_SETTINGS:       drawSettingsMenu(); break;
          case STATE_GALLERY_TYPE:   drawGalleryTypeMenu(); break;
          case STATE_GALLERY_PHOTOS: drawGalleryPhoto(); break;
          case STATE_GALLERY_VIDEOS: drawGalleryVideoItem(); break;
          case STATE_DELETE_CONFIRM: drawDeleteConfirm(); break;
          default: break;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Video playback — blocking render loop until stopped
    if (appState == STATE_VIDEO_PLAYING) {
      playVideoOnTFT();
      continue;
    }

    // Timelapse — blocking capture loop until stopped
    if (appState == STATE_TIMELAPSE) {
      runTimelapse();
      continue;
    }

    int rs = dispReadSlot.load();
    if (rs < 0 || !dispBuf[rs]) {
      vTaskDelay(10);
      continue;
    }

    dispRendering.store(rs);  // mark slot as being rendered

    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      tft.startWrite();

      if (appState == STATE_RECORDING) {
        tft.drawJpg(dispBuf[rs], dispLen[rs], 80, 80, 0, 0, 0, 0, 0.25f);

        // blinking REC icon
        if ((millis() / 500) % 2 == 0) tft.fillCircle(16, 16, 6, TFT_RED);
        else tft.fillCircle(16, 16, 6, TFT_BLACK);

        // recording duration
        unsigned long elapsed = (millis() - recordingStartTime) / 1000;
        char timeStr[12];
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", (int)(elapsed / 3600), (int)((elapsed % 3600) / 60), (int)(elapsed % 60));
        tft.setTextSize(2);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setCursor(30, 6);
        tft.print(timeStr);
      }
      else {
        // idle: RGB565 preview
        for (int y = 0; y < 216; y++) {
          tft.pushImage(0, y, 320, 1, (uint16_t *)(dispBuf[rs] + y * 320 * 2));
        }
      }

      tft.endWrite();
      xSemaphoreGive(spiMutex);
    }

    dispRendering.store(-1);  // slot is free now

    // FPS calculation
    frame_count++;
    uint32_t now = millis();
    if (now - last_fps_time >= 1000) {
      fps = frame_count * 1000.0 / (now - last_fps_time);
      drawStatusBar(fps);
      frame_count = 0;
      last_fps_time = now;
    }
  }
}

// Draws the persistent bottom status bar (FPS + version + SD indicator).
// Called from taskDisplay whenever the 1-second FPS window elapses.
static void drawStatusBar(float fps) {
  if (!xSemaphoreTake(spiMutex, pdMS_TO_TICKS(10))) return;

  tft.fillRect(0, 216, 320, 24, TFT_BLACK);

  // FPS — left side
  tft.setTextSize(2);
  tft.setCursor(8, 220);
  tft.setTextColor(TFT_GREEN);
  tft.printf("FPS: %.1f", fps);

  // Firmware version — right-center
  tft.setTextSize(1);
  tft.setTextColor(0x4208);  // dim grey (RGB565)
  tft.setCursor(218, 224);
  tft.print(FIRMWARE_VERSION);

  // SD status dot — far right
  if (globalSDState.isMounted && !globalSDState.isFull)
    tft.fillCircle(308, 228, 5, TFT_GREEN);
  else
    tft.fillCircle(308, 228, 5, TFT_RED);

  xSemaphoreGive(spiMutex);
}

// ================= TASK: AVI RECORDER (Core 0, Priority 4) =================
static void taskRecorder(void *pvParameters) {
  while (true) {
    if (appState != STATE_RECORDING && appState != STATE_STOPPING) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    RecFrame rf;
    if (xQueueReceive(recFrameQueue, &rf, pdMS_TO_TICKS(100)) == pdTRUE) {
      // Read exactly one frame's worth of audio from the I2S DMA ring buffer.
      size_t audio_bytes_read = 0;
      if (i2s_rx_handle && audioRecBuf) {
        i2s_channel_read(i2s_rx_handle, audioRecBuf, AUDIO_BYTES_PER_FRAME,
                         &audio_bytes_read, 0);
        if (audio_bytes_read < AUDIO_BYTES_PER_FRAME) {
          memset((uint8_t*)audioRecBuf + audio_bytes_read, 0,
                 AUDIO_BYTES_PER_FRAME - audio_bytes_read);
          audio_bytes_read = AUDIO_BYTES_PER_FRAME;
        }
      }

      if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
        if (videoFile) {
          bool ok = writeAVIFrameFromBuf(videoFile, recPool[rf.idx], rf.len);
          if (ok && audio_bytes_read > 0) {
            writeAVIAudioChunk(videoFile, (uint8_t*)audioRecBuf, audio_bytes_read);
          }
          if (!ok) {
            Serial.println("Write error! Requesting recording stop.");
            xSemaphoreGive(spiMutex);
            xSemaphoreGive(recPoolFree);
            xEventGroupSetBits(appEvents, EVT_STOP_RECORDING);
            while (appState == STATE_RECORDING) {
              vTaskDelay(pdMS_TO_TICKS(10));
            }
            continue;
          }
        }
        xSemaphoreGive(spiMutex);
      }
      xSemaphoreGive(recPoolFree);
    }
  }
}

// ================= PHOTO CAPTURE =================
static void savePhotoHighRes() {
  appState = STATE_PHOTO;

  // Show "HOLD ON..." — taskDisplay skips rendering during STATE_PHOTO
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillRoundRect(60, 85, 200, 50, 8, TFT_BLUE);
    tft.drawRoundRect(60, 85, 200, 50, 8, TFT_WHITE);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(95, 102);
    tft.print("HOLD ON...");
    xSemaphoreGive(spiMutex);
  }

  Serial.println("--- HD Mode Start ---");

  initCamera(PHOTO_MODE, PHOTO_RESOLUTION, REC_JPEG_QUALITY);

  // warm up
  camera_fb_t *fb = NULL;
  for (int i = 0; i < 15; i++) {
    fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    vTaskDelay(20);
  }

  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Cam error!");
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      tft.setTextSize(2);
      tft.setCursor(95, 102);
      tft.print("CAM ERROR");
      xSemaphoreGive(spiMutex);
    }
  }
  else {
    bool saved = false;
    bool writeErr = false;
    bool sdLost = false;

    // SD write — separate mutex take to avoid nesting
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      if (SD.totalBytes() > 0) {
        String path = "/hd_pic_" + String(pictureNumber) + ".jpg";
        // Skip past any existing files (prevents overwrite on NVS counter reset)
        while (SD.exists(path.c_str())) {
          pictureNumber++;
          path = "/hd_pic_" + String(pictureNumber) + ".jpg";
        }
        Serial.printf("Saving (%u bytes): %s\n", fb->len, path.c_str());

        File file = SD.open(path.c_str(), FILE_WRITE);
        if (file) {
          size_t written = file.write(fb->buf, fb->len);
          file.close();
          if (written == fb->len) {
            Serial.println("SUCCESS!");
            pictureNumber++;
            { Preferences p; p.begin("cnt", false); p.putInt("pic", pictureNumber); p.end(); }
            saved = true;
          } else {
            Serial.printf("[ERR] Photo write incomplete: %u/%u bytes\n", written, fb->len);
            writeErr = true;
          }
        }
        else {
          Serial.println("File Open Error");
          writeErr = true;
        }
      }
      else {
        Serial.println("SD Lost during capture");
        sdLost = true;
      }
      xSemaphoreGive(spiMutex);
    }

    esp_camera_fb_return(fb);

    // Show result on TFT — separate mutex take
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      if (saved) {
        tft.fillRoundRect(60, 85, 200, 50, 8, TFT_WHITE);
        tft.drawRoundRect(60, 85, 200, 50, 8, TFT_BLACK);
        tft.setTextSize(2);
        tft.setTextColor(TFT_BLACK);
        tft.setCursor(75, 102);
        tft.print("PIC SAVED #" + String(pictureNumber - 1));
      } else if (writeErr) {
        tft.setTextSize(2);
        tft.setCursor(10, 10);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.print("WRITE ERROR");
      } else if (sdLost) {
        tft.setTextSize(2);
        tft.setCursor(10, 10);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.print("SD CONNECTION LOST");
      }
      xSemaphoreGive(spiMutex);
    }
  }

  Serial.println("--- Returning to Idle Mode ---");

  // Keep notification visible for 1 second (STATE_PHOTO blocks taskDisplay)
  vTaskDelay(pdMS_TO_TICKS(1000));

  initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);
  appState = STATE_IDLE;
}

// ================= CAMERA INIT =================
void initCamera(pixformat_t format, framesize_t size, int jpeg_quality) {
  esp_camera_deinit();

  config.pixel_format = format;
  config.frame_size = size;
  config.jpeg_quality = jpeg_quality;
  config.fb_count = (format == PIXFORMAT_RGB565) ? 3 : 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Cam init error");
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    applySettings(s);

    // These are always-on control flags, not user-tuneable.
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);

    if (format == PIXFORMAT_JPEG)
      s->set_gainceiling(s, GAINCEILING_16X);
    else
      s->set_gainceiling(s, GAINCEILING_4X);
  }
  delay(100);
}

// ================= MICROPHONE =================
void initMicrophone() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // DMA geometry tuned for exact per-frame audio drain:
  //   dma_frame_num=160 -> 160 x 2 bytes = 320 bytes per DMA slot (fills in 10 ms)
  //   AUDIO_BYTES_PER_FRAME (3200) / 320 = 10 slots -> drains in exact multiples
  //   dma_desc_num=16   -> 16 x 320 = 5120 bytes total buffer = 160 ms capacity
  chan_cfg.dma_desc_num  = 16;
  chan_cfg.dma_frame_num = 160;
  chan_cfg.auto_clear    = true;

  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &i2s_rx_handle);
  if (err != ESP_OK) {
    Serial.printf("I2S channel error: %d\n", err);
    return;
  }

  i2s_pdm_rx_config_t pdm_rx_cfg = {
    .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .clk = (gpio_num_t)I2S_MIC_CLK_PIN,
      .din = (gpio_num_t)I2S_MIC_DATA_PIN,
      .invert_flags = { .clk_inv = false },
    },
  };

  err = i2s_channel_init_pdm_rx_mode(i2s_rx_handle, &pdm_rx_cfg);
  if (err != ESP_OK) {
    Serial.printf("I2S PDM init error: %d\n", err);
    i2s_del_channel(i2s_rx_handle);
    i2s_rx_handle = NULL;
    return;
  }

  i2s_channel_enable(i2s_rx_handle);
  Serial.println("Microphone ready.");
}

void deinitMicrophone() {
  if (i2s_rx_handle) {
    i2s_channel_disable(i2s_rx_handle);
    i2s_del_channel(i2s_rx_handle);
    i2s_rx_handle = NULL;
  }
}

// ================= TIMELAPSE MODE =================
// Runs in taskDisplay context (Core 1). Camera is reinited for HD JPEG.
// taskCapture is idle (STATE_TIMELAPSE is in its skip list).
// Encoder events consumed directly from inputEventQueue (like video playback).
// Exit via: ENC_LONG, shutter button (appState changed externally by taskInput),
// or SD card removal.
static void runTimelapse() {
  int interval = camSettings.timelapse_interval;
  if (interval < 1) interval = 10;

  Serial.printf("[TL] Starting timelapse, interval=%ds\n", interval);

  // Camera was deinited during menu navigation — reinit for HD photo capture
  initCamera(PHOTO_MODE, PHOTO_RESOLUTION, camSettings.jpeg_quality);

  // Warm up sensor (auto-exposure settle)
  for (int i = 0; i < 10; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  int captureCount = 0;
  bool paused = false;
  bool tlStopped = false;
  unsigned long startTime = millis();
  unsigned long lastCaptureTime = 0;  // 0 = capture immediately on first iteration
  unsigned long lastDisplayUpdate = 0;

  // Draw initial UI chrome (title + footer — redrawn only once)
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN);
    tft.drawString("TIMELAPSE", tft.width() / 2, 16);
    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }

  while (appState == STATE_TIMELAPSE && !tlStopped) {
    // ── Consume encoder events ──
    InputEvent ev;
    while (xQueueReceive(inputEventQueue, &ev, 0) == pdTRUE) {
      if (ev.type == INPUT_ENC_CLICK) {
        paused = !paused;
        // Immediate visual feedback for pause toggle
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50))) {
          tft.fillRect(0, 50, 320, 40, TFT_BLACK);
          tft.setTextDatum(middle_center);
          tft.setTextSize(3);
          if (paused) {
            tft.setTextColor(TFT_YELLOW);
            tft.drawString("PAUSED", tft.width() / 2, 70);
          }
          tft.setTextDatum(top_left);
          xSemaphoreGive(spiMutex);
        }
        lastDisplayUpdate = 0; // force redraw on resume
      } else if (ev.type == INPUT_ENC_LONG) {
        tlStopped = true;
      }
    }
    if (tlStopped) break;

    // ── Paused: just wait ──
    if (paused) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    unsigned long now = millis();
    unsigned long sinceCapture = (lastCaptureTime == 0) ? (unsigned long)interval * 1000 : (now - lastCaptureTime);
    int remaining = interval - (int)(sinceCapture / 1000);
    if (remaining < 0) remaining = 0;

    // ── Time to capture? ──
    if (sinceCapture >= (unsigned long)interval * 1000) {
      // Show capturing indicator
      if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50))) {
        tft.fillRect(0, 50, 320, 40, TFT_BLACK);
        tft.setTextDatum(middle_center);
        tft.setTextSize(2);
        tft.setTextColor(TFT_GREEN);
        tft.drawString("CAPTURING...", tft.width() / 2, 70);
        tft.setTextDatum(top_left);
        xSemaphoreGive(spiMutex);
      }

      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        bool saved = false;
        if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
          if (globalSDState.isMounted) {
            String path = "/hd_pic_" + String(pictureNumber) + ".jpg";
            while (SD.exists(path.c_str())) {
              pictureNumber++;
              path = "/hd_pic_" + String(pictureNumber) + ".jpg";
            }
            File file = SD.open(path.c_str(), FILE_WRITE);
            if (file) {
              size_t written = file.write(fb->buf, fb->len);
              file.close();
              if (written == fb->len) {
                pictureNumber++;
                { Preferences p; p.begin("cnt", false); p.putInt("pic", pictureNumber); p.end(); }
                captureCount++;
                saved = true;
                Serial.printf("[TL] Saved: %s (%u bytes)\n", path.c_str(), fb->len);
              } else {
                Serial.printf("[TL] Write incomplete: %u/%u bytes\n", written, fb->len);
              }
            }
          }
          xSemaphoreGive(spiMutex);
        }
        esp_camera_fb_return(fb);

        // Show save result
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50))) {
          tft.fillRect(0, 140, 320, 20, TFT_BLACK);
          tft.setTextDatum(middle_center);
          tft.setTextSize(1);
          if (saved) {
            tft.setTextColor(TFT_GREEN);
            char msg[32];
            snprintf(msg, sizeof(msg), "PIC #%d saved", pictureNumber - 1);
            tft.drawString(msg, tft.width() / 2, 150);
          } else {
            tft.setTextColor(TFT_RED);
            tft.drawString("Save failed!", tft.width() / 2, 150);
          }
          tft.setTextDatum(top_left);
          xSemaphoreGive(spiMutex);
        }

        // SD removed during timelapse — stop
        if (!saved && !globalSDState.isMounted) {
          Serial.println("[TL] SD removed, stopping.");
          break;
        }
      }
      lastCaptureTime = millis();
    }

    // ── Update display (~500ms rate) ──
    now = millis();
    if (now - lastDisplayUpdate >= 500) {
      lastDisplayUpdate = now;
      unsigned long elapsedSec = (now - startTime) / 1000;
      // Recalculate remaining after potential capture
      if (lastCaptureTime > 0) {
        remaining = interval - (int)((now - lastCaptureTime) / 1000);
        if (remaining < 0) remaining = 0;
      }

      if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50))) {
        // Countdown
        tft.fillRect(0, 50, 320, 40, TFT_BLACK);
        tft.setTextDatum(middle_center);
        tft.setTextSize(3);
        tft.setTextColor(TFT_GREEN);
        char countBuf[8];
        snprintf(countBuf, sizeof(countBuf), "%d", remaining);
        tft.drawString(countBuf, tft.width() / 2, 70);

        // Info line 1: photo count + interval
        tft.fillRect(0, 100, 320, 35, TFT_BLACK);
        tft.setTextSize(1);
        tft.setTextColor(TFT_WHITE);
        char infoBuf[40];
        snprintf(infoBuf, sizeof(infoBuf), "Photos: %d   Interval: %ds", captureCount, interval);
        tft.drawString(infoBuf, tft.width() / 2, 110);

        // Info line 2: elapsed time
        snprintf(infoBuf, sizeof(infoBuf), "Elapsed: %02lu:%02lu",
                 elapsedSec / 60, elapsedSec % 60);
        tft.drawString(infoBuf, tft.width() / 2, 125);

        // Footer
        tft.fillRect(0, 218, 320, 22, TFT_BLACK);
        tft.setTextColor(0x4208);
        tft.drawString("Click: Pause  Long: Stop", tft.width() / 2, 228);

        tft.setTextDatum(top_left);
        xSemaphoreGive(spiMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }

  Serial.printf("[TL] Timelapse ended. %d photos in %lus.\n",
                captureCount, (millis() - startTime) / 1000);

  // Reinit camera to IDLE preview mode and return
  initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    xSemaphoreGive(spiMutex);
  }
  appState = STATE_IDLE;
}

// ================= MENU HELPERS =================

// Enter main menu from IDLE: deinit camera, set state, trigger redraw.
// Camera is powered down during menu navigation to free PSRAM and reduce power.
void enterMenuMain() {
  appState = STATE_MENU_MAIN;
  vTaskDelay(pdMS_TO_TICKS(150));
  esp_camera_deinit();
  Serial.println("[MENU] Camera deinited for menu");
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    xSemaphoreGive(spiMutex);
  }
  if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
}

// Exit menu back to IDLE: reinit camera, clear screen.
void exitMenuToIdle() {
  freeGalleryFiles();
  initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    xSemaphoreGive(spiMutex);
  }
  appState = STATE_IDLE;
}
