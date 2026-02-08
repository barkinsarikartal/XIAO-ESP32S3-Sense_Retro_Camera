#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <EEPROM.h>
#include "esp_camera.h"
#include "esp_wifi.h"
#include "esp_bt.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "img_converters.h" // RGB565 -> JPG
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

//   TFT Pin         ->	   XIAO ESP32-S3
//     VCC           ->	        3V3
//     GND           ->	        GND
//     CS            ->	       GPIO1
//    RESET          ->	       GPIO3
//     A0            ->	       GPIO2
//     SDA           ->	       GPIO9
//     SCK           ->	       GPIO7
//     LED           ->	        3V3

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
#define IDLE_RESOLUTION   FRAMESIZE_QQVGA

#define REC_JPEG_QUALITY  12
#define IDLE_JPEG_QUALITY 0

// ================= STRUCTS & GLOBALS =================
struct SDStatus {
  bool isMounted;     // is the card inserted and mounted
  bool isFull;        // is the card full
  String errorMsg;    // error message, if any
};

SDStatus globalSDState = {false, false, "NO CARD"};
int pictureNumber = 0;
int videoNumber = 0;
bool mirror = true;

bool isRecording = false;
unsigned long recordingStartTime = 0;
File videoFile;

SemaphoreHandle_t sdMutex;  // to prevent simultaneous access to the SD card
SemaphoreHandle_t tftMutex; // to prevent typing on the screen simultaneously

camera_config_t config;

// ================= AVI HEADER VARIABLES =================
const int avi_header_size = 224; 
long avi_movi_size = 0;
long avi_index_size = 0;
unsigned long avi_start_time = 0;
int avi_total_frames = 0;
uint32_t *avi_frame_sizes = nullptr;  // allocated in PSRAM (not internal RAM)
int avi_frame_capacity = 0;

// ================= TFT CLASS =================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7735S _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX() {
    auto bcfg = _bus.config();
    bcfg.spi_host = SPI2_HOST;
    bcfg.spi_mode = 0;
    bcfg.freq_write = 40000000; // 40 MHz
    bcfg.pin_sclk = TFT_SCK;
    bcfg.pin_mosi = TFT_MOSI;
    bcfg.pin_miso = TFT_MISO;
    bcfg.pin_dc   = TFT_DC;
    _bus.config(bcfg);
    _panel.setBus(&_bus);

    auto pcfg = _panel.config();
    pcfg.pin_cs = TFT_CS;
    pcfg.pin_rst = TFT_RST;
    pcfg.panel_width  = 128;
    pcfg.panel_height = 160;
    pcfg.offset_x = 0;
    pcfg.offset_y = 0;
    pcfg.invert = false;
    pcfg.rgb_order = true;
    pcfg.readable = false;

    _panel.config(pcfg);

    setPanel(&_panel);
  }
};

LGFX tft;

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

  sdMutex = xSemaphoreCreateMutex();
  tftMutex = xSemaphoreCreateMutex();

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, SD_CS_PIN);
  delay(100);

  // TFT
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(middle_center);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.drawString("GETTING READY", tft.width() / 2, 40);
  tft.setTextSize(2);
  tft.drawString("PLEASE WAIT..", tft.width() / 2, 70);
  tft.setTextSize(1);
  tft.drawString("github@barkinsarikartal", tft.width() / 2, 110); // mini ad here :)
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

  delay(3000);

  // Task 1: SD Card Monitor (Core: 0)
  // Stack size: 4096 bytes, Priority: 1 (Low)
  xTaskCreatePinnedToCore(
    taskSDMonitor,  
    "SD_Monitor",   
    4096,           
    NULL,           
    1,              
    NULL,           
    0               
  );

  // Task 2: Camera ve UI (Core 1)
  // Stack size: 8192 bytes, Priority: 5 (High)
  xTaskCreatePinnedToCore(
    taskCamera,     
    "Camera_UI",    
    8192,           
    NULL,           
    5,              
    NULL,           
    1               
  );

  Serial.println("Cam is ready.");
}

// ================= LOOP =================
void loop() {
  // loop is no longer in use; tasks are now active
  vTaskDelete(NULL);
}

