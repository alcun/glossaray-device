#include "gemini_stream.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio.h"
#include "glossaray_network.h"

namespace {

constexpr unsigned long kConnectTimeoutMs = 12000;
// A reply is spoken, so it arrives in something close to real time and can be
// longer than the phrase that prompted it: a 7.5s hold came back as 9.3s of
// 24kHz audio. Eight seconds cut that off mid-sentence and scored a good turn
// as a failure. This is the whole hold plus room for the reply to outrun it.
constexpr unsigned long kResponseTimeoutMs = 25000;
constexpr unsigned long kWarmReconnectBackoffMs = 5000;
constexpr size_t kMaxOutputPcmBytes = 18 * 24000 * sizeof(int16_t);
constexpr size_t kMaxTranscriptBytes = 4096;
constexpr size_t kAudioChunkFrames = 1600;

websockets::WebsocketsClient socket;
bool connected = false;
bool ready = false;
bool failed = false;
bool turnComplete = false;
const char* summary = "not run";
uint8_t* outputPcm = nullptr;
char* transcript = nullptr;
size_t outputPcmBytes = 0;
size_t transcriptBytes = 0;
size_t responseMessages = 0;
char sessionSource[8]{};
char sessionTarget[8]{};
char pendingSource[8]{};
char pendingTarget[8]{};
char errorCode[24]{};
bool authorizationHeaderAdded = false;
unsigned long lastPrepareAttempt = 0;
volatile bool prepareRunning = false;
TaskHandle_t prepareTask = nullptr;

void appendTranscript(const char* text) {
  if (text == nullptr || transcript == nullptr) return;
  const size_t available = kMaxTranscriptBytes - 1 - transcriptBytes;
  const size_t bytes = min(strlen(text), available);
  memcpy(transcript + transcriptBytes, text, bytes);
  transcriptBytes += bytes;
  transcript[transcriptBytes] = '\0';
}

void onMessage(websockets::WebsocketsMessage message) {
  ++responseMessages;
  const auto& payload = message.rawData();
  if (message.isBinary()) {
    if (outputPcm == nullptr || outputPcmBytes + payload.size() > kMaxOutputPcmBytes) {
      failed = true;
      summary = "Glossaray audio exceeded playback bound";
      return;
    }
    memcpy(outputPcm + outputPcmBytes, payload.data(), payload.size());
    outputPcmBytes += payload.size();
    return;
  }
  if (!message.isText()) return;
  JsonDocument document;
  if (deserializeJson(document, payload.data(), payload.size()) !=
      DeserializationError::Ok) {
    failed = true;
    summary = "Glossaray control message invalid";
    return;
  }
  const char* type = document["type"] | "";
  if (strcmp(type, "hello") == 0) {
    JsonDocument start;
    start["type"] = "start";
    start["source"] = sessionSource;
    start["target"] = sessionTarget;
    start["mode"] = "agent";
    String body;
    serializeJson(start, body);
    if (!socket.send(body)) failed = true;
  } else if (strcmp(type, "ready") == 0) {
    ready = true;
  } else if (strcmp(type, "said") == 0) {
    appendTranscript(document["text"] | "");
  } else if (strcmp(type, "turn_complete") == 0) {
    turnComplete = true;
  } else if (strcmp(type, "error") == 0) {
    failed = true;
    strlcpy(errorCode, document["code"] | "", sizeof(errorCode));
    Serial.printf("Glossaray stream: server refused the session (%s).\n",
                  errorCode[0] == '\0' ? "unspecified" : errorCode);
    summary = "Glossaray server refused the session";
  }
}

void onEvent(websockets::WebsocketsEvent event, String data) {
  if (event == websockets::WebsocketsEvent::ConnectionOpened) {
    connected = true;
  } else if (event == websockets::WebsocketsEvent::ConnectionClosed) {
    if (!turnComplete) failed = true;
    connected = false;
    Serial.printf("Glossaray stream: socket closed (%u bytes).\n",
                  static_cast<unsigned>(data.length()));
  }
}

void resetSessionState() {
  errorCode[0] = '\0';
  connected = false;
  ready = false;
  failed = false;
  turnComplete = false;
  outputPcmBytes = 0;
  transcriptBytes = 0;
  responseMessages = 0;
}

bool openSession(const char* source, const char* target) {
  if (socket.available()) socket.poll();
  if (socket.available() && ready && !failed &&
      strcmp(sessionSource, source) == 0 && strcmp(sessionTarget, target) == 0) {
    return true;
  }
  if (socket.available()) socket.close();
  resetSessionState();
  strlcpy(sessionSource, source, sizeof(sessionSource));
  strlcpy(sessionTarget, target, sizeof(sessionTarget));

  String origin = GlossarayNetwork::apiOrigin();
  if (!origin.startsWith("https://")) {
    summary = "Glossaray server must use HTTPS";
    return false;
  }
  origin.remove(0, 8);
  const int slash = origin.indexOf('/');
  if (slash >= 0) origin.remove(slash);

  socket.onMessage(onMessage);
  socket.onEvent(onEvent);
  socket.setCACert(GlossarayNetwork::tlsRoot());
  // ONCE, and never again for the life of the board. addHeader appends to a
  // vector the library never clears, and the header survives on the client, so
  // adding it per connect sends Authorization twice on the second handshake.
  // Bun joins duplicates with ", ", the constant-time compare sees
  // "Bearer x, Bearer x", and the upgrade is refused with a 403 - no socket, so
  // no error frame either, which is why this looks like a silent close.
  if (!authorizationHeaderAdded) {
    socket.addHeader("Authorization",
                     String("Bearer ") + GlossarayNetwork::deviceCredential());
    authorizationHeaderAdded = true;
  }
  Serial.println("Glossaray stream: opening shared authenticated socket.");
  connected = socket.connectSecure(origin, 443, "/ws");
  const unsigned long started = millis();
  while (!ready && !failed && millis() - started < kConnectTimeoutMs) {
    socket.poll();
    delay(2);
  }
  if (!ready) {
    failed = true;
    if (socket.available()) socket.close();
  }
  return ready && connected && !failed;
}

bool sendPcm(const int16_t* samples, size_t frames, void*) {
  if (failed || frames == 0 || frames > kAudioChunkFrames) return false;
  const size_t bytes = frames * sizeof(int16_t);
  // Not the String overload: it converts through c_str(), so the PCM would be
  // cut at the first zero byte and silence is 0x0000.
  const bool sent =
      socket.sendBinary(reinterpret_cast<const char*>(samples), bytes);
  socket.poll();
  return sent && !failed;
}

void freeTurnBuffers() {
  heap_caps_free(outputPcm);
  heap_caps_free(transcript);
  outputPcm = nullptr;
  transcript = nullptr;
}

void put16(uint8_t* target, uint16_t value) {
  target[0] = value & 0xff;
  target[1] = value >> 8;
}

void put32(uint8_t* target, uint32_t value) {
  target[0] = value & 0xff;
  target[1] = (value >> 8) & 0xff;
  target[2] = (value >> 16) & 0xff;
  target[3] = value >> 24;
}

void writeStereoWavHeader(uint8_t* wav, size_t pcmBytes) {
  memcpy(wav, "RIFF", 4);
  put32(wav + 4, 36 + pcmBytes * 2);
  memcpy(wav + 8, "WAVEfmt ", 8);
  put32(wav + 16, 16);
  put16(wav + 20, 1);
  put16(wav + 22, 2);
  put32(wav + 24, 24000);
  put32(wav + 28, 24000 * 2 * sizeof(int16_t));
  put16(wav + 32, 2 * sizeof(int16_t));
  put16(wav + 34, 16);
  memcpy(wav + 36, "data", 4);
  put32(wav + 40, pcmBytes * 2);
}

void runPrepareTask(void*) {
  const bool prepared = openSession(pendingSource, pendingTarget);
  summary = prepared ? "Glossaray session warmed" : "Glossaray session unavailable";
  prepareRunning = false;
  prepareTask = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

namespace GlossarayGeminiStream {

bool prepare(const char* source, const char* target) {
  if (socket.available() && ready && !failed &&
      strcmp(sessionSource, source) == 0 && strcmp(sessionTarget, target) == 0) {
    socket.poll();
    return true;
  }
  const unsigned long now = millis();
  if (lastPrepareAttempt != 0 && now - lastPrepareAttempt < kWarmReconnectBackoffMs) return false;
  lastPrepareAttempt = now;
  const bool prepared = openSession(source, target);
  summary = prepared ? "Glossaray session warmed" : "Glossaray session unavailable";
  return prepared;
}

void prepareAsync(const char* source, const char* target) {
  if (prepareRunning || source == nullptr || target == nullptr) return;
  if (socket.available() && ready && !failed &&
      strcmp(sessionSource, source) == 0 && strcmp(sessionTarget, target) == 0) {
    socket.poll();
    return;
  }
  const unsigned long now = millis();
  if (lastPrepareAttempt != 0 && now - lastPrepareAttempt < kWarmReconnectBackoffMs) return;
  lastPrepareAttempt = now;
  strlcpy(pendingSource, source, sizeof(pendingSource));
  strlcpy(pendingTarget, target, sizeof(pendingTarget));
  prepareRunning = true;
  if (xTaskCreatePinnedToCore(runPrepareTask, "glossaray-warm", 8192,
                              nullptr, 1, &prepareTask, 0) != pdPASS) {
    prepareRunning = false;
    prepareTask = nullptr;
  }
}

bool probe(const char* source, const char* target) {
  const bool passed = openSession(source, target);
  if (socket.available()) socket.close();
  summary = passed ? "Glossaray WebSocket setup passed" : "Glossaray WebSocket setup failed";
  return passed;
}

bool translateWhileHeld(int buttonPin, const char* source, const char* target,
                        GlossarayNetwork::TranslationResult* result,
                        ReleasedCallback onReleased, const void* releasedContext) {
  if (result == nullptr) return false;
  *result = {};
  const unsigned long waitStarted = millis();
  while (prepareRunning && millis() - waitStarted < kConnectTimeoutMs) delay(2);
  if (prepareRunning) return false;

  outputPcm = static_cast<uint8_t*>(heap_caps_malloc(
      kMaxOutputPcmBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  transcript = static_cast<char*>(heap_caps_calloc(
      kMaxTranscriptBytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  bool passed = outputPcm != nullptr && transcript != nullptr && openSession(source, target);
  if (!passed) {
    if (socket.available()) socket.close();
    freeTurnBuffers();
    summary = "Glossaray failed before microphone capture";
    return false;
  }

  size_t inputFrames = 0;
  passed = GlossarayAudio::streamPcm16kMonoWhileHeld(
      buttonPin, sendPcm, nullptr, &inputFrames);
  if (onReleased != nullptr) onReleased(releasedContext);
  if (passed) passed = socket.send("{\"type\":\"end_turn\"}");
  const unsigned long responseStarted = millis();
  while (passed && !turnComplete && !failed &&
         millis() - responseStarted < kResponseTimeoutMs) {
    socket.poll();
    delay(2);
  }
  const bool responseTimedOut = passed && !turnComplete && !failed;
  if (socket.available()) socket.close();
  lastPrepareAttempt = 0;
  passed = passed && !failed && turnComplete && outputPcmBytes >= sizeof(int16_t) &&
           outputPcmBytes % sizeof(int16_t) == 0 && transcriptBytes > 0;

  if (passed) {
    const size_t wavBytes = 44 + outputPcmBytes * 2;
    result->wav = static_cast<uint8_t*>(heap_caps_malloc(
        wavBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    result->text = static_cast<char*>(heap_caps_malloc(
        transcriptBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    passed = result->wav != nullptr && result->text != nullptr;
    if (passed) {
      writeStereoWavHeader(result->wav, outputPcmBytes);
      const auto* mono = reinterpret_cast<const int16_t*>(outputPcm);
      auto* stereo = reinterpret_cast<int16_t*>(result->wav + 44);
      const size_t samples = outputPcmBytes / sizeof(int16_t);
      for (size_t index = 0; index < samples; ++index) {
        stereo[index * 2] = mono[index];
        stereo[index * 2 + 1] = mono[index];
      }
      memcpy(result->text, transcript, transcriptBytes + 1);
      result->textBytes = transcriptBytes;
      result->wavBytes = wavBytes;
    }
  }
  if (!passed) GlossarayNetwork::freeTranslation(result);
  freeTurnBuffers();
  summary = passed ? "Glossaray translation collected"
                   : responseTimedOut ? "Glossaray response timed out"
                                      : "Glossaray response incomplete";
  Serial.printf("Glossaray stream: %s (%u input frames, %u PCM bytes, %u messages).\n",
                passed ? "passed" : "failed", static_cast<unsigned>(inputFrames),
                static_cast<unsigned>(outputPcmBytes),
                static_cast<unsigned>(responseMessages));
  return passed;
}

const char* diagnosticSummary() { return summary; }

const char* lastErrorCode() { return errorCode; }

}  // namespace GlossarayGeminiStream
