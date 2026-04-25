#ifndef SHARED_H
#define SHARED_H

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

// ================= EXTERNAL PINS =================
// Boot button (GPIO 0) — disabled in Phase 10.
// Mirror toggle and WiFi entry are accessible via encoder menu.
#define SD_CS_PIN         21

#define SHUTTER_BTN_PIN_2 5

// LED flashlight: GPIO4 was previously used as a software GND for the shutter
// button (the perfboard had no spare GND pads). The shutter leg has been rewired
// to board GND, freeing GPIO4 for 2× white LEDs with 100Ω resistors (~6 mA).
#define LED_FLASHLIGHT_PIN 4

#define TFT_CS            1
#define TFT_DC            2
#define TFT_RST           3
#define TFT_SCK           7
#define TFT_MISO          8
#define TFT_MOSI          9

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

// ================= ENCODER PINS =================
#define ENC_CLK  6
#define ENC_DT   43
#define ENC_SW   44

// ================= CAMERA MODES =================
#define PHOTO_MODE        PIXFORMAT_JPEG
#define PHOTO_RESOLUTION  FRAMESIZE_HD
#define VIDEO_MODE        PIXFORMAT_JPEG
#define VIDEO_RESOLUTION  FRAMESIZE_HVGA
#define IDLE_MODE         PIXFORMAT_RGB565
#define IDLE_RESOLUTION   FRAMESIZE_QVGA
#define REC_JPEG_QUALITY  12
#define IDLE_JPEG_QUALITY 0

// ================= FIRMWARE VERSION =================
#define FIRMWARE_VERSION "v1.6"

// ================= WIFI AP CONFIG =================
#define WIFI_SSID "Retro_Cam"
#define WIFI_PASS "barkinsarikartal"

// ================= DEBUG FLAGS =================
// Set to 1 to enable Serial.printf for every InputEvent sent.
#define DEBUG_INPUT 0
// Set to 1 to add 8s boot delay for Serial Monitor attachment.
#define DEBUG_BOOT_DELAY 0

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
#define MAX_AVI_FRAMES  9000
#define MAX_AVI_CHUNKS  (MAX_AVI_FRAMES * 2)
#define AVI_HEADER_SIZE 324

// ================= BUFFER SIZES =================
// Display double buffer (ping-pong): decouples capture from TFT rendering.
// Separate sizes to reduce PSRAM usage: display needs 160KB (QVGA RGB565 = 150KB,
// HD JPEG gallery < 160KB), but recorder only needs 80KB (HVGA JPEG @ q12 ≈ 15-25KB).
#define DISP_BUF_SIZE      (160 * 1024)
#define REC_BUF_SIZE       (80 * 1024)
#define REC_POOL_SIZE       3

// ================= GALLERY =================
#define GALLERY_MAX_FILES  2000
#define GALLERY_NAME_LEN   24

// Grid gallery layout (320×240 landscape)
#define GRID_COLS           4
#define GRID_ROWS           3
#define GRID_ITEMS_PER_PAGE (GRID_COLS * GRID_ROWS)   // 12
#define THUMB_W            70
#define THUMB_H            52

// ================= MENU =================
#define MENU_MAIN_ITEMS 5
#define SETTINGS_COUNT  13

// ================= APP STATE & EVENTS =================
enum AppState {
  STATE_IDLE,
  STATE_RECORDING,
  STATE_PHOTO,
  STATE_STOPPING,
  STATE_MENU_MAIN,
  STATE_GALLERY_TYPE,
  STATE_GALLERY_PHOTOS,       // photo grid view (4×3 thumbnails)
  STATE_GALLERY_VIDEOS,       // video grid view (4×3 thumbnails)
  STATE_GALLERY_PHOTO_VIEW,   // single-photo detail view (from grid click)
  STATE_GALLERY_VIDEO_VIEW,   // single-video detail view (from grid click)
  STATE_VIDEO_PLAYING,
  STATE_DELETE_CONFIRM,
  STATE_WIFI_MODE,
  STATE_SETTINGS,
  STATE_TIMELAPSE,
};

#define EVT_START_RECORDING  (1 << 0)
#define EVT_STOP_RECORDING   (1 << 1)
#define EVT_TAKE_PHOTO       (1 << 2)
#define EVT_SD_STOP          (1 << 3)

// ================= INPUT EVENT QUEUE =================
// Queue-based input pipeline replaces direct EventGroup GPIO signals.
// Boot button events removed in Phase 10 — mirror toggle and WiFi entry
// are handled via the encoder-driven menu system.
enum InputEventType {
  INPUT_BTN_SHORT,   // shutter short press
  INPUT_BTN_LONG,    // shutter long press
  INPUT_ENC_CW,      // encoder clockwise
  INPUT_ENC_CCW,     // encoder counter-clockwise
  INPUT_ENC_CLICK,   // encoder button short tap
  INPUT_ENC_LONG,    // encoder button long press (>500 ms)
};
struct InputEvent { InputEventType type; };

// ================= STRUCTS =================
struct SDStatus {
  bool isMounted;
  bool isFull;
  String errorMsg;
};

