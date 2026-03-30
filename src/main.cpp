#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <EEPROM.h>
#include "esp_camera.h"
#include "esp_wifi.h"
#include "esp_bt.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "img_converters.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/i2s_pdm.h"
#include <atomic>

//   TFT Pin         ->     XIAO ESP32-S3
//     VCC           ->          3V3
//     GND           ->          GND
//     CS            ->         GPIO1
//    RESET          ->         GPIO3
//     A0            ->         GPIO2
//     SDA           ->         GPIO9
//     SCK           ->         GPIO7
//     LED           ->          3V3

// SHUTTER BUTTON    ->    XIAO ESP32-S3
//      1            ->        GPIO4
//      2            ->        GPIO5

// ================= EXTERNAL PINS =================
#define TFT_CS            1
#define TFT_DC            2
#define TFT_RST           3
#define TFT_SCK           7
#define TFT_MISO          8
#define TFT_MOSI          9

#define BTN_PIN           0  // boot button on XIAO (used for mirroring the image)
#define SD_CS_PIN         21 // SD card CS pin on XIAO ESP32S3 Sense

#define SHUTTER_BTN_PIN_1 4
#define SHUTTER_BTN_PIN_2 5

// ================= MIC PINS (XIAO ESP32-S3 Sense built-in PDM) =================
#define I2S_MIC_CLK_PIN   42
#define I2S_MIC_DATA_PIN  41

// ================= CAM PINS (XIAO ESP32-S3 Sense) =================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// ================= EEPROM SETTINGS =================
#define EEPROM_SIZE       64
#define EEPROM_ADDR_PIC   0
#define EEPROM_ADDR_VID   4

// ================= CAMERA SETTINGS =================
#define PHOTO_MODE        PIXFORMAT_JPEG
#define PHOTO_RESOLUTION  FRAMESIZE_HD

#define VIDEO_MODE        PIXFORMAT_JPEG
#define VIDEO_RESOLUTION  FRAMESIZE_HVGA

#define IDLE_MODE         PIXFORMAT_RGB565
#define IDLE_RESOLUTION   FRAMESIZE_QVGA

#define REC_JPEG_QUALITY  12
#define IDLE_JPEG_QUALITY 0

// ================= AUDIO SETTINGS =================
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_REC_BUF_SIZE  8192

// ================= AVI CAPACITY =================
// Pre-sized for max ~15 min @ 10 FPS. Allocated once at boot to avoid PSRAM fragmentation.
#define MAX_AVI_FRAMES  9000          // 10 FPS × 900 sec
#define MAX_AVI_CHUNKS  (MAX_AVI_FRAMES * 2)  // V+A interleaved

// ================= APP STATE & EVENTS =================
enum AppState { STATE_IDLE, STATE_RECORDING, STATE_PHOTO, STATE_STOPPING };
volatile AppState appState = STATE_IDLE;

#define EVT_START_RECORDING  (1 << 0)
#define EVT_STOP_RECORDING   (1 << 1)
#define EVT_TAKE_PHOTO       (1 << 2)
#define EVT_SD_STOP          (1 << 3)
EventGroupHandle_t appEvents;

// ================= STRUCTS & GLOBALS =================
struct SDStatus {
  bool isMounted;
  bool isFull;
  String errorMsg;
};

SDStatus globalSDState = {false, false, "NO CARD"};
int pictureNumber = 0;
int videoNumber = 0;
bool mirror = true;
unsigned long recordingStartTime = 0;
File videoFile;

// Single SPI mutex: TFT and SD share the same SPI bus (SPI2_HOST),
// so all SPI operations must be serialized to prevent bus corruption.
SemaphoreHandle_t spiMutex;

camera_config_t config;

// ================= AVI HEADER VARIABLES =================
const int avi_header_size = 324;
long avi_movi_size = 0;
long avi_index_size = 0;
unsigned long avi_start_time = 0;
int avi_total_frames = 0;
uint32_t *avi_frame_sizes = nullptr;
const int avi_frame_capacity = MAX_AVI_FRAMES;  // fixed at boot, never changes
uint32_t *avi_audio_sizes = nullptr;
int avi_total_audio_chunks = 0;
uint8_t *avi_chunk_order = nullptr;  // 0=video, 1=audio — tracks actual file layout for correct idx1
int avi_total_chunks = 0;

