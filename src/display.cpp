// Glossaray Waveshare V2 display foundation.
//
// QSPI pins and AXP2101 initialization are adapted from:
//   steveruizok/chat-stick
//   commit 3321c9bfc9771ee8b3adc4815f6c72890d3db125
//   MIT License, Copyright (c) 2026 Steve Ruiz
//
// Glossaray deliberately uses Arduino_GFX's CO5300 panel class. It does not use
// the discontinued V1 SH8601/FT3168 initialization.

#include "display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

constexpr int kScreenWidth = 368;
constexpr int kScreenHeight = 448;

// Waveshare ESP32-S3-Touch-AMOLED-1.8 V2 QSPI mapping, retained from the
// pinned exact-board Chat Stick implementation.
constexpr int kLcdSdio0 = 4;
constexpr int kLcdSdio1 = 5;
constexpr int kLcdSdio2 = 6;
constexpr int kLcdSdio3 = 7;
constexpr int kLcdSclk = 11;
constexpr int kLcdCs = 12;
constexpr int kI2cSda = 15;
constexpr int kI2cScl = 14;

uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return static_cast<uint16_t>(((red & 0xF8) << 8) |
                               ((green & 0xFC) << 3) | (blue >> 3));
}

Arduino_DataBus* displayBus =
    new Arduino_ESP32QSPI(kLcdCs, kLcdSclk, kLcdSdio0, kLcdSdio1, kLcdSdio2,
                         kLcdSdio3);
Arduino_CO5300* panel =
    new Arduino_CO5300(displayBus, GFX_NOT_DEFINED, 0, kScreenWidth,
                       kScreenHeight, 16, 0, 0, 0);
XPowersPMU power;
bool powerReady = false;
bool externalPowerPresent = false;
unsigned long lastPowerPollMs = 0;
unsigned long pendingShortAtMs = 0;
constexpr unsigned long kDoublePressWindowMs = 320;
SemaphoreHandle_t displayMutex = nullptr;
TaskHandle_t bootAnimationTask = nullptr;
volatile bool bootAnimationRunning = false;
unsigned long lastBatteryRefreshMs = 0;
constexpr int kBootFishAreaX = 128;
constexpr int kBootFishAreaY = 45;
constexpr int kBootFishAreaWidth = 112;
constexpr int kBootFishAreaHeight = 78;
uint16_t* bootFishFrame = nullptr;
bool readySceneActive = false;

// There is ONE type size, because there cannot be two: u8g2 at textsize 1
// paints a glyph as 1px-high fills, and a one-pixel primitive on this CO5300
// breaks into dots - the same thing that made drawBatteryIndicator() use
// two-pixel geometry. So a translation longer than the pane holds MOVES
// instead of shrinking, one line at a time, and returns to the top when it
// reaches the end. The tap stays REPEAT and nothing has to be learned.
constexpr int kFirstResultBaseline = 146;
constexpr int kResultLineHeight = 34;
constexpr int kResultLines = 7;
constexpr unsigned long kScrollStepMs = 2200;
constexpr unsigned long kScrollHoldMs = 4500;
String scrollText;
int scrollLines = 0;
int scrollOffset = 0;
unsigned long scrollAt = 0;
bool scrollActive = false;

struct Palette {
  uint16_t background;
  uint16_t surface;
  uint16_t active;
  uint16_t text;
  uint16_t status;
};

Palette palette() {
  return {
      rgb565(0x05, 0x02, 0x07),
      rgb565(0x24, 0x10, 0x33),
      rgb565(0x71, 0x3F, 0xAD),
      rgb565(0xC6, 0xA0, 0xFF),
      rgb565(0x4F, 0xDD, 0xE5),
  };
}

