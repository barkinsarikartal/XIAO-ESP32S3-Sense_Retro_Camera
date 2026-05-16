#include "shared.h"
#include <qrcode.h>

// ================= QR CODE WIFI PAIRING SCREEN =================
// Renders a split-screen layout on the TFT when entering WiFi mode:
//   Left half  — text credentials (SSID, password, IP) for PC/laptop users
//   Right half — QR code encoding a WIFI: URI for instant phone pairing
//
// The QR encodes: WIFI:S:Retro_Cam;T:WPA;P:barkinsarikartal;;
// iOS and Android recognise this format and offer a "Connect to WiFi" prompt.
//
// QR Version 3 = 29×29 modules. At 4 px/module = 116×116 px.
// A 1-module quiet zone (white border) is added for reliable scanning.

void drawWiFiQRScreen(IPAddress ip) {
  // Build the WiFi URI string
  // Format: WIFI:S:<ssid>;T:WPA;P:<password>;;
  static const char *wifiUri = "WIFI:S:" WIFI_SSID ";T:WPA;P:" WIFI_PASS ";;";

  // Generate QR code — stack-allocated, zero PSRAM impact
  QRCode qrcode;
  uint8_t qrcodeBytes[qrcode_getBufferSize(3)];
  qrcode_initText(&qrcode, qrcodeBytes, 3, ECC_LOW, wifiUri);

  // Layout constants (320×240 landscape)
  const int qrModulePx = 4;                             // pixels per QR module
  const int quietZone  = 1;                              // modules of white border
  const int qrFullSize = (qrcode.size + 2 * quietZone) * qrModulePx;  // 124 px
  const int qrX = 320 - qrFullSize - 8;                 // right-aligned with 8px margin
  const int qrY = (240 - qrFullSize) / 2;               // vertically centred

  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);

    // ── Left side: text credentials ──
    tft.setTextDatum(top_left);

    // Title
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN);
    tft.drawString("WiFi Active", 8, 10);

    // SSID
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);  // dim grey
    tft.drawString("SSID:", 8, 42);
    tft.setTextSize(2);
    tft.setTextColor(TFT_GREEN);
    tft.drawString(WIFI_SSID, 8, 55);

    // Password
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    tft.drawString("Password:", 8, 82);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(WIFI_PASS, 8, 96);

    // IP Address
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    tft.drawString("IP:", 8, 118);
    tft.setTextSize(2);
    tft.setTextColor(TFT_YELLOW);
    tft.drawString(ip.toString().c_str(), 8, 132);

    // Scan hint
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    tft.drawString("Scan QR to", 8, 168);
    tft.drawString("connect ->", 8, 180);

    // Footer
    tft.setTextDatum(middle_center);
    tft.setTextSize(1);
    tft.setTextColor(0x4208);  // very dim grey
    tft.drawString("Press to exit", tft.width() / 2, 225);

    // ── Right side: QR code ──
    // Draw white background (quiet zone included)
    tft.fillRect(qrX, qrY, qrFullSize, qrFullSize, TFT_WHITE);

    // Draw QR modules
    for (int y = 0; y < (int)qrcode.size; y++) {
      for (int x = 0; x < (int)qrcode.size; x++) {
        if (qrcode_getModule(&qrcode, x, y)) {
          int px = qrX + (x + quietZone) * qrModulePx;
          int py = qrY + (y + quietZone) * qrModulePx;
          tft.fillRect(px, py, qrModulePx, qrModulePx, TFT_BLACK);
        }
      }
    }

    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }
}
