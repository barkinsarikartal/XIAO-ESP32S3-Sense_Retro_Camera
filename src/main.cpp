#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "esp_camera.h"
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
#include <memory>

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

// ================= NVS NAMESPACES =================
// "cnt" — file counters (pictureNumber, videoNumber)
// "cam" — camera sensor settings (CameraSettings struct)

// ================= CAMERA SETTINGS =================
#define PHOTO_MODE        PIXFORMAT_JPEG
#define PHOTO_RESOLUTION  FRAMESIZE_HD

#define VIDEO_MODE        PIXFORMAT_JPEG
#define VIDEO_RESOLUTION  FRAMESIZE_HVGA

#define IDLE_MODE         PIXFORMAT_RGB565
#define IDLE_RESOLUTION   FRAMESIZE_QVGA

#define REC_JPEG_QUALITY  12
#define IDLE_JPEG_QUALITY 0

// ================= FIRMWARE VERSION =================
#define FIRMWARE_VERSION "v0.7"

// ================= WIFI AP CONFIG =================
#define WIFI_SSID "Retro_Cam"
#define WIFI_PASS "barkinsarikartal"

// ================= DEBUG FLAGS =================
// Set to 1 to enable Serial.printf for every InputEvent sent.
#define DEBUG_INPUT 1

// ================= AUDIO SETTINGS =================
#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_REC_BUF_SIZE       8192

// ================= VIDEO / AUDIO TIMING =================
// A single constant for declared FPS keeps the AVI header, the audio chunk
// size, and the I2S DMA geometry all in sync with each other.
#define TARGET_FPS               10
#define AUDIO_SAMPLES_PER_FRAME  (AUDIO_SAMPLE_RATE / TARGET_FPS)   // 1600 samples
#define AUDIO_BYTES_PER_FRAME    (AUDIO_SAMPLES_PER_FRAME * 2)      // 3200 bytes

// ================= AVI CAPACITY =================
// Pre-sized for max ~15 min @ 10 FPS. Allocated once at boot to avoid PSRAM fragmentation.
#define MAX_AVI_FRAMES  9000          // 10 FPS × 900 sec
#define MAX_AVI_CHUNKS  (MAX_AVI_FRAMES * 2)  // V+A interleaved

// ================= APP STATE & EVENTS =================
enum AppState {
  STATE_IDLE,
  STATE_RECORDING,
  STATE_PHOTO,
  STATE_STOPPING,
  STATE_MENU_MAIN,
  STATE_GALLERY_TYPE,
  STATE_GALLERY_PHOTOS,
  STATE_GALLERY_VIDEOS,
  STATE_VIDEO_PLAYING,
  STATE_DELETE_CONFIRM,
  STATE_WIFI_MODE,
  STATE_SETTINGS,
};
volatile AppState appState = STATE_IDLE;

#define EVT_START_RECORDING  (1 << 0)
#define EVT_STOP_RECORDING   (1 << 1)
#define EVT_TAKE_PHOTO       (1 << 2)
#define EVT_SD_STOP          (1 << 3)
EventGroupHandle_t appEvents;

// ================= INPUT EVENT QUEUE =================
// Queue-based input pipeline replaces direct EventGroup GPIO signals.
enum InputEventType {
  INPUT_BTN_SHORT,   // shutter short press
  INPUT_BTN_LONG,    // shutter long press
  INPUT_BOOT_SHORT,  // boot button short press
  INPUT_BOOT_LONG,   // boot button long press
  INPUT_ENC_CW,      // encoder clockwise
  INPUT_ENC_CCW,     // encoder counter-clockwise
  INPUT_ENC_CLICK,   // encoder button short tap
  INPUT_ENC_LONG,    // encoder button long press (>500 ms)
};
struct InputEvent { InputEventType type; };
QueueHandle_t inputEventQueue = NULL;

// ================= WIFI FILE SERVER =================
AsyncWebServer *webServer = nullptr;

// ================= ENCODER PINS =================
#define ENC_AVAILABLE 1  // EC11 wired to GPIO6/43/44
#define ENC_CLK  6
#define ENC_DT   43
#define ENC_SW   44

// Quadrature state table — DRAM_ATTR ensures ISR-safe access during flash operations.
// Index = (prevState << 2) | currState, where state = (CLK << 1) | DT.
// +1 = CW step, -1 = CCW step, 0 = invalid/bounce (ignored).
static const DRAM_ATTR int8_t ENC_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};
volatile uint8_t encPrevState = 3;  // both HIGH at boot (INPUT_PULLUP default)
volatile int8_t  encAccum     = 0;  // direction accumulator between detents
volatile uint32_t encSwPressMs = 0; // timestamp of SW button press edge

