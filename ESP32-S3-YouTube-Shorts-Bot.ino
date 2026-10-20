#include <Arduino.h>
#include "USB.h"
#include "USBHID.h"
#include <Adafruit_NeoPixel.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// ============================================================
// ESP32-S3 YouTube Shorts Auto Viewer + ST7789 dashboard
// Screen: ST7789 170x320 SPI
//
// IMPORTANT:
// - This is a visual dashboard for your existing USB HID demo.
// - The screen shows the actions this sketch sends; it cannot
//   verify what YouTube actually played or whether a like registered.
// - TFT pins below are provisional. Change them to match your wiring.
// - Arduino IDE: select ESP32S3 Dev Module and USB Mode: USB-OTG (TinyUSB).
//   USBHID is unavailable if the selected USB mode/core build disables TinyUSB HID.
// ============================================================

// ---------- TFT wiring (provisional) ----------
#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST  14
#define TFT_BL   -1   // Set to a GPIO number if your BL pin is wired to GPIO.
// If BL is wired directly to 3.3V, leave TFT_BL as -1.

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ---------- Onboard RGB LED ----------
#define RGB_LED_PIN 48
Adafruit_NeoPixel RGB(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

// ---------- USB HID ----------
USBHID HID;

static const uint8_t touch_report_descriptor[] = {
  0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01,
  0x09, 0x22, 0xA1, 0x02,
  0x09, 0x42, 0x09, 0x32, 0x15, 0x00, 0x25, 0x01,
  0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
  0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
  0x09, 0x51, 0x15, 0x00, 0x25, 0x0F, 0x75, 0x04,
  0x95, 0x01, 0x81, 0x02,
  0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
  0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
  0x16, 0x00, 0x00, 0x26, 0xFF, 0x0F,
  0x75, 0x0C, 0x95, 0x02, 0x81, 0x02,
  0xC0,
  0x05, 0x0D, 0x09, 0x54, 0x15, 0x00, 0x25, 0x01,
  0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
  0xC0
};

class TouchHID : public USBHIDDevice {
public:
  TouchHID() { HID.addDevice(this, sizeof(touch_report_descriptor)); }
  uint16_t _onGetDescriptor(uint8_t *buffer) override {
    memcpy(buffer, touch_report_descriptor, sizeof(touch_report_descriptor));
    return sizeof(touch_report_descriptor);
  }
  bool sendTouch(bool down, uint16_t x, uint16_t y) {
    uint8_t report[6] = {0};
    report[0] = down ? 0x03 : 0x00;
    report[1] = 0;
    report[2] = x & 0xFF;
    report[3] = ((x >> 8) & 0x0F) | ((y & 0x0F) << 4);
    report[4] = (y >> 4) & 0xFF;
    report[5] = down ? 1 : 0;
    return HID.SendReport(0, report, sizeof(report));
  }
};
TouchHID Touch;

// ---------- Behaviour settings ----------
const int START_DELAY_MS = 5000;
const int MIN_WATCH_MS = 2000;
const int MAX_WATCH_MS = 9000;
const int LIKE_PERCENT = 30;
const int MIN_AFTER_LIKE_MS = 1200;
const int MAX_AFTER_LIKE_MS = 2500;
// No fixed short limit: the bot runs until the board is reset or power is removed.

// ---------- UI state ----------
enum UiMode { MODE_BOOT, MODE_WAITING, MODE_WATCHING, MODE_LIKING, MODE_SWIPING, MODE_DONE };
UiMode uiMode = MODE_BOOT;
int shortsDone = 0;
int likesSent = 0;
int swipesSent = 0;
int currentWatchMs = 0;
unsigned long modeStarted = 0;
unsigned long lastAnim = 0;
int animFrame = 0;
bool usbReady = false;

uint16_t C_BG      = 0x0841;
uint16_t C_PANEL   = 0x10A3;
uint16_t C_WHITE   = 0xFFFF;
uint16_t C_GREY    = 0x9CF3;
uint16_t C_RED     = 0xF986;
uint16_t C_CYAN    = 0x07FF;
uint16_t C_GREEN   = 0x07E0;
uint16_t C_YELLOW  = 0xFFE0;
uint16_t C_PURPLE  = 0xA2FF;

// ---------- LED ----------
void setLED(uint8_t r, uint8_t g, uint8_t b) {
  RGB.setPixelColor(0, RGB.Color(r, g, b));
  RGB.show();
}
void ledOff() { RGB.clear(); RGB.show(); }

// Forward declaration: drawStats() calls this before its definition.
void updateCountdown(bool force);

// ---------- TFT helpers ----------
void drawHeader() {
  tft.fillScreen(C_BG);
  tft.fillRoundRect(7, 7, 156, 39, 7, C_PANEL);
  tft.fillRoundRect(13, 14, 22, 22, 5, C_RED);
  // play triangle
  tft.fillTriangle(21, 18, 21, 32, 30, 25, C_WHITE);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(40, 18); tft.print("SHORTS BOT");
}

void drawStatusBadge(const char *label, uint16_t color) {
  // Colored badge with a clear 2-pixel inset frame, leaving breathing room around text.
  tft.fillRoundRect(10, 50, 150, 42, 6, color);
  tft.drawRoundRect(11, 51, 148, 40, 5, C_WHITE);
  tft.drawRoundRect(13, 53, 144, 36, 4, C_BG);
  tft.setTextColor(C_BG);
  tft.setTextSize(2);

  char first[24] = {0};
  char second[24] = {0};
  if (strcmp(label, "WATCHING SHORT") == 0) {
    strcpy(first, "WATCHING"); strcpy(second, "SHORT");
  } else if (strcmp(label, "DOUBLE TAP / LIKE") == 0) {
    strcpy(first, "DOUBLE TAP"); strcpy(second, "/ LIKE");
  } else if (strcmp(label, "SWIPE TO NEXT") == 0) {
    strcpy(first, "SWIPE TO"); strcpy(second, "NEXT");
  } else if (strcmp(label, "SEQUENCE COMPLETE") == 0) {
    strcpy(first, "SEQUENCE"); strcpy(second, "COMPLETE");
  } else if (strcmp(label, "CONNECT PHONE") == 0) {
    strcpy(first, "CONNECT"); strcpy(second, "PHONE");
  } else if (strcmp(label, "BOOTING USB HID") == 0) {
    strcpy(first, "BOOTING"); strcpy(second, "USB HID");
  } else {
    strncpy(first, label, sizeof(first) - 1);
  }

  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds(first, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((170 - w) / 2, second[0] ? 56 : 63);
  tft.print(first);
  if (second[0]) {
    tft.getTextBounds(second, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((170 - w) / 2, 72);
    tft.print(second);
  }
}
void drawStats() {
  tft.fillRoundRect(10, 95, 150, 48, 6, C_PANEL);
  tft.setTextSize(1);
  tft.setTextColor(C_GREY);
  tft.setCursor(18, 102); tft.print("VIEWED");
  tft.setCursor(92, 102); tft.print("LIKES SENT");
  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(18, 116); tft.printf("%03d", shortsDone);
  tft.setCursor(112, 116); tft.printf("%02d", likesSent);

  tft.fillRoundRect(10, 151, 150, 59, 6, C_PANEL);
  tft.setTextSize(1);
  tft.setTextColor(C_GREY);
  tft.setCursor(18, 157); tft.print("NOW WATCHING");
  updateCountdown(true);
}

void updateCountdown(bool force = false) {
  static int lastShown = -999;
  int secondsLeft = 0;
  if (uiMode == MODE_WATCHING) {
    long remaining = (long)currentWatchMs - (long)(millis() - modeStarted);
    if (remaining > 0) secondsLeft = (remaining + 999) / 1000;
  }
  int shown = (uiMode == MODE_WATCHING) ? secondsLeft : -1;
  if (!force && shown == lastShown) return;
  lastShown = shown;

  // Clear only the digits region; never repaint the entire statistics panel.
  tft.fillRect(16, 173, 100, 24, C_PANEL);
  tft.setTextColor(C_CYAN);
  tft.setTextSize(2);
  tft.setCursor(18, 175);
  if (uiMode == MODE_WATCHING) tft.printf("00:%02d", secondsLeft);
  else tft.print("--:--");

  tft.fillRect(17, 198, 136, 9, C_PANEL);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(18, 199);
  if (uiMode == MODE_WATCHING) tft.print("Watching current Short");
  else if (uiMode == MODE_LIKING) tft.print("Sending double tap");
  else if (uiMode == MODE_SWIPING) tft.print("Moving to next Short");
  else tft.print("Autopilot ready");
}

void updateCounters() {
  tft.fillRect(16, 114, 49, 24, C_PANEL);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(18, 116); tft.printf("%03d", shortsDone);
  tft.fillRect(110, 114, 42, 24, C_PANEL);
  tft.setCursor(112, 116); tft.printf("%02d", likesSent);
}
void drawAnimation() {
  // Cute animated mini robot. Drawn from primitives so no bitmap files are needed.
  tft.fillRoundRect(10, 218, 150, 66, 7, C_PANEL);

  int bob = (animFrame % 2) ? 2 : 0;
  int cx = 43;
  int cy = 249 + bob;

  // Shadow
  tft.fillRoundRect(cx - 22, cy + 15, 44, 4, 2, 0x0861);

  // Arms behind the body
  tft.drawLine(cx - 15, cy + 2, cx - 23, cy + 10, C_CYAN);
  tft.drawLine(cx + 15, cy + 2, cx + 23, cy + 8, C_CYAN);
  tft.fillCircle(cx - 24, cy + 11, 4, C_CYAN);
  tft.fillCircle(cx + 24, cy + 9, 4, C_CYAN);

  // Body
  tft.fillRoundRect(cx - 15, cy + 1, 30, 17, 5, 0x39E7);
  tft.drawRoundRect(cx - 15, cy + 1, 30, 17, 5, C_CYAN);
  tft.fillRoundRect(cx - 7, cy + 5, 14, 7, 2, C_RED); // tiny play badge
  tft.fillTriangle(cx - 2, cy + 6, cx - 2, cy + 11, cx + 3, cy + 8, C_WHITE);

  // Head / helmet
  tft.fillRoundRect(cx - 21, cy - 20, 42, 34, 9, 0x7DFF);
  tft.drawRoundRect(cx - 21, cy - 20, 42, 34, 9, C_WHITE);
  tft.fillRoundRect(cx - 16, cy - 15, 32, 24, 6, 0x0841);

  // Ear pieces
  tft.fillRoundRect(cx - 25, cy - 10, 6, 15, 3, C_CYAN);
  tft.fillRoundRect(cx + 19, cy - 10, 6, 15, 3, C_CYAN);

  // Animated eyes blink occasionally
  if (animFrame % 7 == 0) {
    tft.drawFastHLine(cx - 10, cy - 5, 6, C_YELLOW);
    tft.drawFastHLine(cx + 4, cy - 5, 6, C_YELLOW);
  } else {
    tft.fillRoundRect(cx - 10, cy - 8, 6, 8, 2, C_CYAN);
    tft.fillRoundRect(cx + 4, cy - 8, 6, 8, 2, C_CYAN);
  }

  // Smile
  tft.drawPixel(cx - 5, cy + 2, C_WHITE);
  tft.drawFastHLine(cx - 4, cy + 3, 8, C_WHITE);
  tft.drawPixel(cx + 4, cy + 2, C_WHITE);

  // Antenna
  tft.drawLine(cx, cy - 20, cx, cy - 25, C_CYAN);
  tft.fillCircle(cx, cy - 27, 3, (animFrame % 3 == 0) ? C_RED : C_YELLOW);

  // Status and animated dots
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE);
  tft.setCursor(76, 231);
  tft.print("SHORTS BOT");
  tft.setTextColor(C_GREY);
  tft.setCursor(76, 245);
  const char *dots[] = {"thinking .  ", "thinking .. ", "thinking ..."};
  tft.print(dots[animFrame % 3]);
  tft.setTextColor(C_YELLOW);
  tft.setCursor(76, 261);
  tft.print("AUTO MODE");
}
void animateRobot() {
  // Update only tiny animated areas; the rest of the robot is drawn once.
  int bob = (animFrame % 2) ? 2 : 0;
  int cx = 43;
  int cy = 249 + bob;
  // Keep the robot body static to avoid the whole-panel flashing.
  tft.fillRect(cx - 12, 237, 24, 13, 0x0841);
  if (animFrame % 7 == 0) {
    tft.drawFastHLine(cx - 10, 244, 6, C_YELLOW);
    tft.drawFastHLine(cx + 4, 244, 6, C_YELLOW);
  } else {
    tft.fillRoundRect(cx - 10, 241, 6, 8, 2, C_CYAN);
    tft.fillRoundRect(cx + 4, 241, 6, 8, 2, C_CYAN);
  }
  tft.fillRect(74, 243, 80, 11, C_PANEL);
  tft.setTextSize(1); tft.setTextColor(C_GREY); tft.setCursor(76, 245);
  const char *dots[] = {"thinking .  ", "thinking .. ", "thinking ..."};
  tft.print(dots[animFrame % 3]);
  tft.fillCircle(cx, 222, 4, C_PANEL);
  tft.fillCircle(cx, 222, 3, (animFrame % 3 == 0) ? C_RED : C_YELLOW);
}

void drawFooter() {
  tft.setTextSize(1);
  tft.setTextColor(C_GREY);
  tft.setCursor(12, 298);
  tft.print("SMD VIBE  |  DIY LAB");
  tft.drawFastHLine(10, 291, 150, C_PANEL);
}

void drawDashboard() {
  drawHeader();
  switch (uiMode) {
    case MODE_BOOT:     drawStatusBadge("BOOTING USB HID", C_YELLOW); break;
    case MODE_WAITING:  drawStatusBadge("CONNECT PHONE", C_YELLOW); break;
    case MODE_WATCHING: drawStatusBadge("WATCHING SHORT", C_GREEN); break;
    case MODE_LIKING:   drawStatusBadge("DOUBLE TAP / LIKE", C_RED); break;
    case MODE_SWIPING:  drawStatusBadge("SWIPE TO NEXT", C_CYAN); break;
    case MODE_DONE:     drawStatusBadge("SEQUENCE COMPLETE", C_PURPLE); break;
  }
  drawStats();
  drawAnimation();
  drawFooter();
}

void setMode(UiMode mode) {
  uiMode = mode;
  modeStarted = millis();
  switch (uiMode) {
    case MODE_BOOT:     drawStatusBadge("BOOTING USB HID", C_YELLOW); break;
    case MODE_WAITING:  drawStatusBadge("CONNECT PHONE", C_YELLOW); break;
    case MODE_WATCHING: drawStatusBadge("WATCHING SHORT", C_GREEN); break;
    case MODE_LIKING:   drawStatusBadge("DOUBLE TAP / LIKE", C_RED); break;
    case MODE_SWIPING:  drawStatusBadge("SWIPE TO NEXT", C_CYAN); break;
    case MODE_DONE:     drawStatusBadge("SEQUENCE COMPLETE", C_PURPLE); break;
  }
  updateCountdown(true);
}

void uiTick() {
  if (millis() - lastAnim >= 350) {
    lastAnim = millis();
    animFrame++;
    animateRobot();
  }
  if (uiMode == MODE_WATCHING) updateCountdown();
}

// Delay that keeps the display animated and responsive.
void waitWithUI(uint32_t ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    uiTick();
    delay(5);
  }
}

// ---------- Touch actions ----------
void swipeUp() {
  setMode(MODE_SWIPING);
  setLED(0, 0, 100);

  const uint16_t x = 2048;
  const uint16_t yStart = 3400;
  const uint16_t yEnd = 700;
  Touch.sendTouch(true, x, yStart);
  waitWithUI(120);

  const int steps = 40;
  for (int i = 1; i <= steps; i++) {
    uint16_t y = yStart + ((int32_t)(yEnd - yStart) * i / steps);
    Touch.sendTouch(true, x, y);
    waitWithUI(10);
  }
  Touch.sendTouch(false, x, yEnd);
  waitWithUI(500);
  ledOff();
  swipesSent++;
  shortsDone++;
  updateCounters();
}

void doubleTap() {
  setMode(MODE_LIKING);
  setLED(100, 0, 0);

  const uint16_t x = 2048;
  const uint16_t y = 2048;
  Touch.sendTouch(true, x, y);
  waitWithUI(80);
  Touch.sendTouch(false, x, y);
  waitWithUI(120);
  Touch.sendTouch(true, x, y);
  waitWithUI(80);
  Touch.sendTouch(false, x, y);
  waitWithUI(300);
  ledOff();
  likesSent++;
  updateCounters();
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(170, 320);
  tft.setRotation(0); // Change to 1/2/3 if orientation is wrong.
  tft.fillScreen(C_BG);
  if (TFT_BL >= 0) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  }

  RGB.begin();
  RGB.setBrightness(100);
  ledOff();

  uiMode = MODE_BOOT;
  modeStarted = millis();
  drawDashboard();
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(15, 290);
  tft.print("Starting USB HID...");

  HID.begin();
  USB.begin();

  waitWithUI(START_DELAY_MS);
  randomSeed(esp_random());

  setMode(MODE_WAITING);
  // Give the phone a little time after USB enumeration.
  waitWithUI(1000);

  // Run continuously while powered. Reset or disconnect power to stop.
  while (true) {
    currentWatchMs = random(MIN_WATCH_MS, MAX_WATCH_MS + 1);
    setMode(MODE_WATCHING);
    unsigned long watchStart = millis();
    while (millis() - watchStart < (unsigned long)currentWatchMs) {
      uiTick();
      delay(5);
    }

    // Counts the double-tap command sent, not a verified YouTube like.
    if (random(0, 100) < LIKE_PERCENT) {
      doubleTap();
      waitWithUI(random(MIN_AFTER_LIKE_MS, MAX_AFTER_LIKE_MS + 1));
    }

    swipeUp();
    waitWithUI(700);
  }
}

void loop() {
  uiTick();
  delay(10);
}