// ================= TASK 1: SD MONITOR =================
void taskSDMonitor(void *pvParameters) {
  while (true) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {

      // do not occupy the SD card during recording
      if(isRecording) {
        xSemaphoreGive(sdMutex);
        vTaskDelay(2000);
        continue;
      }
      
      bool currentMount = false;
      bool currentFull = false;
      String msg = "";

      if (SD.cardType() == CARD_NONE || SD.totalBytes() == 0) {
        // if the card is missing or disconnected; try restarting.
        SD.end();
        if (!SD.begin(SD_CS_PIN, SPI, 4000000)) {
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

      // if installed, check for fullness.
      if (currentMount) {
        uint64_t total = SD.totalBytes();
        uint64_t used = SD.usedBytes();
        uint64_t free = total - used;
        
        // if less than 200KB of space remains, consider it full.
        if (free < 200 * 1024) { 
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

      xSemaphoreGive(sdMutex);
    } 
    
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ================= TASK 2: CAMERA & UI =================
void taskCamera(void *pvParameters) {
  uint32_t last_fps_time = 0;
  uint32_t frame_count = 0;
  float fps = 0;
  static int fail_count = 0;

  static bool lastBtn = HIGH;
  static bool lastShutterState = HIGH; 
  unsigned long shutterPressTime = 0;

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    
    if (!fb) {
      fail_count++;
      Serial.printf("Cam Fail: %d\n", fail_count);
      if (fail_count > 10) {
        if (isRecording) {
          stopVideoRecording(); 
          if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
            tft.setTextColor(TFT_ORANGE, TFT_BLACK);
            tft.drawString("REC SAVED (ERR)", 10, 80);
            xSemaphoreGive(tftMutex);
          }
          delay(200);
        }

        if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
          tft.fillScreen(TFT_RED);
          tft.drawString("CAM RESET...", 10, 60);
          xSemaphoreGive(tftMutex);
        }
        delay(100);
        if(isRecording) initCamera(VIDEO_MODE, VIDEO_RESOLUTION, REC_JPEG_QUALITY);
        else initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);
        fail_count = 0;
      }
      vTaskDelay(10); 
      continue;
    }
    fail_count = 0;

    // for mirroring the image
    bool btnNow = digitalRead(BTN_PIN);
    if (lastBtn == HIGH && btnNow == LOW) {
      mirror = !mirror;
      sensor_t *s = esp_camera_sensor_get();
      s->set_hmirror(s, mirror);
      vTaskDelay(200);
    }
    lastBtn = btnNow;

    int shutterState = digitalRead(SHUTTER_BTN_PIN_2);
    // capture the moment it's pressed
    if (lastShutterState == HIGH && shutterState == LOW) {
      shutterPressTime = millis();
    }

    // capture the moment it's released
    else if (lastShutterState == LOW && shutterState == HIGH) {
      unsigned long duration = millis() - shutterPressTime;
      
      // short press (< 1 sec) and if not recording -> take a photo
      if (!isRecording && duration < 1000) {
        
        bool canSave = false;
        String errMsg = "";
        
        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100))) {
          if (!globalSDState.isMounted) errMsg = "NO SD CARD!";
          else if (globalSDState.isFull) errMsg = "SD FULL!";
          else canSave = true;
          xSemaphoreGive(sdMutex);
        }

        if (!canSave) {
          if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
            tft.setCursor(0, 0);
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.print(errMsg);
            xSemaphoreGive(tftMutex);
          }
          vTaskDelay(1000);
        }
        else {
          esp_camera_fb_return(fb);
          savePhotoHighRes();
          fb = NULL;
        }
      }
      
      // if it's recording and enough time has passed -> stop recording
      // (ignore any stops within the first 2 seconds after recording starts)
      else if (isRecording) {
        if (millis() - recordingStartTime > 2000) {
          stopVideoRecording();
          esp_camera_fb_return(fb);
          fb = NULL;
          initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY); // back to normal mode
        }
      }
    }

    // long press (> 1 sec) -> record video
    if (shutterState == LOW && !isRecording && (millis() - shutterPressTime > 1000)) {
       
      esp_camera_fb_return(fb); 
      fb = NULL;
      
      bool cardReady = false;
      String errorMsg = "";
      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100))) {
        if (!globalSDState.isMounted) errorMsg = "NO SD CARD!";
        else if (globalSDState.isFull) errorMsg = "SD FULL!";
        else cardReady = true;
        xSemaphoreGive(sdMutex);
      }

      if(cardReady) {
        if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
          tft.fillScreen(TFT_BLACK);
          xSemaphoreGive(tftMutex);
        }
        initCamera(VIDEO_MODE, VIDEO_RESOLUTION, REC_JPEG_QUALITY);
        startVideoRecording(); 
        recordingStartTime = millis();
      }
      else {
        if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
          tft.setCursor(0, 0);
          tft.setTextColor(TFT_RED, TFT_BLACK);
          tft.print(errorMsg);
          xSemaphoreGive(tftMutex);
        }
        vTaskDelay(1000);
      }
    }
    
    lastShutterState = shutterState;

    if (!fb) continue;

    // printing to screen
    if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
      tft.startWrite();
      if (fb) {
        if (isRecording) {
          // printing video visuals to the screen
          // this is not working good right now. going to try to fix this later
          tft.drawJpg(fb->buf, fb->len, 20, 16, 0, 0, 0, 0, JPEG_DIV_4);

          // recording icon (blinking)
          if ((millis() / 500) % 2 == 0) tft.fillCircle(10, 10, 4, TFT_RED);
          else tft.fillCircle(10, 10, 4, TFT_BLACK);

          // displaying recording duration (hh:mm:ss)
          unsigned long elapsed = (millis() - recordingStartTime) / 1000;
          int hours = elapsed / 3600;
          int minutes = (elapsed % 3600) / 60;
          int seconds = elapsed % 60;
          char timeStr[12];
          sprintf(timeStr, "%02d:%02d:%02d", hours, minutes, seconds);
          tft.setTextColor(TFT_WHITE, TFT_BLACK);
          tft.setCursor(20, 4);
          tft.print(timeStr);
        }
        else {
          // printing what the cam sees to the screen when no photos or videos are being taken
          for (int y = 0; y < 112; y++) { // displaying 160x112 pixels. last 16 pixel row is for FPS indicator.
            tft.pushImage(0, y, 160, 1, (uint16_t *)(fb->buf + y * 160 * 2));
          }
        }
      }
      tft.endWrite();
      xSemaphoreGive(tftMutex);
    }

    // video recording
    if (isRecording && fb->format == PIXFORMAT_JPEG) {
      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(50))) {
        if (videoFile) {
          writeAVIFrame(videoFile, fb);
        }
        xSemaphoreGive(sdMutex);
      }
    }

    if (fb) esp_camera_fb_return(fb);

    // FPS calculation
    frame_count++;
    uint32_t now = millis();
    if (now - last_fps_time >= 1000) {
      fps = frame_count * 1000.0 / (now - last_fps_time);
      
      if (xSemaphoreTake(tftMutex, 10)) {
        tft.fillRect(0, 112, 160, 16, TFT_BLACK);
        
        tft.setCursor(4, 116);
        tft.setTextColor(TFT_GREEN);
        tft.printf("FPS: %.1f", fps);

        if (globalSDState.isMounted && !globalSDState.isFull) {
          tft.fillCircle(150, 120, 3, TFT_GREEN);
        }
        else {
          tft.fillCircle(150, 120, 3, TFT_RED);
        }

        xSemaphoreGive(tftMutex);
      }

      frame_count = 0;
      last_fps_time = now;
    }
    
    vTaskDelay(1); 
  }
}

