#pragma once

#include <stddef.h>
#include <stdint.h>

namespace GlossarayNetwork {

enum class ConfigResult { Ready, HotspotHandoff, Failed };
using PortalStartedCallback = void (*)();

struct TranslationResult {
  char* text;
  size_t textBytes;
  uint8_t* wav;
  size_t wavBytes;
};

// Connect using saved WiFi or open GLOSSARAY-SETUP. API health apiUrl and
// bearer token are stored only in the board's "glossaray" NVS namespace.
ConfigResult configure(bool forcePortal = false,
                       PortalStartedCallback onPortalStarted = nullptr);

// Perform one TLS-verified authenticated health request.
bool checkHealth();

// Read-only connection material for the shared authenticated Glossaray socket.
const char* apiOrigin();
const char* deviceCredential();
const char* tlsRoot();

// The turn's text and audio, filled by the Glossaray socket and released here
// because that is where they were allocated.
void freeTranslation(TranslationResult* result);

const char* diagnosticSummary();

}  // namespace GlossarayNetwork
