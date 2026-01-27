#include <Arduino.h>
#include <LovyanGFX.hpp>
#include "esp_camera.h"
#include "esp_wifi.h"
#include "esp_bt.h"

//   TFT Pin         ->	   XIAO ESP32-S3
//     VCC           ->	        3V3
//     GND           ->	        GND
//     SCK           ->	       GPIO7
//  SDA / MOSI       ->	       GPIO9
//      CS           ->	       GPIO1
//      DC           ->	       GPIO2
//     RST           ->	       GPIO3
//     LED           ->	        3V3

// ================= EXTERNAL PINS =================
#define TFT_CS   1
#define TFT_DC   2
#define TFT_RST  3
#define TFT_SCK  7
#define TFT_MOSI 9
#define BTN_PIN  0 // boot button on XIAO (used for mirroring the image)

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
    bcfg.pin_miso = -1;
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

// ================= GLOBAL VALUES =================
uint32_t last_fps_time = 0;
uint32_t frame_count = 0;
float fps = 0;
bool mirror = true;
bool flip   = false;

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  esp_wifi_stop();
  esp_bt_controller_disable();

  pinMode(BTN_PIN, INPUT_PULLUP);

  // TFT
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);

  // CAMERA CONFIG
  camera_config_t config;
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
  config.pixel_format = PIXFORMAT_RGB565;

  config.frame_size = FRAMESIZE_QQVGA; // 160x120
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Couldn't initialize cam...");
    while (1);
  }

  Serial.println("Cam is ready.");

  tft.fillScreen(TFT_RED);
  delay(500);
  tft.fillScreen(TFT_GREEN);
  delay(500);
  tft.fillScreen(TFT_BLUE);
  delay(500);
  tft.fillScreen(TFT_BLACK);

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 1);        // -2 .. +2
    s->set_contrast(s, 0);          // -2 .. +2
    s->set_saturation(s, 2);        // -2 .. +2
    s->set_gainceiling(s, GAINCEILING_4X);

    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);           // auto

    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_ae_level(s, 0);
  }

}

// ================= LOOP =================
void loop() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;

  static bool lastBtn = HIGH;

  bool btnNow = digitalRead(BTN_PIN);
  if (lastBtn == HIGH && btnNow == LOW) {
    mirror = !mirror;

    sensor_t *s = esp_camera_sensor_get();
    s->set_hmirror(s, mirror);

    delay(200); // debounce
  }
  lastBtn = btnNow;

  
  //autoBrightness(fb); // set_exposure_ctrl(s, 1) works fine too

  uint32_t t0 = micros();

  tft.startWrite();
  for (int y = 0; y < 112; y++) { // displaying 160x112 pixels. last 16 pixel row is for FPS indicator.
    tft.pushImage(0, y, 160, 1, (uint16_t *)(fb->buf + y * 160 * 2));
  }
  tft.endWrite();

  uint32_t t1 = micros();
  Serial.println("micros: " + String(t1 - t0)); // how many microseconds passed for pushing the image to screen

  esp_camera_fb_return(fb);

  // FPS calculation
  frame_count++;
  uint32_t now = millis();
  if (now - last_fps_time >= 1000) {
    fps = frame_count * 1000.0 / (now - last_fps_time);
    Serial.printf("FPS: %.1f\n", fps);

    tft.fillRect(0, 112, 160, 16, TFT_BLACK);
    tft.setCursor(4, 116);
    tft.setTextColor(TFT_GREEN);
    tft.printf("FPS: %.1f", fps);

    frame_count = 0;
    last_fps_time = now;
  }
}

void autoBrightness(camera_fb_t *fb) {
  uint32_t sum = 0;
  uint32_t count = 0;

  uint16_t *p = (uint16_t *)fb->buf;

  for (int i = 0; i < fb->len / 2; i += 16) {
    uint16_t c = p[i];
    uint8_t r = (c >> 11) & 0x1F;
    uint8_t g = (c >> 5)  & 0x3F;
    uint8_t b =  c        & 0x1F;

    uint8_t y = (r * 2 + g * 4 + b) >> 3; // approximate luma
    sum += y;
    count++;
  }

  uint8_t avg = sum / count;
  sensor_t *s = esp_camera_sensor_get();

  if (avg < 60) {
    s->set_gainceiling(s, GAINCEILING_16X);
  }
  else if (avg < 100) {
    s->set_gainceiling(s, GAINCEILING_8X);
  }
  else {
    s->set_gainceiling(s, GAINCEILING_4X);
  }
}