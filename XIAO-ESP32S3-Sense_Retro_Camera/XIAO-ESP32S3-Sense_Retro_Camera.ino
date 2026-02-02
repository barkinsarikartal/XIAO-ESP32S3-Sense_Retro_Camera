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
//     SCK           ->	       GPIO7
//  SDA / MOSI       ->	       GPIO9
//      CS           ->	       GPIO1
//      DC           ->	       GPIO2
//     RST           ->	       GPIO3
//     LED           ->	        3V3

// SHUTTER BUTTON    ->        GPIO4

// ================= EXTERNAL PINS =================
#define TFT_CS            1
#define TFT_DC            2
#define TFT_RST           3
#define TFT_SCK           7
#define TFT_MISO          8
#define TFT_MOSI          9
#define BTN_PIN           0  // boot button on XIAO (used for mirroring the image)
#define SD_CS_PIN         21 // SD card CS pin on XIAO ESP32S3 Sense
#define SHUTTER_BTN_PIN   4

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
#define EEPROM_SIZE 64
#define EEPROM_ADDR 0

// ================= STRUCTS & GLOBALS =================

struct SDStatus {
  bool isMounted;     // is the card inserted and mounted
  bool isFull;        // is the card full
  String errorMsg;    // error message, if any
};

SDStatus globalSDState = {false, false, "NO CARD"};
int pictureNumber = 0;
bool mirror = true;

SemaphoreHandle_t sdMutex;  // to prevent simultaneous access to the SD card
SemaphoreHandle_t tftMutex; // to prevent typing on the screen simultaneously

camera_config_t config;

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
  pinMode(SHUTTER_BTN_PIN, INPUT_PULLUP);

  sdMutex = xSemaphoreCreateMutex();
  tftMutex = xSemaphoreCreateMutex();

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, SD_CS_PIN);
  delay(100);

  // TFT
  tft.init();
  tft.setRotation(3);
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
  pictureNumber = EEPROM.readInt(EEPROM_ADDR);

  if (pictureNumber < 0 || pictureNumber > 10000) {
    pictureNumber = 0;
    EEPROM.writeInt(EEPROM_ADDR, pictureNumber);
    EEPROM.commit();
  }
  Serial.printf("Initial image no: %d\n", pictureNumber);

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
  
  initCamera(PIXFORMAT_RGB565, FRAMESIZE_QQVGA, 0);

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

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    
    if (!fb) {
      fail_count++;
      Serial.printf("Cam Fail: %d\n", fail_count);
      if (fail_count > 10) {
        if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
          tft.fillScreen(TFT_RED);
          tft.drawString("CAM RESET...", 10, 60);
          xSemaphoreGive(tftMutex);
        }
        delay(100);
        initCamera(PIXFORMAT_RGB565, FRAMESIZE_QQVGA, 0);
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

    // for taking a picture of that lovely view
    if (digitalRead(SHUTTER_BTN_PIN) == LOW) {
      
      bool canSave = false;
      String errMsg = "";
      
      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100))) {
        if (!globalSDState.isMounted) {
          errMsg = "NO SD CARD!";
        }
        else if (globalSDState.isFull) {
          errMsg = "SD FULL!";
        }
        else {
          canSave = true;
        }
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

      while(digitalRead(SHUTTER_BTN_PIN) == LOW) vTaskDelay(10);
      
      if (fb) esp_camera_fb_return(fb);
      continue; 
    }

    // printing to screen
    if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
      tft.startWrite();
      if (fb) {
        for (int y = 0; y < 112; y++) { // displaying 160x112 pixels. last 16 pixel row is for FPS indicator.
          tft.pushImage(0, y, 160, 1, (uint16_t *)(fb->buf + y * 160 * 2));
        }
      }
      tft.endWrite();
      xSemaphoreGive(tftMutex);
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

  initCamera(PIXFORMAT_JPEG, FRAMESIZE_HD, 12);
  
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
    tft.setCursor(40, 60);
    tft.print("CAM ERROR");
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
          EEPROM.writeInt(EEPROM_ADDR, pictureNumber);
          EEPROM.commit();

          if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
            tft.fillRoundRect(30, 50, 100, 30, 5, TFT_WHITE);
            tft.drawRoundRect(30, 50, 100, 30, 5, TFT_BLACK);
            tft.setTextColor(TFT_BLACK);
            tft.setCursor(45, 60);
            tft.print("SAVED #" + String(pictureNumber-1));
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

  Serial.println("--- Returning to Video Mode ---");
  initCamera(PIXFORMAT_RGB565, FRAMESIZE_QQVGA, 0);

  vTaskDelay(1000); 
}

void initCamera(pixformat_t format, framesize_t size, int jpeg_quality) {
  esp_camera_deinit(); 

  config.pixel_format = format;
  config.frame_size = size;
  config.jpeg_quality = jpeg_quality;
  config.fb_count = 2; 
  config.fb_location = CAMERA_FB_IN_PSRAM;
  
  if (format == PIXFORMAT_JPEG) {
      config.fb_count = 2; 
  }

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
}