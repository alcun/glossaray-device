#pragma once

namespace GlossarayDisplay {

enum class PowerEvent { None, Short, Double, Long };

// Initialize the V2 AXP2101/CO5300 foundation and draw the static first-light
// composition. Returns false if panel initialization fails.
bool begin();

// Keep one branded boot composition on screen while connection work proceeds.
// The fish bobs independently; callers only replace the two status lines.
void showBootStatus(const char* title, const char* detail);
void finishBoot();

// Refresh the quiet top-right battery/charge glyph at a bounded cadence.
void refreshBatteryIndicator();

// True while AXP2101 reports valid external VBUS power.
bool isExternalPowerPresent();

// Display a two-line status message.
// The second detail line is for the one screen that has to say what to DO
// rather than what happened; everything else stays a sparse two-line state.
void showStatus(const char* title, const char* detail,
                const char* secondDetail = nullptr);

// Stable ready canvas: update only the pair/preview region while cycling.
void showReady(const char* pair, const char* nextLanguage);

// Show returned UTF-8 translation text with script-aware wrapping.
void showTranslation(const char* pair, const char* text, const char* footer,
                     const char* secondFooter = nullptr);

// Advance the result pane when a translation is longer than it holds. Call it
// from the idle loop; it does nothing for a translation that already fits, and
// nothing on any other scene.
void tickTranslation();

// Diagnostic only: no language, swap, or shutdown action is assigned.
PowerEvent pollPowerEvent();
void powerOff();

// Let the PMIC itself handle a long lower-button press while blocking setup
// code prevents the normal event loop from running.
void setHardwareLongPressPowerOff(bool enabled);

}  // namespace GlossarayDisplay