void savePhotoHighRes() {
  if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
    digitalWrite(TFT_CS, HIGH);
    tft.fillRoundRect(30, 50, 100, 30, 5, TFT_BLUE);
    tft.drawRoundRect(30, 50, 100, 30, 5, TFT_WHITE);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(40, 60);
    tft.print("HOLD ON...");
    xSemaphoreGive(tftMutex);
  }

  Serial.println("--- HD Mode Start ---");

  initCamera(PHOTO_MODE, PHOTO_RESOLUTION, REC_JPEG_QUALITY);
  
  // warm up
  camera_fb_t *fb = NULL;
  for(int i = 0; i < 15; i++) {
    fb = esp_camera_fb_get();
    if(fb) esp_camera_fb_return(fb);
    vTaskDelay(20); 
  }

  fb = esp_camera_fb_get();
  if(!fb) {
    Serial.println("Cam error!");
    if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
      tft.setCursor(40, 60);
      tft.print("CAM ERROR");
      xSemaphoreGive(tftMutex);
    }
  }
  else {
    if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
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

          if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
            tft.fillRoundRect(30, 50, 100, 30, 5, TFT_WHITE);
            tft.drawRoundRect(30, 50, 100, 30, 5, TFT_BLACK);
            tft.setTextColor(TFT_BLACK);
            tft.setCursor(40, 60);
            tft.print("PIC SAVED #" + String(pictureNumber-1));
            xSemaphoreGive(tftMutex);
          }
        }
        else {
          Serial.println("File Open Error");
          if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
            tft.setCursor(0, 0);
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.print("WRITE ERROR");
            xSemaphoreGive(tftMutex);
          }
        }
      }
      else {
        Serial.println("SD Lost during capture");
        if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
          tft.setCursor(0, 0);
          tft.setTextColor(TFT_RED, TFT_BLACK);
          tft.print("SD CONNECTION LOST");
          xSemaphoreGive(tftMutex);
        }
      }
      
      xSemaphoreGive(sdMutex);
    }
    esp_camera_fb_return(fb);
  }

  Serial.println("--- Returning to Idle Mode ---");
  initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);

  vTaskDelay(1000); 
}