// Debug counters: ISR increments, taskInput reads and prints the delta.
// Safe because uint32 reads/writes are atomic on Xtensa; no mutex needed.
// These are only active when DEBUG_INPUT=1.
#if DEBUG_INPUT
volatile uint32_t dbgEncCw    = 0;
volatile uint32_t dbgEncCcw   = 0;
volatile uint32_t dbgEncClick = 0;
volatile uint32_t dbgEncLong  = 0;
#endif

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

// ================= GALLERY =================
#define GALLERY_MAX_FILES  2000
#define GALLERY_NAME_LEN   24
char **galleryFiles = nullptr;
int galleryFileCount = 0;
int galleryIndex = 0;
int galleryTypeSelection = 0;   // 0=Photos, 1=Videos
int deleteSelection = 0;        // 0=Cancel, 1=Delete
volatile bool galleryNeedsRedraw = false;

// ================= CAMERA SETTINGS =================
// Persistent camera parameters, loaded from NVS at boot, applied after each initCamera.
struct CameraSettings {
  int brightness;     // -2..+2   (default: 1)
  int contrast;       // -2..+2   (default: 0)
  int saturation;     // -2..+2   (default: 2)
  int ae_level;       // -2..+2   (default: 0)
  int wb_mode;        // 0=Auto 1=Sunny 2=Cloudy 3=Office 4=Home
  int special_effect; // 0=None 1=Neg 2=Gray 3=Red 4=Green 5=Blue 6=Sepia
  int hmirror;        // 0/1      (default: 1)
  int vflip;          // 0/1      (default: 0)
  int jpeg_quality;   // 4=Max..63=Low, inverse scale (default: 12)
};
CameraSettings camSettings = { 1, 0, 2, 0, 0, 0, 1, 0, 12 };

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
static void drawStatusBar(float fps);
void loadCameraSettings();
void saveCameraSettings();
void applySettings(sensor_t *s);
void startWiFiMode();
void stopWiFiMode();
void scanGalleryFiles(bool videosOnly);
void freeGalleryFiles();
void drawGalleryTypeMenu();
void drawGalleryPhoto();
void drawGalleryVideoItem();
void drawDeleteConfirm();
void playVideoOnTFT();
#if ENC_AVAILABLE
void encoderISR();
void encSwISR();
#endif

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(8000);

  WiFi.mode(WIFI_OFF);
  esp_bt_controller_disable();

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(SHUTTER_BTN_PIN_1, OUTPUT);
  pinMode(SHUTTER_BTN_PIN_2, INPUT_PULLUP);

  digitalWrite(SHUTTER_BTN_PIN_1, LOW);

#if ENC_AVAILABLE
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);
#endif

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

  // Load file counters from NVS (replaces EEPROM)
  {
    Preferences p;
    p.begin("cnt", true);  // read-only
    pictureNumber = p.getInt("pic", 0);
    videoNumber   = p.getInt("vid", 0);
    p.end();
  }

  // Load camera settings from NVS
  loadCameraSettings();
  mirror = camSettings.hmirror;  // runtime mirror tracks saved preference

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

#if ENC_AVAILABLE
  // Attach encoder interrupts AFTER tasks are created so the queue exists.
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT),  encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_SW),  encSwISR,  CHANGE);
  Serial.println("Encoder ISRs attached (CLK+DT+SW).");