// ================= MICROPHONE =================
i2s_chan_handle_t i2s_rx_handle = NULL;

// ================= FRAME MANAGEMENT =================
// Display double buffer (ping-pong): decouples capture from TFT rendering
#define FRAME_BUF_SIZE     (160 * 1024)  // 160KB covers both QVGA RGB565 and HVGA JPEG
uint8_t *dispBuf[2] = {nullptr, nullptr};
volatile size_t dispLen[2] = {0, 0};
// std::atomic gives correct memory-ordering guarantees on ESP32-S3 SMP.
// volatile alone is insufficient for inter-core visibility.
std::atomic<int> dispWriteSlot{0};
std::atomic<int> dispReadSlot{-1};
std::atomic<int> dispRendering{-1};  // slot currently being rendered by taskDisplay (-1 = none)
TaskHandle_t taskDisplayHandle = NULL;

// Recorder frame pool: 3 PSRAM slots so capture never blocks on recorder
#define REC_POOL_SIZE  3
uint8_t *recPool[REC_POOL_SIZE] = {nullptr, nullptr, nullptr};
typedef struct { int idx; size_t len; } RecFrame;
QueueHandle_t recFrameQueue = NULL;
SemaphoreHandle_t recPoolFree = NULL;
int recPoolWriteIdx = 0;

// ================= AUDIO BUFFER =================
int16_t *audioRecBuf = nullptr;  // direct I2S read buffer for recorder

// ================= TFT CLASS =================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX() {
    auto bcfg = _bus.config();
    bcfg.spi_host = SPI2_HOST;
    bcfg.spi_mode = 0;
    bcfg.freq_write = 80000000; // 80 MHz
    bcfg.pin_sclk = TFT_SCK;
    bcfg.pin_mosi = TFT_MOSI;
    bcfg.pin_miso = TFT_MISO;
    bcfg.pin_dc   = TFT_DC;
    _bus.config(bcfg);
    _panel.setBus(&_bus);

    auto pcfg = _panel.config();
    pcfg.pin_cs = TFT_CS;
    pcfg.pin_rst = TFT_RST;
    pcfg.panel_width  = 240;
    pcfg.panel_height = 320;
    pcfg.offset_x = 0;
    pcfg.offset_y = 0;
    pcfg.invert = true;
    pcfg.rgb_order = false;
    pcfg.readable = false;

    _panel.config(pcfg);

    setPanel(&_panel);
  }
};

LGFX tft;

// ================= FUNCTION PROTOTYPES =================
void taskSDMonitor(void *pvParameters);
void taskCapture(void *pvParameters);
void taskDisplay(void *pvParameters);
void taskRecorder(void *pvParameters);
void taskInput(void *pvParameters);
void savePhotoHighRes();
void initCamera(pixformat_t format, framesize_t size, int jpeg_quality);
void startAVI(File &file, int fps, int width, int height);
bool writeAVIFrameFromBuf(File &file, uint8_t *buf, size_t len);
bool writeAVIAudioChunk(File &file, uint8_t *buf, size_t bytes);
void endAVI(File &file, int fps, int width, int height);
void initMicrophone();
void deinitMicrophone();
void allocateBuffers();

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  esp_wifi_stop();
  esp_bt_controller_disable();

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(SHUTTER_BTN_PIN_1, OUTPUT);
  pinMode(SHUTTER_BTN_PIN_2, INPUT_PULLUP);

  digitalWrite(SHUTTER_BTN_PIN_1, LOW);

  spiMutex = xSemaphoreCreateMutex();
  appEvents = xEventGroupCreate();
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

  EEPROM.begin(EEPROM_SIZE);

  pictureNumber = EEPROM.readInt(EEPROM_ADDR_PIC);
  if (pictureNumber < 0 || pictureNumber > 10000) {
    pictureNumber = 0;
    EEPROM.writeInt(EEPROM_ADDR_PIC, pictureNumber);
  }

  videoNumber = EEPROM.readInt(EEPROM_ADDR_VID);
  Serial.printf("Initial video no: %d\n", videoNumber);
  if (videoNumber < 0 || videoNumber > 10000) {
    videoNumber = 0;
    EEPROM.writeInt(EEPROM_ADDR_VID, videoNumber);
  }

  EEPROM.commit();

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

  delay(3000);

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

  Serial.println("Cam is ready. Tasks launched.");
}