void initCamera(pixformat_t format, framesize_t size, int jpeg_quality) {
  esp_camera_deinit(); 

  config.pixel_format = format;
  config.frame_size = size;
  config.jpeg_quality = jpeg_quality;
  config.fb_count = 2; 
  config.fb_location = CAMERA_FB_IN_PSRAM;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Cam init error");
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    // these settings need to be re-entered after each reset
    s->set_brightness(s, 1);        
    s->set_contrast(s, 0);          
    s->set_saturation(s, 2); 
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_ae_level(s, 0);
    
    // we're slightly increasing the gain ceiling at high resolution so it doesn't stay dark
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

void startVideoRecording() {
  if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
    String path = "/vid_" + String(videoNumber) + ".avi"; 
    Serial.printf("Video Start: %s\n", path.c_str());
    
    videoFile = SD.open(path.c_str(), FILE_WRITE);
    if (videoFile) {
      isRecording = true;
      videoNumber++;
      EEPROM.writeInt(EEPROM_ADDR_VID, videoNumber);
      EEPROM.commit();
      
      startAVI(videoFile, 10, 240, 176); 
    }
    else {
      Serial.println("Video file create failed");
    }
    xSemaphoreGive(sdMutex);
  }
}

void stopVideoRecording() {
  if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
    if (videoFile) {
      endAVI(videoFile, 10, 240, 176); 
      Serial.println("Video Saved.");
      
      if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
        tft.fillRoundRect(30, 50, 100, 30, 5, TFT_WHITE);
        tft.drawRoundRect(30, 50, 100, 30, 5, TFT_BLACK);
        tft.setTextColor(TFT_BLACK);
        tft.setCursor(40, 60);
        tft.print("VIDEO SAVED #" + String(videoNumber-1));

        // tft.fillScreen(TFT_GREEN);
        // tft.setTextColor(TFT_BLACK);
        // tft.setCursor(30, 60);
        // tft.print("VIDEO SAVED #" + String(videoNumber-1));
        xSemaphoreGive(tftMutex);
      }
      delay(1000);
    }
    isRecording = false;
    xSemaphoreGive(sdMutex);
  }
}

void startAVI(File &file, int fps, int width, int height) {
  avi_movi_size = 0;
  avi_total_frames = 0;
  avi_start_time = millis();
  
  // Allocate frame size array in PSRAM
  if (avi_frame_sizes) {
    free(avi_frame_sizes);
    avi_frame_sizes = nullptr;
  }
  avi_frame_capacity = 36000;  // 1 hour at 10fps
  avi_frame_sizes = (uint32_t*)ps_malloc(avi_frame_capacity * sizeof(uint32_t));
  
  uint8_t zero_buf[avi_header_size];
  memset(zero_buf, 0, avi_header_size);
  file.write(zero_buf, avi_header_size);
}