#endif

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
//
// Queue-based input pipeline.
// All physical inputs are translated into InputEvent and pushed to inputEventQueue.
// This decouples gesture classification from downstream state handling, and provides
// a natural extension point for encoder ISR which also pushes to the same queue.
//
// Temporary test mapping (encoder not yet wired):
//   SHUTTER short  → INPUT_BTN_SHORT   (photo / start rec — legacy path kept via EventGroup)
//   SHUTTER long   → INPUT_BTN_LONG    (start recording   — legacy path kept)
//   BOOT short     → INPUT_BOOT_SHORT  → mapped to INPUT_ENC_CCW (menu: previous)
//   BOOT long      → INPUT_BOOT_LONG   → mapped to INPUT_ENC_LONG (menu: back)
//   SHUTTER short  in STATE_IDLE       → also mapped to INPUT_ENC_CW  (menu: next)
//   SHUTTER long   in STATE_IDLE >1s   → also mapped to INPUT_ENC_CLICK (menu: select)
void taskInput(void *pvParameters) {
  bool lastShutter = HIGH;
  bool lastBoot    = HIGH;
  unsigned long shutterPressTime = 0;
  unsigned long bootPressTime    = 0;

  auto sendEvent = [](InputEventType t) {
    InputEvent e = { t };
    xQueueSend(inputEventQueue, &e, 0);
#if DEBUG_INPUT
    // Map enum to readable name for Serial output.
    static const char* const names[] = {
      "BTN_SHORT", "BTN_LONG", "BOOT_SHORT", "BOOT_LONG",
      "ENC_CW",   "ENC_CCW",  "ENC_CLICK",  "ENC_LONG"
    };
    Serial.printf("[INPUT] %s\n", (t < 8) ? names[t] : "?");
#endif
  };

  while (true) {
    bool shutterNow = digitalRead(SHUTTER_BTN_PIN_2);
    bool bootNow    = digitalRead(BTN_PIN);

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
          // Legacy: fire photo event
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
            if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
              tft.setTextSize(2);
              tft.setCursor(10, 10);
              tft.setTextColor(TFT_RED, TFT_BLACK);
              tft.print(errMsg);
              xSemaphoreGive(spiMutex);
            }
          }
        } else if (appState == STATE_RECORDING && millis() - recordingStartTime > 2000) {
          // Stop recording
          xEventGroupSetBits(appEvents, EVT_STOP_RECORDING);
          sendEvent(INPUT_BTN_SHORT);
        } else if (appState == STATE_GALLERY_TYPE || appState == STATE_GALLERY_PHOTOS ||
                   appState == STATE_GALLERY_VIDEOS || appState == STATE_DELETE_CONFIRM ||
                   appState == STATE_VIDEO_PLAYING) {
          // Emergency exit from any gallery/menu state → IDLE
          Serial.println("[GAL] Shutter short press → exit to IDLE");
          freeGalleryFiles();
          initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);
          if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
            tft.fillScreen(TFT_BLACK);
            xSemaphoreGive(spiMutex);
          }
          appState = STATE_IDLE;
        }
      }
      // Long press released — already handled below via held detection
    }

    // ── SHUTTER held long (>1s) → start recording ──
    // Works from IDLE or any gallery state (exits gallery first, reinits camera)
    {
      AppState cs = appState;
      bool isGallery = (cs == STATE_GALLERY_TYPE || cs == STATE_GALLERY_PHOTOS ||
                        cs == STATE_GALLERY_VIDEOS || cs == STATE_DELETE_CONFIRM ||
                        cs == STATE_VIDEO_PLAYING);
      if (shutterNow == LOW && (cs == STATE_IDLE || isGallery) &&
          (millis() - shutterPressTime > 1000) && shutterPressTime != 0) {
        // Exit gallery first if needed
        if (isGallery) {
          Serial.println("[GAL] Shutter long press → exit gallery, start recording");
          freeGalleryFiles();
          initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);
          if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
            tft.fillScreen(TFT_BLACK);
            xSemaphoreGive(spiMutex);
          }
          appState = STATE_IDLE;
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
          if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
            tft.setTextSize(2);
            tft.setCursor(10, 10);
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.print(errorMsg);
            xSemaphoreGive(spiMutex);
          }
          vTaskDelay(pdMS_TO_TICKS(1000));
          shutterPressTime = 0;
        }
      }
    }

    // ── BOOT press edge ──
    if (lastBoot == HIGH && bootNow == LOW) {
      bootPressTime = millis();
    }

    // ── BOOT release edge ──
    if (lastBoot == LOW && bootNow == HIGH) {
      unsigned long dur = millis() - bootPressTime;
      if (dur < 800) {
        if (appState == STATE_IDLE) {
          mirror = !mirror;
          sensor_t *s = esp_camera_sensor_get();
          if (s) s->set_hmirror(s, mirror);
          sendEvent(INPUT_BOOT_SHORT);
        }
      } else {
        // Long press: enter WiFi mode from IDLE, exit from WIFI
        if (appState == STATE_IDLE) {
          sendEvent(INPUT_BOOT_LONG);
          startWiFiMode();
        } else if (appState == STATE_WIFI_MODE) {
          sendEvent(INPUT_BOOT_LONG);
          stopWiFiMode();
        }
      }
    }

    // ── Process encoder events from ISR queue ──
    // Unified consumer: all encoder-based state transitions handled here.
    // Skipped during video playback — playVideoOnTFT() consumes events directly.
    if (appState != STATE_VIDEO_PLAYING) {
      InputEvent ev;
      while (xQueueReceive(inputEventQueue, &ev, 0) == pdTRUE) {
        if (ev.type != INPUT_ENC_CW && ev.type != INPUT_ENC_CCW &&
            ev.type != INPUT_ENC_CLICK && ev.type != INPUT_ENC_LONG) continue;

        switch (appState) {
          case STATE_IDLE:
            if (ev.type == INPUT_ENC_CLICK) {
              if (!globalSDState.isMounted) {
                // Temporarily block camera rendering so message stays visible
                appState = STATE_PHOTO;
                if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
                  tft.fillRect(0, 80, 320, 60, TFT_BLACK);
                  tft.setTextDatum(middle_center);
                  tft.setTextSize(2);
                  tft.setTextColor(TFT_RED);
                  tft.drawString("NO SD CARD!", tft.width() / 2, 110);
                  tft.setTextDatum(top_left);
                  xSemaphoreGive(spiMutex);
                }
                vTaskDelay(pdMS_TO_TICKS(1500));
                appState = STATE_IDLE;
                break;
              }
              galleryTypeSelection = 0;
              galleryNeedsRedraw = true;
              // Set state FIRST so taskCapture stops using the camera,
              // then delay to let any in-progress frame complete,
              // then deinit. Same pattern as startWiFiMode().
              appState = STATE_GALLERY_TYPE;
              vTaskDelay(pdMS_TO_TICKS(150));
              esp_camera_deinit();
              Serial.println("[GAL] Camera deinited for gallery");
              if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
                tft.fillScreen(TFT_BLACK);
                xSemaphoreGive(spiMutex);
              }
              if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
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
              initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);
              appState = STATE_IDLE;
              if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
                tft.fillScreen(TFT_BLACK);
                xSemaphoreGive(spiMutex);
              }
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

          default:
            break;
        }
      }
    }

    lastShutter = shutterNow;
    lastBoot    = bootNow;

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
// so Phase 5+ menu logic consumes all input from a single source.
#if ENC_AVAILABLE
void IRAM_ATTR encoderISR() {
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

void IRAM_ATTR encSwISR() {
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
#endif

// ================= TASK: CAMERA CAPTURE (Core 1, Priority 6) =================
void taskCapture(void *pvParameters) {
  int fail_count = 0;

  while (true) {
    // Skip capture when camera is deinited (WiFi mode, gallery states)
    {
      AppState cs = appState;
      if (cs == STATE_WIFI_MODE || cs == STATE_GALLERY_TYPE ||
          cs == STATE_GALLERY_PHOTOS || cs == STATE_GALLERY_VIDEOS ||
          cs == STATE_DELETE_CONFIRM || cs == STATE_VIDEO_PLAYING) {
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
        if (ws != dispRendering.load() && fb->len <= FRAME_BUF_SIZE && dispBuf[ws]) {
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

    // Don't render during photo/stopping/wifi — let notifications stay on screen
    if (appState == STATE_PHOTO || appState == STATE_STOPPING || appState == STATE_WIFI_MODE) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Gallery states — render on demand, skip camera preview
    if (appState == STATE_GALLERY_TYPE || appState == STATE_GALLERY_PHOTOS ||
        appState == STATE_GALLERY_VIDEOS || appState == STATE_DELETE_CONFIRM) {
      if (galleryNeedsRedraw) {
        galleryNeedsRedraw = false;
        switch (appState) {
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
      drawStatusBar(fps);
      frame_count = 0;
      last_fps_time = now;
    }
  }
}

// Draws the persistent bottom status bar (FPS + version + SD indicator).
// Called from taskDisplay whenever the 1-second FPS window elapses.
// Extracted to keep taskDisplay readable and to make version updates trivial.
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
void taskRecorder(void *pvParameters) {
  while (true) {
    if (appState != STATE_RECORDING && appState != STATE_STOPPING) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    RecFrame rf;
    if (xQueueReceive(recFrameQueue, &rf, pdMS_TO_TICKS(100)) == pdTRUE) {
      // Read exactly one frame's worth of audio from the I2S DMA ring buffer.
      // Because dma_frame_num=160 gives 320-byte DMA slots, AUDIO_BYTES_PER_FRAME
      // (3200) drains in exactly 10 slots — i.e. one slot per 10 ms of audio.
      // Zero-pad any shortage so every video frame gets a fixed-size audio chunk;
      // this keeps total audio sample count predictable and prevents AV drift.
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

      // portMAX_DELAY: never silently drop a frame. If SD is slow, backpressure
      // naturally propagates via recPoolFree semaphore to throttle capture.
      if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
        if (videoFile) {
          bool ok = writeAVIFrameFromBuf(videoFile, recPool[rf.idx], rf.len);
          // Always write audio chunk — consistent chunk per frame is required for
          // correct sync. Missing chunks cause cumulative drift; silence is better.
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
          { Preferences p; p.begin("cnt", false); p.putInt("pic", pictureNumber); p.end(); }
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
  p.end();
  Serial.println("[NVS] Camera settings saved.");
}

// Applies CameraSettings to the active sensor. Called after every initCamera.
void applySettings(sensor_t *s) {
  if (!s) return;
  s->set_brightness(s, camSettings.brightness);
  s->set_contrast(s, camSettings.contrast);
  s->set_saturation(s, camSettings.saturation);
  s->set_ae_level(s, camSettings.ae_level);
  s->set_wb_mode(s, camSettings.wb_mode);
  s->set_special_effect(s, camSettings.special_effect);
  s->set_hmirror(s, mirror);  // runtime mirror (toggled by BOOT button)
  s->set_vflip(s, camSettings.vflip);
}

// ================= WIFI FILE SERVER =================
// Embedded HTML — mobile-first dark theme file manager with photo thumbnails.
// Served from PROGMEM to avoid PSRAM/heap allocation for static content.
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Retro Cam</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',system-ui,sans-serif;background:#111;color:#e0e0e0;padding:16px;max-width:600px;margin:0 auto}
h1{font-size:1.4em;margin-bottom:12px;color:#fff}
.bar{background:#222;border-radius:8px;overflow:hidden;height:24px;margin-bottom:16px;position:relative}
.bar-fill{height:100%;background:linear-gradient(90deg,#2ecc71,#27ae60);transition:width .3s}
.bar-text{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);font-size:.8em;color:#fff;white-space:nowrap}
.file{display:flex;align-items:center;background:#1a1a1a;border-radius:8px;padding:10px 12px;margin-bottom:8px;border:1px solid #2a2a2a}
.thumb{width:80px;height:45px;border-radius:4px;object-fit:cover;flex-shrink:0;background:#222}
.vid-thumb{width:80px;height:45px;border-radius:4px;flex-shrink:0;background:#222;display:flex;align-items:center;justify-content:center;font-size:1.6em;color:#666}
.file-info{flex:1;min-width:0;margin-left:10px}
.file-name{font-size:.9em;word-break:break-all;color:#fff}
.file-size{font-size:.75em;color:#888;margin-top:2px}
.btns{display:flex;gap:6px;margin-left:8px;flex-shrink:0}
.btn{border:none;padding:8px 12px;border-radius:6px;cursor:pointer;font-size:.8em;font-weight:600;text-decoration:none;text-align:center}
.btn-dl{background:#2980b9;color:#fff}
.btn-del{background:#c0392b;color:#fff}
.btn:active{opacity:.7}
.empty{text-align:center;color:#666;padding:40px 0}
.err{text-align:center;color:#e74c3c;padding:30px 0;font-size:.9em}
.modal-bg{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.7);z-index:10;align-items:center;justify-content:center}
.modal-bg.show{display:flex}
.modal{background:#222;border-radius:12px;padding:24px;max-width:320px;width:90%;text-align:center}
.modal p{margin-bottom:16px;font-size:.95em}
.modal .btns{justify-content:center}
#status{text-align:center;color:#2ecc71;font-size:.85em;margin-bottom:12px;min-height:1.2em}
</style>
</head>
<body>
<h1>&#128247; Retro Cam Files</h1>
<div id="status"></div>
<div class="bar"><div class="bar-fill" id="barFill"></div><div class="bar-text" id="barText">Loading...</div></div>
<div id="list"><div class="empty">Loading...</div></div>
<div class="modal-bg" id="modalBg"><div class="modal"><p id="modalMsg">Delete?</p><div class="btns"><button class="btn btn-dl" onclick="closeModal()">Cancel</button><button class="btn btn-del" id="modalDel">Delete</button></div></div></div>
<script>
let delTarget='';
function load(){
 fetch('/api/info').then(r=>r.json()).then(d=>{
  if(d.error){document.getElementById('barText').textContent='SD card unavailable';return;}
  let pct=((d.used/d.total)*100).toFixed(1);
  document.getElementById('barFill').style.width=pct+'%';
  let fmt=b=>(b/1048576).toFixed(1)+' MB';
  document.getElementById('barText').textContent=fmt(d.used)+' / '+fmt(d.total)+' ('+fmt(d.free)+' free)';
 }).catch(()=>{document.getElementById('barText').textContent='Connection error';});
 fetch('/api/files').then(r=>r.json()).then(files=>{
  let el=document.getElementById('list');
  if(files.error){el.innerHTML='<div class="err">'+files.error+'</div>';return;}
  if(!files.length){el.innerHTML='<div class="empty">No files on SD card</div>';return;}
  let h='';
  files.forEach(f=>{
   let sz=f.size<1048576?(f.size/1024).toFixed(1)+' KB':(f.size/1048576).toFixed(1)+' MB';
   h+='<div class="file">';
   if(f.isVideo){h+='<div class="vid-thumb">&#127916;</div>';}
   else{h+='<img class="thumb" loading="lazy" src="/api/preview?file='+encodeURIComponent(f.name)+'" alt="">';}
   h+='<div class="file-info"><div class="file-name">'+f.name+'</div><div class="file-size">'+sz+'</div></div><div class="btns">';
   h+='<a class="btn btn-dl" download href="/api/download?file='+encodeURIComponent(f.name)+'">&#11015;</a>';
   h+='<button class="btn btn-del" onclick="confirmDel(\''+f.name.replace(/'/g,"\\'")+'\')">&#128465;</button>';
   h+='</div></div>';
  });
  el.innerHTML=h;
 }).catch(()=>{document.getElementById('list').innerHTML='<div class="err">Connection error</div>';});
}
function confirmDel(name){delTarget=name;document.getElementById('modalMsg').textContent='Delete '+name+'?';document.getElementById('modalBg').classList.add('show');}
function closeModal(){document.getElementById('modalBg').classList.remove('show');delTarget='';}
document.getElementById('modalDel').onclick=function(){
 if(!delTarget)return;
 let s=document.getElementById('status');
 s.textContent='Deleting...';s.style.color='#e67e22';
 fetch('/api/delete?file='+encodeURIComponent(delTarget)).then(r=>r.json()).then(d=>{
  closeModal();
  s.textContent=d.ok?'Deleted!':'Error: '+d.msg;
  s.style.color=d.ok?'#2ecc71':'#c0392b';
  setTimeout(()=>{s.textContent='';load();},1200);
 });
};
load();
</script>
</body>
</html>
)rawliteral";

// Start WiFi AP mode: deinit camera, start softAP + web server, show TFT info.
void startWiFiMode() {
  if (appState == STATE_WIFI_MODE) return;

  Serial.println("[WIFI] Entering WiFi mode...");

  // Tell other tasks to stop using camera & TFT immediately
  appState = STATE_WIFI_MODE;
  vTaskDelay(pdMS_TO_TICKS(150));

  // Deinit camera — frees PSRAM for WiFi stack
  esp_camera_deinit();

  // Start WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  delay(100);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[WIFI] AP started: %s / %s\n", WIFI_SSID, ip.toString().c_str());

  // Allocate and configure web server
  webServer = new AsyncWebServer(80);

  // Route: main page
  webServer->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", INDEX_HTML);
  });

  // Route: SD info JSON — validates SD mount before access
  webServer->on("/api/info", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!globalSDState.isMounted) {
      request->send(200, "application/json", "{\"error\":\"SD card not mounted\",\"total\":0,\"used\":0,\"free\":0}");
      return;
    }
    uint64_t total = 0, used = 0;
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      total = SD.totalBytes();
      used  = SD.usedBytes();
      xSemaphoreGive(spiMutex);
    }
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"total\":%llu,\"used\":%llu,\"free\":%llu}",
             total, used, total - used);
    request->send(200, "application/json", buf);
  });

  // Route: file list JSON — validates SD mount before scan
  webServer->on("/api/files", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!globalSDState.isMounted) {
      request->send(200, "application/json", "{\"error\":\"SD card not mounted\"}");
      return;
    }
    String json = "[";
    bool first = true;
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      File root = SD.open("/");
      if (root) {
        File f = root.openNextFile();
        while (f) {
          if (!f.isDirectory()) {
            String name = f.name();
            // Only list .jpg and .avi files
            if (name.endsWith(".jpg") || name.endsWith(".avi")) {
              if (!first) json += ",";
              first = false;
              json += "{\"name\":\"";
              json += name;
              json += "\",\"size\":";
              json += String((unsigned long)f.size());
              json += ",\"isVideo\":";
              json += name.endsWith(".avi") ? "true" : "false";
              json += "}";
            }
          }
          f = root.openNextFile();
        }
        root.close();
      }
      xSemaphoreGive(spiMutex);
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  // Route: download file (chunked stream, forced download via HTML download attr)
  // Uses shared_ptr<File> so the file is auto-closed if client disconnects mid-transfer.
  webServer->on("/api/download", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) {
      request->send(400, "text/plain", "Missing file param");
      return;
    }
    if (!globalSDState.isMounted) {
      request->send(503, "text/plain", "SD card not mounted");
      return;
    }
    String filename = request->getParam("file")->value();
    if (!filename.startsWith("/")) filename = "/" + filename;

    // Open file with mutex — shared_ptr ensures RAII cleanup
    auto fp = std::make_shared<File>();
    bool opened = false;
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      *fp = SD.open(filename, FILE_READ);
      opened = (bool)*fp;
      xSemaphoreGive(spiMutex);
    }
    if (!opened) {
      request->send(404, "text/plain", "File not found");
      return;
    }

    String contentType = filename.endsWith(".jpg") ? "image/jpeg" : "application/octet-stream";

    AsyncWebServerResponse *response = request->beginChunkedResponse(
      contentType.c_str(),
      [fp](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        if (!*fp) return 0;
        size_t bytesRead = 0;
        if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
          bytesRead = fp->read(buffer, maxLen);
          if (bytesRead == 0) {
            fp->close();
          }
          xSemaphoreGive(spiMutex);
        }
        return bytesRead;
      }
    );
    request->send(response);
  });

  // Route: preview file inline (for thumbnail <img> tags — no Content-Disposition)
  webServer->on("/api/preview", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) {
      request->send(400, "text/plain", "Missing file param");
      return;
    }
    if (!globalSDState.isMounted) {
      request->send(503, "text/plain", "SD not mounted");
      return;
    }
    String filename = request->getParam("file")->value();
    if (!filename.startsWith("/")) filename = "/" + filename;

    auto fp = std::make_shared<File>();
    bool opened = false;
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      *fp = SD.open(filename, FILE_READ);
      opened = (bool)*fp;
      xSemaphoreGive(spiMutex);
    }
    if (!opened) {
      request->send(404, "text/plain", "Not found");
      return;
    }

    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "image/jpeg",
      [fp](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        if (!*fp) return 0;
        size_t bytesRead = 0;
        if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
          bytesRead = fp->read(buffer, maxLen);
          if (bytesRead == 0) {
            fp->close();
          }
          xSemaphoreGive(spiMutex);
        }
        return bytesRead;
      }
    );
    // Cache thumbnails in browser for 5 min — avoid re-downloading on page reload
    response->addHeader("Cache-Control", "max-age=300");
    request->send(response);
  });

  // Route: delete file — validates SD mount
  webServer->on("/api/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) {
      request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Missing file param\"}");
      return;
    }
    if (!globalSDState.isMounted) {
      request->send(200, "application/json", "{\"ok\":false,\"msg\":\"SD card not mounted\"}");
      return;
    }
    String filename = request->getParam("file")->value();
    if (!filename.startsWith("/")) filename = "/" + filename;

    bool ok = false;
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      ok = SD.remove(filename);
      xSemaphoreGive(spiMutex);
    }
    if (ok) {
      request->send(200, "application/json", "{\"ok\":true}");
      Serial.printf("[WIFI] Deleted: %s\n", filename.c_str());
    } else {
      request->send(200, "application/json", "{\"ok\":false,\"msg\":\"Delete failed\"}");
    }
  });

  webServer->begin();
  Serial.println("[WIFI] Web server started on port 80.");

  // Draw info screen on TFT
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);
    tft.setTextColor(TFT_CYAN);
    tft.setTextSize(2);
    tft.drawString("WiFi Active", tft.width() / 2, 50);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.drawString("SSID:", tft.width() / 2, 95);
    tft.setTextColor(TFT_GREEN);
    tft.drawString(WIFI_SSID, tft.width() / 2, 120);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("IP:", tft.width() / 2, 155);
    tft.setTextColor(TFT_YELLOW);
    tft.drawString(ip.toString().c_str(), tft.width() / 2, 180);
    tft.setTextColor(0x7BEF);  // dim grey
    tft.setTextSize(1);
    tft.drawString("Press to exit", tft.width() / 2, 220);
    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }
}

// Stop WiFi mode: tear down server + AP, reinit camera, return to IDLE.
void stopWiFiMode() {
  if (appState != STATE_WIFI_MODE) return;

  Serial.println("[WIFI] Exiting WiFi mode...");

  if (webServer) {
    webServer->end();
    delete webServer;
    webServer = nullptr;
  }

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[WIFI] AP stopped.");

  // Reinit camera
  initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);

  // Clear TFT
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    xSemaphoreGive(spiMutex);
  }

  appState = STATE_IDLE;
  Serial.println("[WIFI] Back to IDLE.");
}

