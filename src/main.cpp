#include <Arduino.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <Preferences.h>

#include "audio.h"
#include "display.h"
#include "glossaray_network.h"
#include "gemini_stream.h"

namespace {

GlossarayNetwork::TranslationResult lastTranslation{};
constexpr int kTalkButtonPin = 0;
constexpr unsigned long kIdlePowerOffMs = 5UL * 60UL * 1000UL;
bool applicationReady = false;
bool bootSceneVisible = true;
unsigned long lastActivityAt = 0;
Preferences settings;

struct LanguagePair {
  const char* home;
  const char* away;
  bool swappable;
};

constexpr LanguagePair kPairs[] = {
    {"en", "cy", true},
    {"en", "it", true},
    {"en", "fr", true},
    {"en", "es", true},
    {"en", "de", true},
    {"en", "pl", true},
    {"en", "ru", true},
    {"en", "tr", true},
    {"en", "ja", true},
    {"en", "zh", true},
};
constexpr size_t kPairCount = sizeof(kPairs) / sizeof(kPairs[0]);
size_t selectedPair = 0;
bool pairSwapped = false;

const LanguagePair& pair() { return kPairs[selectedPair]; }

String pairLabel() {
  String label = pairSwapped ? pair().away : pair().home;
  label.toUpperCase();
  label += " -> ";
  String target = pairSwapped ? pair().home : pair().away;
  target.toUpperCase();
  label += target;
  return label;
}

String resultPowerAction() {
  if (!pair().swappable) return "v TAP CHANGE LANGUAGE";
  String action = "v v SWAP TO ";
  String source = pairSwapped ? pair().home : pair().away;
  source.toUpperCase();
  action += source;
  action += " -> ";
  String target = pairSwapped ? pair().away : pair().home;
  target.toUpperCase();
  action += target;
  return action;
}

const char* sourceCode() { return pairSwapped ? pair().away : pair().home; }
const char* targetCode() { return pairSwapped ? pair().home : pair().away; }

void markActivity() { lastActivityAt = millis(); }

void showReady() {
  const String label = pairLabel();
  const size_t nextIndex = (selectedPair + 1) % kPairCount;
  String next = kPairs[nextIndex].away;
  next.toUpperCase();
  GlossarayDisplay::showReady(label.c_str(), next.c_str());
}

void savePair() {
  settings.begin("glossaray", false);
  settings.putUChar("pair", selectedPair);
  settings.putBool("swapped", pairSwapped);
  settings.end();
}

void loadPair() {
  settings.begin("glossaray", true);
  selectedPair = settings.getUChar("pair", 0);
  pairSwapped = settings.getBool("swapped", false);
  settings.end();
  if (selectedPair >= kPairCount) selectedPair = 0;
  if (!pair().swappable) pairSwapped = false;
}

void clearCachedTranslation() {
  GlossarayNetwork::freeTranslation(&lastTranslation);
}

void showSetupPortal() {
  GlossarayDisplay::setHardwareLongPressPowerOff(true);
  // The title IS the network name, but a name alone does not tell anyone what
  // to do with it: this screen was showing how to switch the board off and
  // never once said to join the thing on a phone, which is the only action
  // that moves setup forward.
  if (bootSceneVisible) {
    GlossarayDisplay::showBootStatus("GLOSSARAY-SETUP", "JOIN THIS WI-FI ON PHONE");
  } else {
    GlossarayDisplay::showStatus("GLOSSARAY-SETUP", "JOIN THIS WI-FI ON PHONE",
                                 "HOLD v TO POWER OFF");
  }
}

void enterSetupPortal() {
  applicationReady = false;
  const GlossarayNetwork::ConfigResult result =
      GlossarayNetwork::configure(true, showSetupPortal);
  GlossarayDisplay::setHardwareLongPressPowerOff(false);
  if (result == GlossarayNetwork::ConfigResult::HotspotHandoff) {
    GlossarayDisplay::showStatus("WI-FI SAVED", "HOTSPOT THEN RESET");
  } else if (result == GlossarayNetwork::ConfigResult::Ready) {
    GlossarayDisplay::showStatus("WI-FI SAVED", "RESETTING");
    delay(800);
    ESP.restart();
  } else {
    GlossarayDisplay::showStatus("WI-FI ERROR", "TAP v TO RETRY");
  }
}

void openPowerMenu() {
  GlossarayDisplay::showStatus("POWER MENU", "^ WI-FI / v OFF");
  const unsigned long openedAt = millis();
  while (millis() - openedAt < 6000) {
    if (digitalRead(kTalkButtonPin) == LOW) {
      while (digitalRead(kTalkButtonPin) == LOW) delay(10);
      enterSetupPortal();
      return;
    }
    const GlossarayDisplay::PowerEvent event = GlossarayDisplay::pollPowerEvent();
    // Ignore the release tail of the long press that opened this menu.
    if (millis() - openedAt > 900 &&
        (event == GlossarayDisplay::PowerEvent::Short ||
         event == GlossarayDisplay::PowerEvent::Double)) {
      GlossarayDisplay::showStatus("POWERING OFF", "PRESS v TO WAKE");
      delay(700);
      GlossarayDisplay::powerOff();
      return;
    }
    delay(20);
  }
  if (applicationReady) showReady();
}

void handlePowerEvent(GlossarayDisplay::PowerEvent event) {
  markActivity();
  switch (event) {
    case GlossarayDisplay::PowerEvent::Short:
      selectedPair = (selectedPair + 1) % kPairCount;
      pairSwapped = false;
      clearCachedTranslation();
      savePair();
      showReady();
      return;
    case GlossarayDisplay::PowerEvent::Double:
      if (!pair().swappable) {
        GlossarayDisplay::showStatus("EN -> CY", "ONE WAY WELSH MODE");
        delay(900);
      } else {
        pairSwapped = !pairSwapped;
        clearCachedTranslation();
        savePair();
      }
      showReady();
      return;
    case GlossarayDisplay::PowerEvent::Long:
      openPowerMenu();
      return;
    case GlossarayDisplay::PowerEvent::None: return;
  }
}

// The server's own codes, and the same screens `errorScene()` gives them in the
// web client - keep the two lists together. This used to read an error set by
// an HTTP upload path the device no longer has, so nothing set it and every
// failure said PROVIDER ERROR whatever had actually happened.
void showTranslationFailure() {
  const char* code = GlossarayGeminiStream::lastErrorCode();
  if (strcmp(code, "busy") == 0) {
    GlossarayDisplay::showStatus("BUSY", "TRY AGAIN");
  } else if (strcmp(code, "rate_limited") == 0) {
    GlossarayDisplay::showStatus("BUSY", "TOO MANY TRIES");
  } else if (strcmp(code, "unconfigured") == 0) {
    GlossarayDisplay::showStatus("AUTH ERROR", "HOLD v FOR MENU");
  } else if (strcmp(code, "unsupported_pair") == 0) {
    GlossarayDisplay::showStatus("PROVIDER ERROR", "PAIR UNAVAILABLE");
  } else if (strcmp(code, "too_short") == 0) {
    GlossarayDisplay::showStatus("ERROR", "HOLD LONGER");
  } else if (strcmp(code, "session_expired") == 0 || strcmp(code, "idle") == 0) {
    GlossarayDisplay::showStatus("TIMEOUT", "TRY AGAIN");
  } else if (code[0] == '\0') {
    // No code means no answer: the socket closed or the reply never finished.
    GlossarayDisplay::showStatus("TIMEOUT", "CHECK CONNECTION");
  } else {
    GlossarayDisplay::showStatus("PROVIDER ERROR", "TRY AGAIN");
  }
}

void showGeminiFinishing(const void* context) {
  GlossarayDisplay::showStatus("TRANSLATING", static_cast<const char*>(context));
}

void printHardwareReport() {
  esp_chip_info_t chipInfo;
  esp_chip_info(&chipInfo);

  uint32_t flashBytes = 0;
  const esp_err_t flashResult = esp_flash_get_size(nullptr, &flashBytes);

  Serial.println();
  Serial.println("Glossaray");
  Serial.println("USB and serial bring-up ready.");
  Serial.println("Target: Waveshare ESP32-S3-Touch-AMOLED-1.8 V2 (SKU 29957)");
  Serial.printf("Chip: %s, revision %d, %d cores\n",
                ESP.getChipModel(),
                chipInfo.revision,
                chipInfo.cores);
  if (flashResult == ESP_OK) {
    Serial.printf("Flash: %lu bytes (%.1f MB)\n",
                  static_cast<unsigned long>(flashBytes),
                  flashBytes / 1048576.0);
  } else {
    Serial.printf("Flash: unavailable (%s)\n", esp_err_to_name(flashResult));
  }
  Serial.printf("PSRAM: %lu bytes (%.1f MB)\n",
                static_cast<unsigned long>(ESP.getPsramSize()),
                ESP.getPsramSize() / 1048576.0);
  Serial.printf("Free PSRAM: %lu bytes\n",
                static_cast<unsigned long>(ESP.getFreePsram()));
  Serial.printf("Audio: %s\n", GlossarayAudio::diagnosticSummary());
  Serial.printf("Network: %s\n", GlossarayNetwork::diagnosticSummary());
  Serial.println("Diagnostics: native USB serial remains available.");
  Serial.printf("Reset reason: %d\n", static_cast<int>(esp_reset_reason()));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const unsigned long waitStarted = millis();
  while (!Serial && millis() - waitStarted < 3000) {
    delay(10);
  }
  printHardwareReport();
  loadPair();
  pinMode(kTalkButtonPin, INPUT_PULLUP);
  if (!GlossarayDisplay::begin()) {
    Serial.println("Glossaray display initialization failed; serial diagnostics remain available.");
  }
  // Let the CRT wordmark settle into Glossaray's own short synthesized texture.
  // Startup audio is deliberately best-effort and never blocks recovery.
  GlossarayAudio::playBootSound();
  GlossarayDisplay::showBootStatus("CONNECTING", "SAVED NETWORK");
  const GlossarayNetwork::ConfigResult configResult =
      GlossarayNetwork::configure(false, showSetupPortal);
  GlossarayDisplay::setHardwareLongPressPowerOff(false);
  if (configResult == GlossarayNetwork::ConfigResult::HotspotHandoff) {
    GlossarayDisplay::showBootStatus("WI-FI SAVED", "HOTSPOT THEN RESET");
    Serial.println("Glossaray: one-phone hotspot handoff ready; waiting for RESET.");
  } else if (configResult != GlossarayNetwork::ConfigResult::Ready) {
    GlossarayDisplay::showBootStatus("WI-FI ERROR", "TAP v TO RETRY");
    Serial.println("Glossaray network setup incomplete; display and serial remain available.");
  } else {
    GlossarayDisplay::showBootStatus("CONNECTING", "CHECKING SERVICE");
    if (GlossarayNetwork::checkHealth()) {
      GlossarayDisplay::showBootStatus("READY", pairLabel().c_str());
      delay(800);
      GlossarayDisplay::finishBoot();
      bootSceneVisible = false;
      showReady();
      applicationReady = true;
      markActivity();
    } else {
      GlossarayDisplay::showBootStatus("READY", "NETWORK DEGRADED");
      Serial.println("Glossaray health probe failed; translation remains available.");
      delay(1400);
      GlossarayDisplay::finishBoot();
      bootSceneVisible = false;
      showReady();
      applicationReady = true;
      markActivity();
    }
  }
}

void loop() {
  GlossarayDisplay::refreshBatteryIndicator();
  const GlossarayDisplay::PowerEvent powerEvent = GlossarayDisplay::pollPowerEvent();
  if (!applicationReady) {
    if (powerEvent == GlossarayDisplay::PowerEvent::Long) {
      GlossarayDisplay::showStatus("POWERING OFF", "PRESS v TO WAKE");
      delay(700);
      GlossarayDisplay::powerOff();
    } else if (powerEvent == GlossarayDisplay::PowerEvent::Short ||
               powerEvent == GlossarayDisplay::PowerEvent::Double) {
      enterSetupPortal();
    } else {
      delay(20);
    }
    return;
  }

  if (powerEvent != GlossarayDisplay::PowerEvent::None) {
    handlePowerEvent(powerEvent);
    return;
  }

  if (digitalRead(kTalkButtonPin) != LOW) {
    if (GlossarayDisplay::isExternalPowerPresent()) {
      // A powered desk session must never become immediately eligible when the
      // cable is removed; inactivity begins at unplug, not at boot.
      markActivity();
    } else if (lastActivityAt != 0 &&
               millis() - lastActivityAt >= kIdlePowerOffMs) {
      Serial.println("Glossaray: five-minute battery inactivity timeout.");
      GlossarayDisplay::showStatus("SLEEPING", "PRESS v TO WAKE");
      delay(700);
      GlossarayDisplay::powerOff();
      return;
    }
    // Open sessions on demand. Preparing an unused provider session would
    // spend a session-start allowance without translating any speech.
    GlossarayDisplay::tickTranslation();
    delay(20);
    return;
  }

  markActivity();
  const unsigned long pressedAt = millis();
  while (digitalRead(kTalkButtonPin) == LOW &&
         millis() - pressedAt < 350) {
    delay(10);
  }
  if (digitalRead(kTalkButtonPin) != LOW && lastTranslation.wav != nullptr) {
    const String label = pairLabel();
    const String powerAction = resultPowerAction();
    GlossarayDisplay::showTranslation(label.c_str(), lastTranslation.text, "REPEATING");
    GlossarayAudio::playWav(lastTranslation.wav, lastTranslation.wavBytes);
    GlossarayDisplay::showTranslation(label.c_str(), lastTranslation.text,
                                  "^ TAP REPEAT / HOLD NEW",
                                  powerAction.c_str());
    markActivity();
    delay(250);
    return;
  }

  const String label = pairLabel();
  GlossarayNetwork::TranslationResult next{};
  GlossarayDisplay::showStatus("LISTENING", "RELEASE WHEN DONE");
  const bool translated = GlossarayGeminiStream::translateWhileHeld(
      kTalkButtonPin, sourceCode(), targetCode(), &next,
      showGeminiFinishing, label.c_str());
  if (!translated) {
    showTranslationFailure();
    delay(1800);
    showReady();
    return;
  }

  GlossarayNetwork::freeTranslation(&lastTranslation);
  lastTranslation = next;
  const String powerAction = resultPowerAction();
  GlossarayDisplay::showTranslation(label.c_str(), lastTranslation.text, "SPEAKING");
  if (!GlossarayAudio::playWav(lastTranslation.wav, lastTranslation.wavBytes)) {
    GlossarayDisplay::showStatus("ERROR", "PLAYBACK FAILED");
    return;
  }
  GlossarayDisplay::showTranslation(label.c_str(), lastTranslation.text,
                                "^ TAP REPEAT / HOLD NEW",
                                powerAction.c_str());
  markActivity();
  while (digitalRead(kTalkButtonPin) == LOW) delay(10);
  delay(250);
}