void writeAVIFrame(File &file, camera_fb_t *fb) {
  uint8_t dc_buf[4] = {0x30, 0x30, 0x64, 0x63}; 
  file.write(dc_buf, 4);

  uint32_t len = fb->len;
  uint32_t rem = len % 4;
  uint32_t padding = (rem == 0) ? 0 : 4 - rem;
  uint32_t totalLen = len + padding;

  file.write((uint8_t*)&totalLen, 4);
  file.write(fb->buf, fb->len);
  
  if (padding > 0) {
    uint8_t pad[3] = {0,0,0};
    file.write(pad, padding);
  }

  avi_movi_size += (totalLen + 8);
  if (avi_frame_sizes && avi_total_frames < avi_frame_capacity) {
    avi_frame_sizes[avi_total_frames] = totalLen;
  }
  avi_total_frames++;
}

void endAVI(File &file, int fps, int width, int height) {
  unsigned long duration = millis() - avi_start_time;
  float real_fps = (float)avi_total_frames / (duration / 1000.0);
  if (real_fps <= 0) real_fps = (float)fps;

  //  step 1: write idx1 at end of file
  file.seek(0, SeekEnd);
  file.write((const uint8_t*)"idx1", 4);
  uint32_t idx1Size = avi_total_frames * 16;
  file.write((uint8_t*)&idx1Size, 4);
  
  uint32_t frameOffset = 4;  // offset from "movi" tag
  for (int i = 0; i < avi_total_frames; i++) {
    uint32_t frameSize = (avi_frame_sizes && i < avi_frame_capacity) ? avi_frame_sizes[i] : 0;
    file.write((const uint8_t*)"00dc", 4);
    uint32_t kf = 0x10;  // AVIIF_KEYFRAME
    file.write((uint8_t*)&kf, 4);
    file.write((uint8_t*)&frameOffset, 4);
    file.write((uint8_t*)&frameSize, 4);
    frameOffset += frameSize + 8;
  }
  
  // step 2: rewrite header at position 0
  file.seek(0);
  
  uint32_t totalSize = file.size();
  uint32_t riffSize = totalSize - 8;
  uint32_t microSecPerFrame = (uint32_t)(1000000.0 / real_fps);
  uint32_t maxBytesPerSec = (width * height * 2) * real_fps; 

  file.write((const uint8_t*)"RIFF", 4);
  file.write((uint8_t*)&riffSize, 4);
  file.write((const uint8_t*)"AVI ", 4);

  file.write((const uint8_t*)"LIST", 4);
  uint32_t hdrlSize = 192; 
  file.write((uint8_t*)&hdrlSize, 4);
  file.write((const uint8_t*)"hdrl", 4);

  file.write((const uint8_t*)"avih", 4);
  uint32_t avihSize = 56;
  file.write((uint8_t*)&avihSize, 4);
  
  file.write((uint8_t*)&microSecPerFrame, 4);
  uint32_t maxTransferRate = maxBytesPerSec;
  file.write((uint8_t*)&maxTransferRate, 4); 
  uint32_t padding = 0;
  file.write((uint8_t*)&padding, 4); 
  uint32_t avih_flags = 0x10;  // AVIF_HASINDEX
  file.write((uint8_t*)&avih_flags, 4);
  file.write((uint8_t*)&avi_total_frames, 4);
  file.write((uint8_t*)&padding, 4); 
  uint32_t streams = 1;
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
  if(rate == 0) rate = 10;
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

  file.write((const uint8_t*)"LIST", 4);
  uint32_t moviListSize = 4 + avi_movi_size;
  file.write((uint8_t*)&moviListSize, 4);
  file.write((const uint8_t*)"movi", 4);

  if (avi_frame_sizes) {
    free(avi_frame_sizes);
    avi_frame_sizes = nullptr;
  }
  avi_frame_capacity = 0;

  file.close();
}