struct CameraSettings {
  int brightness;          // -2..+2   (default: 1)
  int contrast;            // -2..+2   (default: 0)
  int saturation;          // -2..+2   (default: 2)
  int ae_level;            // -2..+2   (default: 0)
  int wb_mode;             // 0=Auto 1=Sunny 2=Cloudy 3=Office 4=Home
  int special_effect;      // 0=None 1=Neg 2=Gray 3=Red 4=Green 5=Blue 6=Sepia
  int hmirror;             // 0/1      (default: 1)
  int vflip;               // 0/1      (default: 0)
  int jpeg_quality;        // 4=Max..63=Low, inverse scale (default: 12)
  int timelapse_interval;  // seconds between captures (default: 10)
  int rec_max_seconds;     // 0=unlimited, >0=auto-stop after N seconds (default: 0)
  int flashlight_on;       // 0=off, 1=on (default: 0) — GPIO4 LED output
};

typedef struct { int idx; size_t len; } RecFrame;

// ================= TFT CLASS =================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
public:
  LGFX() {
    auto bcfg = _bus.config();
    bcfg.spi_host = SPI2_HOST;
    bcfg.spi_mode = 0;
    bcfg.freq_write = 80000000;  // 80 MHz
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

// ================= EXTERN GLOBALS — main.cpp =================
extern LGFX tft;
extern SemaphoreHandle_t spiMutex;
extern volatile AppState appState;
extern EventGroupHandle_t appEvents;
extern QueueHandle_t inputEventQueue;
extern SDStatus globalSDState;
extern CameraSettings camSettings;
extern camera_config_t config;
extern int pictureNumber;
extern int videoNumber;
extern unsigned long recordingStartTime;
extern File videoFile;
extern TaskHandle_t taskDisplayHandle;

// Display double buffer
extern uint8_t *dispBuf[2];
extern volatile size_t dispLen[2];
extern std::atomic<int> dispWriteSlot;
extern std::atomic<int> dispReadSlot;
extern std::atomic<int> dispRendering;

// Recorder pool
extern uint8_t *recPool[REC_POOL_SIZE];
extern QueueHandle_t recFrameQueue;
extern SemaphoreHandle_t recPoolFree;
extern int recPoolWriteIdx;

// Audio
extern int16_t *audioRecBuf;
extern i2s_chan_handle_t i2s_rx_handle;

// Gallery state
extern char **galleryFiles;
extern int galleryFileCount;
extern int galleryIndex;
extern int galleryTypeSelection;
extern int deleteSelection;
extern volatile bool galleryNeedsRedraw;
extern int gridPage;           // current grid page (0-indexed)

// Menu state
extern int menuMainSelection;
extern int settingsIndex;
extern bool settingsEditing;
extern int resetCounterConfirm;  // 0=No, 1=Yes — for counter reset confirmation

// Encoder ISR state
extern volatile uint8_t encPrevState;
extern volatile int8_t  encAccum;
extern volatile uint32_t encSwPressMs;

// ================= EXTERN GLOBALS — avi_writer.cpp =================
extern long avi_movi_size;
extern unsigned long avi_start_time;
extern int avi_total_frames;
extern uint32_t *avi_frame_sizes;
extern uint32_t *avi_audio_sizes;
extern int avi_total_audio_chunks;
extern uint8_t *avi_chunk_order;
extern int avi_total_chunks;

// ================= EXTERN GLOBALS — wifi_server.cpp =================
extern AsyncWebServer *webServer;

// ================= FUNCTION PROTOTYPES — main.cpp =================
void initCamera(pixformat_t format, framesize_t size, int jpeg_quality);
void initMicrophone();
void deinitMicrophone();
void allocateBuffers();
void enterMenuMain();
void exitMenuToIdle();
void emergencyExitToIdle();
void showTFTMessage(const char *msg, uint16_t color, int durationMs = 0);

// ================= FUNCTION PROTOTYPES — avi_writer.cpp =================
void startAVI(File &file, int fps, int width, int height);
bool writeAVIFrameFromBuf(File &file, uint8_t *buf, size_t len);
bool writeAVIAudioChunk(File &file, uint8_t *buf, size_t bytes);
void endAVI(File &file, int fps, int width, int height);

// ================= FUNCTION PROTOTYPES — wifi_server.cpp =================
void startWiFiMode();
void stopWiFiMode();

// ================= FUNCTION PROTOTYPES — gallery.cpp =================
void scanGalleryFiles(bool videosOnly);
void freeGalleryFiles();
void drawGalleryTypeMenu();
void drawPhotoGrid();
void drawVideoGrid();
void drawGalleryPhotoView();
void drawGalleryVideoView();
size_t extractAVIFrame(File &aviFile, uint8_t *buf, size_t bufSize, int targetFrame);
void drawDeleteConfirm();
void playVideoOnTFT();
void drawGridCursor(int col, int row, uint16_t color);
void clearGridCursor(int col, int row);

// ================= FUNCTION PROTOTYPES — settings.cpp =================
void loadCameraSettings();
void saveCameraSettings();
void applySettings(sensor_t *s);
void drawSettingsMenu();
void drawMenuMain();

// ================= FUNCTION PROTOTYPES — exif_writer.cpp =================
void buildExifBlock();
size_t getExifBlock(const uint8_t **out);

#endif // SHARED_H
