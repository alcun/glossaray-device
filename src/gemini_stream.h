#pragma once

#include "glossaray_network.h"

namespace GlossarayGeminiStream {

// Open the authenticated Glossaray session and wait for readiness. This
// probe sends no microphone audio and closes after the connection check.
bool probe(const char* source, const char* target);

// Quietly prepare the session while the ready screen is idle.
bool prepare(const char* source, const char* target);

// Prepare without blocking the physical button loop.
void prepareAsync(const char* source, const char* target);

// Stream bounded microphone PCM through the same Glossaray protocol as the web
// client and return its transcript and 24kHz response as a device-ready WAV.
using ReleasedCallback = void (*)(const void* context);
bool translateWhileHeld(int buttonPin, const char* source, const char* target,
                        GlossarayNetwork::TranslationResult* result,
                        ReleasedCallback onReleased = nullptr,
                        const void* releasedContext = nullptr);
const char* diagnosticSummary();

// The server's own word for why a turn was refused - "busy", "rate_limited"
// and the rest - or empty when the turn failed without one. Empty is the
// normal case for a timeout or a dropped socket: those never got an answer.
const char* lastErrorCode();

}  // namespace GlossarayGeminiStream