// ================= LOOP =================
void loop() {
  vTaskDelete(NULL);
}

// ================= BUFFER ALLOCATION =================
void allocateBuffers() {
  for (int i = 0; i < 2; i++) {
    dispBuf[i] = (uint8_t*)ps_malloc(FRAME_BUF_SIZE);
    if (!dispBuf[i]) Serial.printf("[ERR] dispBuf[%d] alloc failed!\n", i);
  }
  for (int i = 0; i < REC_POOL_SIZE; i++) {
    recPool[i] = (uint8_t*)ps_malloc(FRAME_BUF_SIZE);
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



// ================= TASK: SD MONITOR (Core 0, Priority 1) =================
void taskSDMonitor(void *pvParameters) {
  while (true) {
    if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {

      // during recording: only check free space
      if (appState == STATE_RECORDING) {
        bool shouldStop = false;
        if (SD.cardType() == CARD_NONE || SD.totalBytes() == 0) {
          shouldStop = true;
          Serial.println("SD card removed during recording!");
        }
        else {
          uint64_t freeBytes = SD.totalBytes() - SD.usedBytes();
          Serial.println("Free space during recording: " + String(freeBytes / 1024) + " KB");
          if (freeBytes < 10 * 1024 * 1024) {
            shouldStop = true;
            Serial.println("SD card nearly full during recording!");
          }
        }
        if (shouldStop) {
          xEventGroupSetBits(appEvents, EVT_SD_STOP);
        }
        xSemaphoreGive(spiMutex);
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }

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
void taskInput(void *pvParameters) {
  bool lastBtn = HIGH;
  bool lastShutter = HIGH;
  unsigned long shutterPressTime = 0;

  while (true) {
    // Mirror button (boot button)
    bool btnNow = digitalRead(BTN_PIN);
    if (lastBtn == HIGH && btnNow == LOW) {
      mirror = !mirror;
      sensor_t *s = esp_camera_sensor_get();
      if (s) s->set_hmirror(s, mirror);
    }
    lastBtn = btnNow;

    // Shutter button
    int shutterState = digitalRead(SHUTTER_BTN_PIN_2);

    // pressed
    if (lastShutter == HIGH && shutterState == LOW) {
      shutterPressTime = millis();
    }
    // released
    else if (lastShutter == LOW && shutterState == HIGH) {
      unsigned long duration = millis() - shutterPressTime;

      // short press -> take photo (only in idle)
      if (appState == STATE_IDLE && duration < 1000) {
        bool canSave = false;
        String errMsg = "";
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100))) {
          if (!globalSDState.isMounted) errMsg = "NO SD CARD!";
          else if (globalSDState.isFull) errMsg = "SD FULL!";
          else canSave = true;
          xSemaphoreGive(spiMutex);
        }

        if (canSave) {
          xEventGroupSetBits(appEvents, EVT_TAKE_PHOTO);
        } else {
          if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
            tft.setTextSize(2);
            tft.setCursor(10, 10);
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.print(errMsg);
            xSemaphoreGive(spiMutex);
          }
        }
      }
      // release during recording -> stop (only after 2 sec since start)
      else if (appState == STATE_RECORDING && millis() - recordingStartTime > 2000) {
        xEventGroupSetBits(appEvents, EVT_STOP_RECORDING);
      }
    }

    // long press -> start recording (only in idle)
    if (shutterState == LOW && appState == STATE_IDLE && (millis() - shutterPressTime > 1000)) {
      bool cardReady = false;
      String errorMsg = "";
      if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100))) {
        if (!globalSDState.isMounted) errorMsg = "NO SD CARD!";
        else if (globalSDState.isFull) errorMsg = "SD FULL!";
        else cardReady = true;
        xSemaphoreGive(spiMutex);
      }

      if (cardReady) {
        xEventGroupSetBits(appEvents, EVT_START_RECORDING);
        // wait until state changes so we don't re-trigger
        while (appState == STATE_IDLE && digitalRead(SHUTTER_BTN_PIN_2) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(50));
        }
      }
      else {
        if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
          tft.setTextSize(2);
          tft.setCursor(10, 10);
          tft.setTextColor(TFT_RED, TFT_BLACK);
          tft.print(errorMsg);
          xSemaphoreGive(spiMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    lastShutter = shutterState;
    vTaskDelay(pdMS_TO_TICKS(20)); // 50Hz polling
  }
}