// Original 16x11 Glossaray fish from the curated v0 reference. Spaces are
// transparent; other cells map directly to the canonical palette.
constexpr const char* kFish[] = {
    "     dddddd     ", "   dddppppddd   ", "  cdppppppppd   ",
    " ccdpppppppppd  ", "cccdpppplllpppd ", "cccdppppldkpppd ",
    "cccdpppplllpppd ", " ccdpppppppppd  ", "  cdppppppppd   ",
    "   dddppppddd   ", "     dddddd     ",
};

uint16_t fishColor(char cell, const Palette& colors) {
  switch (cell) {
    case 'd':
      return colors.surface;
    case 'p':
      return colors.active;
    case 'l':
      return colors.text;
    case 'c':
      return colors.status;
    case 'k':
      return colors.background;
    default:
      return colors.background;
  }
}

void drawFish(int originX, int originY, int pixel, const Palette& colors) {
  for (int row = 0; row < 11; ++row) {
    for (int column = 0; column < 16; ++column) {
      const char cell = kFish[row][column];
      if (cell == ' ') {
        continue;
      }
      panel->fillRect(originX + column * pixel, originY + row * pixel, pixel,
                      pixel, fishColor(cell, colors));
    }
  }
}

void drawBatteryIndicator(const Palette& colors) {
  externalPowerPresent = powerReady && power.isVbusIn();
  if (!powerReady || !power.isBatteryConnect()) return;
  constexpr int x = 322;
  constexpr int y = 16;
  constexpr int width = 28;
  constexpr int height = 16;
  const bool charging = power.isCharging();
  const int percent = constrain(power.getBatteryPercent(), 0, 100);
  const uint16_t outline = charging ? colors.status : colors.surface;
  const uint16_t fill = charging ? colors.status : colors.active;

  // Two-pixel filled geometry is intentional: one-pixel CO5300 primitives
  // were barely visible on the physical panel and reduced this to dots.
  panel->fillRect(x, y, width, 2, outline);
  panel->fillRect(x, y + height - 2, width, 2, outline);
  panel->fillRect(x, y + 2, 2, height - 4, outline);
  panel->fillRect(x + width - 2, y + 2, 2, height - 4, outline);
  panel->fillRect(x + width, y + 4, 4, height - 8, outline);
  const int bars = percent == 0 ? 0 : (percent + 24) / 25;
  for (int bar = 0; bar < bars; ++bar) {
    panel->fillRect(x + 4 + bar * 6, y + 4, 4, height - 8, fill);
  }
  if (charging) {
    panel->fillRect(x - 10, y + 7, 8, 2, colors.status);
    panel->fillRect(x - 7, y + 4, 2, 8, colors.status);
  }
}

void lockDisplay() {
  if (displayMutex != nullptr) xSemaphoreTake(displayMutex, portMAX_DELAY);
}

void unlockDisplay() {
  if (displayMutex != nullptr) xSemaphoreGive(displayMutex);
}

// Temporary first-light text abstraction. It intentionally uses the
// hard-edged built-in GFX bitmap face and can later be replaced by a pinned,
// licensed font with verified Welsh coverage.
void drawCenteredText(const char* text, int baselineY, uint8_t scale,
                      uint16_t color) {
  int16_t boundsX = 0;
  int16_t boundsY = 0;
  uint16_t boundsWidth = 0;
  uint16_t boundsHeight = 0;
  panel->setTextSize(scale);
  panel->setTextWrap(false);
  panel->getTextBounds(text, 0, 0, &boundsX, &boundsY, &boundsWidth,
                       &boundsHeight);
  const int x = (kScreenWidth - static_cast<int>(boundsWidth)) / 2;
  panel->setTextColor(color);
  panel->setCursor(x, baselineY);
  panel->print(text);
}