// ================= MICROPHONE =================
void initMicrophone() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // DMA geometry tuned for exact per-frame audio drain:
  //   dma_frame_num=160 → 160 × 2 bytes = 320 bytes per DMA slot (fills in 10 ms)
  //   AUDIO_BYTES_PER_FRAME (3200) / 320 = 10 slots → drains in exact multiples
  //   dma_desc_num=16   → 16 × 320 = 5120 bytes total buffer ≈ 160 ms capacity
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
  float real_fps = (float)avi_total_frames / (duration / 1000.0f);
  if (real_fps <= 0) real_fps = (float)TARGET_FPS;
  Serial.printf("[REC] Actual FPS: %.2f, frames: %d, audio chunks: %d\n",
                real_fps, avi_total_frames, avi_total_audio_chunks);

  // Use the declared TARGET_FPS for the AVI header rather than the measured average.
  // The measured average is skewed by the camera warm-up period and variable SD
  // write latency; a stable constant gives AVI players a reliable timing baseline.
  uint32_t microSecPerFrame = 1000000UL / TARGET_FPS;

  // Each audio chunk is exactly AUDIO_BYTES_PER_FRAME bytes (zero-padded when
  // needed), so total samples is exactly chunks × samples-per-chunk.
  uint32_t total_audio_samples = (uint32_t)avi_total_audio_chunks * AUDIO_SAMPLES_PER_FRAME;

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

  // step 2: capture exact file position right after idx1 — file.size() may return
  // stale data before FAT commits, which caused riffSize = 0xFFFFFFF8 in MediaInfo.
  uint32_t totalSize = (uint32_t)file.position();
  uint32_t riffSize  = totalSize - 8;

  file.seek(0);
  // microSecPerFrame is derived from TARGET_FPS (set in endAVI preamble), not real_fps.
  uint32_t microSecPerFrame_hdr = 1000000UL / TARGET_FPS;
  uint32_t maxBytesPerSec = (uint32_t)((width * height * 2) * TARGET_FPS) + AUDIO_SAMPLE_RATE * 2;

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

  file.write((uint8_t*)&microSecPerFrame_hdr, 4);
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
  uint32_t rate = TARGET_FPS;
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
  galleryIndex = 0;
  Serial.printf("[GAL] Found %d %s files\n", count, videosOnly ? "video" : "photo");
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
      if (fsize <= FRAME_BUF_SIZE && jpgBuf) {
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

// Display a single video item with play icon and file info.
void drawGalleryVideoItem() {
  if (galleryFileCount == 0 || galleryIndex >= galleryFileCount) return;

  const char *filePath = galleryFiles[galleryIndex];

  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);

    // Header: index / total
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    char header[32];
    snprintf(header, sizeof(header), "%d / %d", galleryIndex + 1, galleryFileCount);
    tft.drawString(header, tft.width() / 2, 10);

    // Play icon (triangle)
    int cx = tft.width() / 2;
    tft.fillTriangle(cx - 20, 60, cx - 20, 120, cx + 25, 90, TFT_CYAN);

    // Filename (strip leading /)
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE);
    const char *dispName = filePath;
    if (dispName[0] == '/') dispName++;
    tft.drawString(dispName, tft.width() / 2, 145);

    // File size
    float sizeMB = 0;
    File f = SD.open(filePath, FILE_READ);
    if (f) {
      sizeMB = f.size() / 1048576.0f;
      f.close();
    }
    char sizeStr[16];
    snprintf(sizeStr, sizeof(sizeStr), "%.1f MB", sizeMB);
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    tft.drawString(sizeStr, tft.width() / 2, 170);

    // Footer
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

  // Seek past AVI header — MOVI chunk data starts at offset avi_header_size (324)
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    aviFile.seek(avi_header_size);
    xSemaphoreGive(spiMutex);
  }

  uint8_t *frameBuf = dispBuf[0];  // reuse display buffer (camera copy paused)
  uint32_t frameTimeMs = 1000 / TARGET_FPS;  // 100ms per frame
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
        if (chunkSize > 0 && chunkSize <= FRAME_BUF_SIZE && frameBuf) {
          size_t rd = aviFile.read(frameBuf, chunkSize);
          if (rd == chunkSize) {
            tft.drawJpg(frameBuf, chunkSize, 40, 40, 240, 160, 0, 0, 0.5f);
            frameNum++;
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

    // Frame timing — maintain TARGET_FPS
    uint32_t elapsed = millis() - frameStart;
    if (elapsed < frameTimeMs) {
      vTaskDelay(pdMS_TO_TICKS(frameTimeMs - elapsed));
    }
  }

  // Close file and return to video list
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    aviFile.close();
    xSemaphoreGive(spiMutex);
  }

  Serial.printf("[GAL] Playback ended. Frames: %d\n", frameNum);
  appState = STATE_GALLERY_VIDEOS;
  galleryNeedsRedraw = true;
  if (taskDisplayHandle) xTaskNotifyGive(taskDisplayHandle);
}