// ================= TASK: CAMERA CAPTURE (Core 1, Priority 6) =================
void taskCapture(void *pvParameters) {
  int fail_count = 0;

  while (true) {
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
        Serial.printf("Video Start: %s\n", path.c_str());
        videoFile = SD.open(path.c_str(), FILE_WRITE);
        if (videoFile) {
          videoNumber++;
          EEPROM.writeInt(EEPROM_ADDR_VID, videoNumber);
          EEPROM.commit();
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
    // Skip if taskDisplay is currently rendering this slot
    int ws = dispWriteSlot.load();
    if (ws != dispRendering.load() && fb->len <= FRAME_BUF_SIZE && dispBuf[ws]) {
      memcpy(dispBuf[ws], fb->buf, fb->len);
      dispLen[ws] = fb->len;
      dispReadSlot.store(ws);
      dispWriteSlot.store(1 - ws);
      if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
    }

    // Copy frame for recorder (if recording)
    if (appState == STATE_RECORDING && fb->format == PIXFORMAT_JPEG) {
      if (xSemaphoreTake(recPoolFree, 0) == pdTRUE) {
        int idx = recPoolWriteIdx;
        recPoolWriteIdx = (recPoolWriteIdx + 1) % REC_POOL_SIZE;
        if (recPool[idx] && fb->len <= FRAME_BUF_SIZE) {
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

    vTaskDelay(1);
  }
}

// ================= TASK: TFT DISPLAY (Core 1, Priority 3) =================
void taskDisplay(void *pvParameters) {
  uint32_t last_fps_time = 0;
  uint32_t frame_count = 0;
  float fps = 0;

  while (true) {
    // wait for a new frame notification (200ms timeout for FPS update even if no frames)
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));

    // Don't render during photo/stopping — let notifications stay on screen
    if (appState == STATE_PHOTO || appState == STATE_STOPPING) {
      vTaskDelay(pdMS_TO_TICKS(50));
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
        sprintf(timeStr, "%02d:%02d:%02d", (int)(elapsed / 3600), (int)((elapsed % 3600) / 60), (int)(elapsed % 60));
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

      if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(10))) {
        tft.fillRect(0, 216, 320, 24, TFT_BLACK);

        tft.setTextSize(2);
        tft.setCursor(8, 220);
        tft.setTextColor(TFT_GREEN);
        tft.printf("FPS: %.1f", fps);

        if (globalSDState.isMounted && !globalSDState.isFull) {
          tft.fillCircle(304, 228, 5, TFT_GREEN);
        }
        else {
          tft.fillCircle(304, 228, 5, TFT_RED);
        }

        xSemaphoreGive(spiMutex);
      }

      frame_count = 0;
      last_fps_time = now;
    }
  }
}