// One pass of the wrap. With `draw` false it counts instead of painting, so
// the same code both decides how many lines the text needs and lays it out; a
// separate measuring routine would drift from this one and the count would
// stop describing the picture.
int layoutResult(const char* text, const Palette& colors, uint8_t textSize,
                 int lineHeight, int maximumLines, bool draw, int skipLines,
                 bool clearRows) {
  constexpr int kMaximumWidth = kScreenWidth - 80;
  constexpr int kFirstBaseline = kFirstResultBaseline;
  // COLUMNS, not measured pixels. getTextBounds() reports a string's ink
  // extents and unifont is a character-cell font that advances 8px whatever is
  // inked, so measuring fitted 33 characters into a line that then drew 528px
  // wide and ran off the panel. That was the text going missing off the right
  // edge, and no amount of line counting would have found it.
  const int kAdvance = 8 * textSize;
  const int maximumColumns = kMaximumWidth / kAdvance;
  String line;
  String word;
  int lineColumns = 0;
  int wordColumns = 0;
  int renderedLines = 0;

  panel->setFont(u8g2_font_unifont_h_utf8);
  panel->setUTF8Print(true);
  panel->setTextSize(textSize);

  const auto flushLine = [&]() {
    if (line.length() == 0) return;
    const int row = renderedLines - skipLines;
    const bool onScreen = row >= 0 && row < maximumLines;
    if (draw && onScreen) {
      // Centred on the ADVANCE width, for the same reason the wrap counts
      // columns: the ink is narrower than the cell and centring on it would
      // push every line right of where it belongs.
      const int x = (kScreenWidth - lineColumns * kAdvance) / 2;
      // Clear THIS row and immediately draw it. Blanking the whole pane first
      // and filling it afterwards is the same pixels in the end, but the eye
      // catches the empty frame in between and it reads as a flicker.
      if (clearRows) {
        panel->fillRect(40, kFirstBaseline + row * lineHeight - lineHeight + 2,
                        kScreenWidth - 80, lineHeight, colors.background);
      }
      panel->setTextSize(textSize);
      panel->setTextWrap(false);
      panel->setTextColor(colors.text);
      panel->setCursor(x, kFirstBaseline + row * lineHeight);
      panel->print(line.c_str());
    }
    line = "";
    lineColumns = 0;
    ++renderedLines;
  };

  const auto commitWord = [&]() {
    if (word.length() > 0) {
      const int gap = lineColumns > 0 ? 1 : 0;
      if (lineColumns + gap + wordColumns > maximumColumns) flushLine();
      if (renderedLines >= skipLines + maximumLines) return;
      if (lineColumns > 0) {
        line += ' ';
        ++lineColumns;
      }
      line += word;
      lineColumns += wordColumns;
      word = "";
      wordColumns = 0;
    }
  };

  for (size_t index = 0; text[index] != '\0' && renderedLines < skipLines + maximumLines;) {
    const uint8_t lead = static_cast<uint8_t>(text[index]);
    size_t bytes = 1;
    uint32_t codepoint = lead;
    if ((lead & 0xE0) == 0xC0) {
      bytes = 2;
      codepoint = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
      bytes = 3;
      codepoint = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
      bytes = 4;
      codepoint = lead & 0x07;
    }
    bool valid = true;
    for (size_t offset = 1; offset < bytes; ++offset) {
      const uint8_t continuation = static_cast<uint8_t>(text[index + offset]);
      if (continuation == 0 || (continuation & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      codepoint = (codepoint << 6) | (continuation & 0x3F);
    }
    if (!valid) {
      bytes = 1;
      codepoint = lead;
    }

    const bool cjk =
        (codepoint >= 0x2E80 && codepoint <= 0x9FFF) ||
        (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
        (codepoint >= 0x3040 && codepoint <= 0x30FF);
    if (cjk) {
      commitWord();
      String glyph;
      for (size_t offset = 0; offset < bytes; ++offset) {
        glyph += text[index + offset];
      }
      // A CJK glyph is a full-width cell: two of unifont's columns, not one.
      if (lineColumns + 2 > maximumColumns) flushLine();
      if (renderedLines < skipLines + maximumLines) {
        line += glyph;
        lineColumns += 2;
      }
    } else if (codepoint == ' ' || codepoint == '\t' || codepoint == '\n' ||
               codepoint == '\r') {
      commitWord();
      if (codepoint == '\n') flushLine();
    } else {
      for (size_t offset = 0; offset < bytes; ++offset) {
        word += text[index + offset];
      }
      ++wordColumns;
      // A word wider than the panel breaks at a glyph boundary rather than
      // running off it. Llanfairpwllgwyngyll... is a real translation.
      if (wordColumns >= maximumColumns) {
        commitWord();
        flushLine();
      }
    }
    index += bytes;
  }
  commitWord();
  flushLine();
  panel->setFont(static_cast<const GFXfont*>(nullptr));
  panel->setUTF8Print(false);
  return renderedLines;
}

void drawWrappedResult(const char* text, const Palette& colors) {
  // Measure first, draw second, and remember the total: tickTranslation()
  // needs to know how far there is to go before it moves anything.
  const int needed = layoutResult(text, colors, 2, kResultLineHeight, 999,
                                  false, 0, false);
  layoutResult(text, colors, 2, kResultLineHeight, kResultLines, true,
               scrollOffset, false);
  scrollLines = needed;
}

bool initializePower() {
  Wire.begin(kI2cSda, kI2cScl);
  Wire.setClock(400000);
  if (!power.begin(Wire, AXP2101_SLAVE_ADDRESS, kI2cSda, kI2cScl)) {
    Serial.println("AXP2101: not detected; continuing with existing rails.");
    return false;
  }
  power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  power.clearIrqStatus();
  power.enableIRQ(XPOWERS_AXP2101_PKEY_POSITIVE_IRQ |
                  XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ |
                  XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                  XPOWERS_AXP2101_PKEY_LONG_IRQ);
  power.setChargeTargetVoltage(3);
  power.disableLongPressShutdown();
  powerReady = true;
  externalPowerPresent = power.isVbusIn();
  Serial.printf("AXP2101: online, battery=%dmV, VBUS=%dmV\n",
                power.getBattVoltage(), power.getVbusVoltage());
  return true;
}

void drawFirstLight() {
  const Palette colors = palette();
  panel->fillScreen(colors.background);

  constexpr int kFishPixel = 7;
  constexpr int kFishWidth = 16 * kFishPixel;
  // The final sprite column is transparent, so move the visible 15-column
  // artwork three pixels right to centre its actual 105-pixel painted bounds.
  drawFish((kScreenWidth - kFishWidth) / 2 + 3, 72, kFishPixel, colors);

  drawCenteredText("GLOSSARAY", 205, 5, colors.text);
  drawCenteredText("EN -> IT", 264, 3, colors.text);

  panel->drawFastHLine(132, 331, 104, colors.active);
  drawCenteredText("READY", 350, 3, colors.status);
  panel->drawFastHLine(132, 386, 104, colors.active);
}

void drawBootFish(int offsetY) {
  const Palette colors = palette();
  constexpr int kFishPixel = 6;
  constexpr int kFishWidth = 16 * kFishPixel;
  if (bootFishFrame == nullptr) {
    panel->fillRect(kBootFishAreaX, kBootFishAreaY, kBootFishAreaWidth,
                    kBootFishAreaHeight, colors.background);
    drawFish((kScreenWidth - kFishWidth) / 2 + 3, 50 + offsetY, kFishPixel,
             colors);
    return;
  }

  const size_t pixels = kBootFishAreaWidth * kBootFishAreaHeight;
  for (size_t index = 0; index < pixels; ++index) {
    bootFishFrame[index] = colors.background;
  }
  const int originX = (kScreenWidth - kFishWidth) / 2 + 3 - kBootFishAreaX;
  const int originY = 50 + offsetY - kBootFishAreaY;
  for (int row = 0; row < 11; ++row) {
    for (int column = 0; column < 16; ++column) {
      const char cell = kFish[row][column];
      if (cell == ' ') continue;
      const uint16_t color = fishColor(cell, colors);
      for (int py = 0; py < kFishPixel; ++py) {
        const int y = originY + row * kFishPixel + py;
        for (int px = 0; px < kFishPixel; ++px) {
          const int x = originX + column * kFishPixel + px;
          bootFishFrame[y * kBootFishAreaWidth + x] = color;
        }
      }
    }
  }
  panel->draw16bitRGBBitmap(kBootFishAreaX, kBootFishAreaY, bootFishFrame,
                            kBootFishAreaWidth, kBootFishAreaHeight);
}

void runBootAnimation(void*) {
  constexpr int kOffsets[] = {0, -1, -2, -3, -2, -1, 0, 1, 2, 3, 2, 1};
  size_t frame = 0;
  while (bootAnimationRunning) {
    lockDisplay();
    drawBootFish(kOffsets[frame]);
    unlockDisplay();
    frame = (frame + 1) % (sizeof(kOffsets) / sizeof(kOffsets[0]));
    vTaskDelay(pdMS_TO_TICKS(110));
  }
  bootAnimationTask = nullptr;
  vTaskDelete(nullptr);
}

void drawBootIntro() {
  const Palette colors = palette();
  panel->fillScreen(colors.background);

  // A restrained CRT-like reveal: a bright centre line opens horizontally,
  // then resolves into the wordmark with a short lavender afterglow.
  constexpr int kCentreY = 174;
  for (int width = 8; width <= 216; width += 12) {
    panel->fillRect((kScreenWidth - width) / 2, kCentreY, width, 2,
                    colors.status);
    delay(24);
  }
  panel->fillRect(76, kCentreY - 2, 216, 6, colors.text);
  delay(120);
  panel->fillRect(76, kCentreY - 2, 216, 6, colors.background);
  drawCenteredText("GLOSSARAY", 157, 5, colors.active);
  delay(140);
  drawCenteredText("GLOSSARAY", 157, 5, colors.text);
  panel->drawFastHLine(76, 202, 216, colors.surface);
  drawBootFish(0);
  drawBatteryIndicator(colors);
}

void drawGeometryCalibration() {
  const Palette colors = palette();
  panel->fillScreen(colors.background);

  // Use filled bars rather than thin-line primitives: the first physical
  // calibration showed the blocks but not the one-pixel guides.
  panel->fillRect(0, 0, 8, kScreenHeight, colors.status);
  panel->fillRect(kScreenWidth - 8, 0, 8, kScreenHeight, colors.status);
  panel->fillRect(8, 0, kScreenWidth - 16, 8, colors.status);
  panel->fillRect(8, kScreenHeight - 8, kScreenWidth - 16, 8, colors.status);

  // Symmetric eight-pixel bars with sixteen-pixel outer margins.
  panel->fillRect(16, 16, 8, kScreenHeight - 32, colors.active);
  panel->fillRect(kScreenWidth - 24, 16, 8, kScreenHeight - 32,
                  colors.active);
  panel->fillRect(24, 16, kScreenWidth - 48, 8, colors.active);
  panel->fillRect(24, kScreenHeight - 24, kScreenWidth - 48, 8,
                  colors.active);

  // A 368x448 viewport has its centre between columns 183/184 and rows 223/224.
  panel->fillRect(182, 32, 4, kScreenHeight - 64, colors.text);
  panel->fillRect(32, 222, kScreenWidth - 64, 4, colors.text);

  // Equal mirrored blocks expose any asymmetric clipping immediately.
  panel->fillRect(32, 48, 32, 32, colors.active);
  panel->fillRect(kScreenWidth - 64, 48, 32, 32, colors.active);
  panel->fillRect(32, kScreenHeight - 80, 32, 32, colors.active);
  panel->fillRect(kScreenWidth - 64, kScreenHeight - 80, 32, 32,
                  colors.active);

  drawCenteredText("368 x 448", 194, 2, colors.status);
  drawCenteredText("CENTER", 256, 2, colors.status);
}

}  // namespace

namespace GlossarayDisplay {

bool begin() {
  initializePower();

  if (!panel->begin()) {
    Serial.println("CO5300: initialization failed.");
    return false;
  }
  panel->setBrightness(96);
  displayMutex = xSemaphoreCreateMutex();
  bootFishFrame = static_cast<uint16_t*>(heap_caps_malloc(
      kBootFishAreaWidth * kBootFishAreaHeight * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (bootFishFrame == nullptr) {
    Serial.println("CO5300: PSRAM fish frame unavailable; using direct draws.");
  }
  drawBootIntro();
  bootAnimationRunning = true;
  if (xTaskCreatePinnedToCore(runBootAnimation, "glossaray-boot", 3072, nullptr, 1,
                              &bootAnimationTask, 0) != pdPASS) {
    bootAnimationRunning = false;
    bootAnimationTask = nullptr;
    Serial.println("CO5300: boot animation task unavailable; using static fish.");
  }
  Serial.println("CO5300: branded boot scene rendered.");
  return true;
}

void showBootStatus(const char* title, const char* detail) {
  const Palette colors = palette();
  lockDisplay();
  panel->fillRect(20, 218, kScreenWidth - 40, 166, colors.background);
  drawCenteredText(title, 263, 3, colors.text);
  panel->drawFastHLine(104, 303, 160, colors.surface);
  drawCenteredText(detail, 334, 2, colors.status);
  unlockDisplay();
}

void finishBoot() {
  bootAnimationRunning = false;
  const unsigned long waitStarted = millis();
  while (bootAnimationTask != nullptr && millis() - waitStarted < 500) {
    delay(10);
  }
}

void refreshBatteryIndicator() {
  const unsigned long now = millis();
  if (now - lastBatteryRefreshMs < 2000) return;
  lastBatteryRefreshMs = now;
  const Palette colors = palette();
  lockDisplay();
  panel->fillRect(309, 14, 47, 20, colors.background);
  drawBatteryIndicator(colors);
  unlockDisplay();
}

void showStatus(const char* title, const char* detail,
                const char* secondDetail) {
  const Palette colors = palette();
  lockDisplay();
  readySceneActive = false;
  scrollActive = false;
  panel->fillScreen(colors.background);
  panel->drawRect(24, 128, kScreenWidth - 48, 192, colors.active);
  drawCenteredText(title, 190, 3, colors.text);
  panel->drawFastHLine(104, 239, 160, colors.surface);
  if (secondDetail == nullptr) {
    drawCenteredText(detail, 270, 2, colors.status);
  } else {
    drawCenteredText(detail, 262, 2, colors.status);
    drawCenteredText(secondDetail, 292, 2, colors.status);
  }
  drawBatteryIndicator(colors);
  unlockDisplay();
}

void showReady(const char* pair, const char* nextLanguage) {
  const Palette colors = palette();
  lockDisplay();
  if (!readySceneActive) {
    panel->fillScreen(colors.background);
    panel->drawFastHLine(40, 103, kScreenWidth - 80, colors.active);
    drawCenteredText("^ HOLD TO TALK", 246, 3, colors.text);
    panel->drawFastHLine(40, 378, kScreenWidth - 80, colors.surface);
    drawCenteredText("v v SWAP DIRECTION", 424, 2, colors.active);
    drawBatteryIndicator(colors);
    readySceneActive = true;
  }

  // Keep the canvas stable while cycling: update only pair and next action.
  panel->fillRect(34, 45, kScreenWidth - 68, 54, colors.background);
  panel->fillRect(34, 382, kScreenWidth - 68, 30, colors.background);
  drawCenteredText(pair, 72, 3, colors.status);
  String preview = "v TAP NEXT  ";
  preview += nextLanguage;
  drawCenteredText(preview.c_str(), 398, 2, colors.status);
  unlockDisplay();
}

void showTranslation(const char* pair, const char* text, const char* footer,
                     const char* secondFooter) {
  const Palette colors = palette();
  lockDisplay();
  readySceneActive = false;
  // A new translation always starts at its first line.
  scrollText = text;
  scrollOffset = 0;
  scrollAt = millis();
  panel->fillScreen(colors.background);
  drawCenteredText(pair, 72, 3, colors.status);
  panel->drawFastHLine(40, 103, kScreenWidth - 80, colors.active);
  drawWrappedResult(text, colors);
  panel->drawFastHLine(40, 378, kScreenWidth - 80, colors.surface);
  drawBatteryIndicator(colors);
  if (secondFooter == nullptr) {
    drawCenteredText(footer, 410, 2, colors.status);
  } else {
    drawCenteredText(footer, 398, 2, colors.status);
    drawCenteredText(secondFooter, 424, 2, colors.active);
  }
  scrollActive = scrollLines > kResultLines;
  unlockDisplay();
}

void tickTranslation() {
  if (!scrollActive) return;
  const int lastOffset = scrollLines - kResultLines;
  if (lastOffset <= 0) return;
  // Longer at the end than between lines, so the last of the translation is
  // readable before it goes back to the first.
  const unsigned long due = scrollOffset >= lastOffset ? kScrollHoldMs
                                                       : kScrollStepMs;
  const unsigned long now = millis();
  if (now - scrollAt < due) return;
  scrollAt = now;
  scrollOffset = scrollOffset >= lastOffset ? 0 : scrollOffset + 1;

  const Palette colors = palette();
  lockDisplay();
  // Only the pane, and a row at a time: the rules and footers are not moving,
  // and neither is most of the text on any one step.
  const int total = layoutResult(scrollText.c_str(), colors, 2,
                                 kResultLineHeight, kResultLines, true,
                                 scrollOffset, true);
  // The last screenful is usually short of seven lines, so whatever the
  // previous offset left below it has to go.
  for (int row = total - scrollOffset; row < kResultLines; ++row) {
    if (row < 0) continue;
    panel->fillRect(40, kFirstResultBaseline + row * kResultLineHeight -
                            kResultLineHeight + 2,
                    kScreenWidth - 80, kResultLineHeight, colors.background);
  }
  panel->setFont(static_cast<const GFXfont*>(nullptr));
  panel->setUTF8Print(false);
  unlockDisplay();
}

PowerEvent pollPowerEvent() {
  const unsigned long now = millis();
  if (!powerReady || now - lastPowerPollMs < 20) return PowerEvent::None;
  lastPowerPollMs = now;

  power.getIrqStatus();
  const bool shortPress = power.isPekeyShortPressIrq();
  const bool longPress = power.isPekeyLongPressIrq();
  power.clearIrqStatus();

  if (longPress) {
    pendingShortAtMs = 0;
    return PowerEvent::Long;
  }
  if (shortPress) {
    if (pendingShortAtMs != 0 &&
        now - pendingShortAtMs <= kDoublePressWindowMs) {
      pendingShortAtMs = 0;
      return PowerEvent::Double;
    }
    pendingShortAtMs = now;
  }
  if (pendingShortAtMs != 0 &&
      now - pendingShortAtMs > kDoublePressWindowMs) {
    pendingShortAtMs = 0;
    return PowerEvent::Short;
  }
  return PowerEvent::None;
}

void powerOff() {
  if (powerReady) power.shutdown();
}

bool isExternalPowerPresent() {
  return externalPowerPresent;
}

void setHardwareLongPressPowerOff(bool enabled) {
  if (!powerReady) return;
  if (enabled) {
    power.enableLongPressShutdown();
  } else {
    power.disableLongPressShutdown();
  }
}

}  // namespace GlossarayDisplay
