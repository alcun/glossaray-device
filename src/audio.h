#pragma once

#include <stddef.h>
#include <stdint.h>

namespace GlossarayAudio {

using PcmChunkCallback = bool (*)(const int16_t* samples, size_t frames,
                                  void* context);

// Play Glossaray's short, original synthesized startup texture. Failure is
// non-fatal so audio can never prevent the device from continuing to boot.
bool playBootSound();

// Sound a cue, capture while the active-low talk button remains held (up to ten
// seconds), then resample the proven 24kHz stereo input to a 16kHz mono signed
// 16-bit PCM WAV in PSRAM. Caller frees with heap_caps_free().
bool recordWav16kMonoWhileHeld(int buttonPin, uint8_t** wav, size_t* wavBytes);

// Capture through the proven ES8311 path and deliver bounded 100ms chunks as
// 16kHz mono signed-16-bit PCM while the active-low button remains held.
bool streamPcm16kMonoWhileHeld(int buttonPin, PcmChunkCallback callback,
                              void* context, size_t* totalFrames);

// Sound a cue, capture three seconds from the onboard microphone, then play
// the captured stereo PCM through the onboard speaker.
bool runMicrophoneLoopback();

// Validate and play a 24kHz stereo signed 16-bit PCM WAV from memory.
bool playWav(const uint8_t* wav, size_t wavBytes);

// Persistent summary included in the repeating serial hardware report.
const char* diagnosticSummary();

}  // namespace GlossarayAudio