// ================= TASK: AVI RECORDER (Core 0, Priority 4) =================
void taskRecorder(void *pvParameters) {
  while (true) {
    if (appState != STATE_RECORDING && appState != STATE_STOPPING) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    RecFrame rf;
    if (xQueueReceive(recFrameQueue, &rf, pdMS_TO_TICKS(100)) == pdTRUE) {
      // Read audio right before SD write (non-blocking, matches original code)
      size_t audio_bytes_read = 0;
      if (i2s_rx_handle && audioRecBuf) {
        i2s_channel_read(i2s_rx_handle, audioRecBuf, AUDIO_REC_BUF_SIZE, &audio_bytes_read, 0);
      }

      // portMAX_DELAY: never silently drop a frame. If SD is slow, backpressure
      // naturally propagates via recPoolFree semaphore to throttle capture.
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
void savePhotoHighRes() {
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
        Serial.printf("Saving (%u bytes): %s\n", fb->len, path.c_str());

        File file = SD.open(path.c_str(), FILE_WRITE);
        if (file) {
          file.write(fb->buf, fb->len);
          file.close();
          Serial.println("SUCCESS!");

          pictureNumber++;
          EEPROM.writeInt(EEPROM_ADDR_PIC, pictureNumber);
          EEPROM.commit();
          saved = true;
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
  config.fb_count = (format == PIXFORMAT_RGB565) ? 3 : 2;  // fewer JPEG bufs to reduce PSRAM contention
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Cam init error");
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 1);
    s->set_contrast(s, 0);
    s->set_saturation(s, 2);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_ae_level(s, 0);

    if (format == PIXFORMAT_JPEG) {
      s->set_gainceiling(s, GAINCEILING_16X);
    }
    else {
      s->set_gainceiling(s, GAINCEILING_4X);
    }

    s->set_hmirror(s, mirror);
  }
  delay(100);
}

// ================= MICROPHONE =================
void initMicrophone() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 512;
  chan_cfg.auto_clear = true;

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

// ================= AVI WRITER =================
void startAVI(File &file, int fps, int width, int height) {
  avi_movi_size = 0;
  avi_total_frames = 0;
  avi_total_audio_chunks = 0;
  avi_total_chunks = 0;
  avi_start_time = millis();

  // Arrays are pre-allocated at boot — just zero them to clear stale data from last session.
  // This avoids PSRAM malloc/free cycles that cause heap fragmentation over multiple recordings.
  if (avi_frame_sizes) memset(avi_frame_sizes, 0, MAX_AVI_FRAMES * sizeof(uint32_t));
  if (avi_audio_sizes) memset(avi_audio_sizes, 0, MAX_AVI_FRAMES * sizeof(uint32_t));
  if (avi_chunk_order) memset(avi_chunk_order, 0, MAX_AVI_CHUNKS);

  uint8_t zero_buf[avi_header_size];
  memset(zero_buf, 0, avi_header_size);
  file.write(zero_buf, avi_header_size);
}

bool writeAVIFrameFromBuf(File &file, uint8_t *buf, size_t len) {
  uint8_t dc_buf[4] = {0x30, 0x30, 0x64, 0x63}; // "00dc"
  size_t w1 = file.write(dc_buf, 4);

  uint32_t rem = len % 4;
  uint32_t padding = (rem == 0) ? 0 : 4 - rem;
  uint32_t totalLen = len + padding;

  size_t w2 = file.write((uint8_t*)&totalLen, 4);
  size_t w3 = file.write(buf, len);

  if (padding > 0) {
    uint8_t pad[3] = {0, 0, 0};
    file.write(pad, padding);
  }

  if (w1 != 4 || w2 != 4 || w3 != len) {
    return false;
  }

  avi_movi_size += (totalLen + 8);
  if (avi_frame_sizes && avi_total_frames < avi_frame_capacity) {
    avi_frame_sizes[avi_total_frames] = totalLen;
  }
  avi_total_frames++;
  if (avi_chunk_order && avi_total_chunks < avi_frame_capacity * 2) {
    avi_chunk_order[avi_total_chunks++] = 0;
  }
  return true;
}

bool writeAVIAudioChunk(File &file, uint8_t *buf, size_t bytes) {
  uint8_t wb_tag[4] = {0x30, 0x31, 0x77, 0x62}; // "01wb"
  size_t w1 = file.write(wb_tag, 4);

  uint32_t len = bytes;
  uint32_t rem = len % 4;
  uint32_t pad = (rem == 0) ? 0 : 4 - rem;
  uint32_t totalLen = len + pad;

  size_t w2 = file.write((uint8_t*)&totalLen, 4);
  size_t w3 = file.write(buf, bytes);

  if (pad > 0) {
    uint8_t zeros[3] = {0, 0, 0};
    file.write(zeros, pad);
  }

  if (w1 != 4 || w2 != 4 || w3 != bytes) return false;

  avi_movi_size += (totalLen + 8);
  if (avi_audio_sizes && avi_total_audio_chunks < avi_frame_capacity) {
    avi_audio_sizes[avi_total_audio_chunks] = totalLen;
  }
  avi_total_audio_chunks++;
  if (avi_chunk_order && avi_total_chunks < avi_frame_capacity * 2) {
    avi_chunk_order[avi_total_chunks++] = 1;
  }
  return true;
}

void endAVI(File &file, int fps, int width, int height) {
  unsigned long duration = millis() - avi_start_time;
  float real_fps = (float)avi_total_frames / (duration / 1000.0);
  if (real_fps <= 0) real_fps = (float)fps;

  uint32_t total_audio_samples = (uint32_t)((duration / 1000.0) * AUDIO_SAMPLE_RATE);

  // step 1: write idx1 at end of file
  file.seek(0, SeekEnd);
  file.write((const uint8_t*)"idx1", 4);
  int total_idx_entries = avi_total_chunks;
  uint32_t idx1Size = total_idx_entries * 16;
  file.write((uint8_t*)&idx1Size, 4);

  // Build idx1 in actual chunk write order (not assumed V-A interleaving)
  uint32_t chunkOffset = 4;
  int vi = 0, ai = 0;
  for (int i = 0; i < avi_total_chunks; i++) {
    if (avi_chunk_order && avi_chunk_order[i] == 0) {
      uint32_t fSize = (avi_frame_sizes && vi < avi_frame_capacity) ? avi_frame_sizes[vi] : 0;
      file.write((const uint8_t*)"00dc", 4);
      uint32_t kf = 0x10;  // AVIIF_KEYFRAME
      file.write((uint8_t*)&kf, 4);
      file.write((uint8_t*)&chunkOffset, 4);
      file.write((uint8_t*)&fSize, 4);
      chunkOffset += fSize + 8;
      vi++;
    } else {
      uint32_t aSize = (avi_audio_sizes && ai < avi_frame_capacity) ? avi_audio_sizes[ai] : 0;
      file.write((const uint8_t*)"01wb", 4);
      uint32_t noKf = 0x00;
      file.write((uint8_t*)&noKf, 4);
      file.write((uint8_t*)&chunkOffset, 4);
      file.write((uint8_t*)&aSize, 4);
      chunkOffset += aSize + 8;
      ai++;
    }
  }

  // step 2: rewrite header at position 0
  file.seek(0);

  uint32_t totalSize = file.size();
  uint32_t riffSize = totalSize - 8;
  uint32_t microSecPerFrame = (uint32_t)(1000000.0 / real_fps);
  uint32_t maxBytesPerSec = (uint32_t)((width * height * 2) * real_fps) + AUDIO_SAMPLE_RATE * 2;

  file.write((const uint8_t*)"RIFF", 4);
  file.write((uint8_t*)&riffSize, 4);
  file.write((const uint8_t*)"AVI ", 4);

  file.write((const uint8_t*)"LIST", 4);
  uint32_t hdrlSize = 292;
  file.write((uint8_t*)&hdrlSize, 4);
  file.write((const uint8_t*)"hdrl", 4);

  file.write((const uint8_t*)"avih", 4);
  uint32_t avihSize = 56;
  file.write((uint8_t*)&avihSize, 4);

  file.write((uint8_t*)&microSecPerFrame, 4);
  file.write((uint8_t*)&maxBytesPerSec, 4);
  uint32_t padding = 0;
  file.write((uint8_t*)&padding, 4);
  uint32_t avih_flags = 0x10;  // AVIF_HASINDEX
  file.write((uint8_t*)&avih_flags, 4);
  file.write((uint8_t*)&avi_total_frames, 4);
  file.write((uint8_t*)&padding, 4);
  uint32_t streams = 2;  // video + audio
  file.write((uint8_t*)&streams, 4);
  uint32_t bufSize = width * height * 2;
  file.write((uint8_t*)&bufSize, 4);
  file.write((uint8_t*)&width, 4);
  file.write((uint8_t*)&height, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);

  file.write((const uint8_t*)"LIST", 4);
  uint32_t strlSize = 116;
  file.write((uint8_t*)&strlSize, 4);
  file.write((const uint8_t*)"strl", 4);

  file.write((const uint8_t*)"strh", 4);
  uint32_t strhSize = 56;
  file.write((uint8_t*)&strhSize, 4);
  file.write((const uint8_t*)"vids", 4);
  file.write((const uint8_t*)"MJPG", 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  uint32_t scale = 1;
  file.write((uint8_t*)&scale, 4);
  uint32_t rate = (uint32_t)real_fps;
  if (rate == 0) rate = 10;
  file.write((uint8_t*)&rate, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&avi_total_frames, 4);
  file.write((uint8_t*)&bufSize, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);

  file.write((const uint8_t*)"strf", 4);
  uint32_t strfSize = 40;
  file.write((uint8_t*)&strfSize, 4);
  file.write((uint8_t*)&strfSize, 4);
  file.write((uint8_t*)&width, 4);
  file.write((uint8_t*)&height, 4);
  uint16_t planes = 1;
  file.write((uint8_t*)&planes, 2);
  uint16_t bitCount = 24;
  file.write((uint8_t*)&bitCount, 2);
  file.write((const uint8_t*)"MJPG", 4);
  uint32_t imageSize = width * height * 3;
  file.write((uint8_t*)&imageSize, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);

  // audio stream LIST (strl)
  file.write((const uint8_t*)"LIST", 4);
  uint32_t strlSize_aud = 92;
  file.write((uint8_t*)&strlSize_aud, 4);
  file.write((const uint8_t*)"strl", 4);

  // audio strh
  file.write((const uint8_t*)"strh", 4);
  file.write((uint8_t*)&strhSize, 4);
  file.write((const uint8_t*)"auds", 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  uint32_t audio_scale = 1;
  file.write((uint8_t*)&audio_scale, 4);
  uint32_t audio_rate = AUDIO_SAMPLE_RATE;
  file.write((uint8_t*)&audio_rate, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&total_audio_samples, 4);
  uint32_t audio_buf_suggest = AUDIO_SAMPLE_RATE * 2;
  file.write((uint8_t*)&audio_buf_suggest, 4);
  file.write((uint8_t*)&padding, 4);
  uint32_t audio_sample_size = 2;
  file.write((uint8_t*)&audio_sample_size, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);

  // audio strf (PCMWAVEFORMAT)
  file.write((const uint8_t*)"strf", 4);
  uint32_t strfSize_aud = 16;
  file.write((uint8_t*)&strfSize_aud, 4);
  uint16_t wFormatTag = 1;  // PCM
  file.write((uint8_t*)&wFormatTag, 2);
  uint16_t nChannels = 1;
  file.write((uint8_t*)&nChannels, 2);
  uint32_t nSamplesPerSec = AUDIO_SAMPLE_RATE;
  file.write((uint8_t*)&nSamplesPerSec, 4);
  uint32_t nAvgBytesPerSec = AUDIO_SAMPLE_RATE * 2;
  file.write((uint8_t*)&nAvgBytesPerSec, 4);
  uint16_t nBlockAlign = 2;
  file.write((uint8_t*)&nBlockAlign, 2);
  uint16_t wBitsPerSample = 16;
  file.write((uint8_t*)&wBitsPerSample, 2);

  // movi LIST header
  file.write((const uint8_t*)"LIST", 4);
  uint32_t moviListSize = 4 + avi_movi_size;
  file.write((uint8_t*)&moviListSize, 4);
  file.write((const uint8_t*)"movi", 4);

  // AVI metadata arrays are intentionally kept alive (never freed).
  // They are boot-allocated and reused across sessions via memset in startAVI().

  file.close();
}
