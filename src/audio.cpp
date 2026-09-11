// Audio capture and playback for the Waveshare V2 board.
//
// Pin mapping and audio orchestration are selectively adapted from:
//   steveruizok/chat-stick
//   commit 3321c9bfc9771ee8b3adc4815f6c72890d3db125
//   MIT License, Copyright (c) 2026 Steve Ruiz
//
// The amplifier polarity and stereo slot mode are confirmed by Waveshare's
// official V2 Arduino example at commit
// ba32b5cbca96f0e04b0736d04959b6e832268d3f (Apache-2.0).
//
// The minimal ES8311 register sequence is derived from Espressif's
// Apache-2.0 ES8311 driver retained by that pinned reference:
//   SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
//   SPDX-License-Identifier: Apache-2.0

#include "audio.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <math.h>

namespace {

constexpr uint8_t kCodecAddress = 0x18;
constexpr int kMclkPin = 16;
constexpr int kBclkPin = 9;
constexpr int kCodecDataOutPin = 10;
constexpr int kWordSelectPin = 45;
constexpr int kCodecDataInPin = 8;
constexpr int kAmplifierEnablePin = 46;
constexpr int kSampleRate = 24000;
constexpr int kChunkFrames = 240;
constexpr int kPlaybackPrerollFrames = kSampleRate / 10;
constexpr int kCaptureSeconds = 3;
constexpr size_t kCaptureBytes =
    kSampleRate * kCaptureSeconds * 2 * sizeof(int16_t);
constexpr int kUploadSampleRate = 16000;
constexpr int kMaxTalkSeconds = 10;
constexpr size_t kMaxTalkCaptureBytes =
    kSampleRate * kMaxTalkSeconds * 2 * sizeof(int16_t);
constexpr size_t kMaxUploadFrames = kUploadSampleRate * kMaxTalkSeconds;
constexpr size_t kMaxUploadWavBytes =
    44 + kMaxUploadFrames * sizeof(int16_t);
constexpr size_t kMinimumTalkFrames = kSampleRate / 2;

I2SClass i2s;
const char* audioSummary = "not run";

bool writeCodec(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kCodecAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readCodec(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(kCodecAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(kCodecAddress, static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  value = Wire.read();
  return true;
}

bool updateCodec(uint8_t reg, uint8_t clearMask, uint8_t setMask) {
  uint8_t value = 0;
  return readCodec(reg, value) &&
         writeCodec(reg, static_cast<uint8_t>((value & clearMask) | setMask));
}

bool configureCodec() {
  uint8_t chipId1 = 0;
  uint8_t chipId2 = 0;
  uint8_t chipVersion = 0;
  if (!readCodec(0xFD, chipId1) || !readCodec(0xFE, chipId2) ||
      !readCodec(0xFF, chipVersion)) {
    audioSummary = "ES8311 did not answer at I2C 0x18";
    return false;
  }
  Serial.printf("Audio: ES8311 IDs=%02X/%02X version=%02X.\n", chipId1,
                chipId2, chipVersion);
  audioSummary = "ES8311 register configuration failed";

  // Reset and power on.
  if (!writeCodec(0x00, 0x1F) || !writeCodec(0x00, 0x00) ||
      !writeCodec(0x00, 0x80)) {
    return false;
  }

  // 6.144MHz MCLK, 24kHz sample rate, 16-bit stereo I2S slave.
  if (!writeCodec(0x01, 0x3F) ||
      !updateCodec(0x02, 0x07, 0x00) ||
      !writeCodec(0x03, 0x10) ||
      !writeCodec(0x04, 0x10) ||
      !writeCodec(0x05, 0x00) ||
      !updateCodec(0x06, 0xE0, 0x03) ||
      !updateCodec(0x07, 0xC0, 0x00) ||
      !writeCodec(0x08, 0xFF) ||
      !updateCodec(0x00, 0xBF, 0x00) ||
      !writeCodec(0x09, 0x0C) ||
      !writeCodec(0x0A, 0x0C)) {
    return false;
  }

  // Power the DAC/output path, bypass EQ, set conservative volume and unmute.
  return writeCodec(0x0D, 0x01) &&
         writeCodec(0x0E, 0x02) &&
         writeCodec(0x12, 0x00) &&
         writeCodec(0x13, 0x10) &&
         writeCodec(0x14, 0x1A) &&
         writeCodec(0x16, 0x03) &&
         writeCodec(0x17, 0xC8) &&
         writeCodec(0x1C, 0x6A) &&
         writeCodec(0x37, 0x08) &&
         writeCodec(0x32, 0xC0) &&
         updateCodec(0x31, 0x9F, 0x00);
}

bool playTone(int frequency, int durationMs) {
  const int totalFrames = kSampleRate * durationMs / 1000;
  int16_t samples[kChunkFrames * 2];
  float phase = 0.0f;
  const float phaseStep =
      2.0f * static_cast<float>(PI) * frequency / kSampleRate;

  for (int frame = 0; frame < totalFrames;) {
    const int frames = min(kChunkFrames, totalFrames - frame);
    for (int index = 0; index < frames; ++index) {
      const int absoluteFrame = frame + index;
      const int fadeFrames = kSampleRate / 50;
      const float fadeIn =
          min(1.0f, static_cast<float>(absoluteFrame) / fadeFrames);
      const float fadeOut = min(
          1.0f, static_cast<float>(totalFrames - absoluteFrame - 1) /
                    fadeFrames);
      const float envelope = min(fadeIn, fadeOut);
      const int16_t sample =
          static_cast<int16_t>(sinf(phase) * 12000.0f * envelope);
      samples[index * 2] = sample;
      samples[index * 2 + 1] = sample;
      phase += phaseStep;
      if (phase >= 2.0f * static_cast<float>(PI)) {
        phase -= 2.0f * static_cast<float>(PI);
      }
    }
    const size_t bytes = frames * 2 * sizeof(int16_t);
    if (i2s.write(reinterpret_cast<const uint8_t*>(samples), bytes) != bytes) {
      return false;
    }
    frame += frames;
  }
  return true;
}

bool writePlaybackPreroll() {
  int16_t silence[kChunkFrames * 2] = {};
  int writtenFrames = 0;
  while (writtenFrames < kPlaybackPrerollFrames) {
    const int frames = min(kChunkFrames,
                           kPlaybackPrerollFrames - writtenFrames);
    const size_t bytes = frames * 2 * sizeof(int16_t);
    if (i2s.write(reinterpret_cast<const uint8_t*>(silence), bytes) != bytes) {
      return false;
    }
    writtenFrames += frames;
  }
  return true;
}

uint16_t get16(const uint8_t* value) {
  return value[0] | (static_cast<uint16_t>(value[1]) << 8);
}

uint32_t get32(const uint8_t* value) {
  return value[0] | (static_cast<uint32_t>(value[1]) << 8) |
         (static_cast<uint32_t>(value[2]) << 16) |
         (static_cast<uint32_t>(value[3]) << 24);
}

bool findPcm(const uint8_t* wav, size_t wavBytes, const uint8_t** pcm,
             size_t* pcmBytes) {
  if (wavBytes < 44 || memcmp(wav, "RIFF", 4) != 0 ||
      memcmp(wav + 8, "WAVE", 4) != 0) {
    return false;
  }
  bool formatOk = false;
  size_t offset = 12;
  while (offset + 8 <= wavBytes) {
    const uint32_t chunkBytes = get32(wav + offset + 4);
    const size_t dataOffset = offset + 8;
    if (dataOffset + chunkBytes > wavBytes) {
      return false;
    }
    if (memcmp(wav + offset, "fmt ", 4) == 0 && chunkBytes >= 16) {
      formatOk = get16(wav + dataOffset) == 1 &&
                 get16(wav + dataOffset + 2) == 2 &&
                 get32(wav + dataOffset + 4) == kSampleRate &&
                 get16(wav + dataOffset + 14) == 16;
    } else if (memcmp(wav + offset, "data", 4) == 0 && formatOk) {
      *pcm = wav + dataOffset;
      *pcmBytes = chunkBytes;
      return chunkBytes > 0 && chunkBytes % 4 == 0;
    }
    offset = dataOffset + chunkBytes + (chunkBytes & 1);
  }
  return false;
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

void writeUploadWavHeader(uint8_t* wav, size_t frames) {
  const size_t wavBytes = 44 + frames * sizeof(int16_t);
  memcpy(wav, "RIFF", 4);
  put32(wav + 4, wavBytes - 8);
  memcpy(wav + 8, "WAVEfmt ", 8);
  put32(wav + 16, 16);
  put16(wav + 20, 1);
  put16(wav + 22, 1);
  put32(wav + 24, kUploadSampleRate);
  put32(wav + 28, kUploadSampleRate * sizeof(int16_t));
  put16(wav + 32, sizeof(int16_t));
  put16(wav + 34, 16);
  memcpy(wav + 36, "data", 4);
  put32(wav + 40, frames * sizeof(int16_t));
}

}  // namespace

namespace GlossarayAudio {

bool playBootSound() {
  constexpr int kSoundFrames = kSampleRate * 680 / 1000;
  int16_t samples[kChunkFrames * 2];
  float fundamentalPhase = 0.0f;
  float detunedPhase = 0.0f;

  pinMode(kAmplifierEnablePin, OUTPUT);
  digitalWrite(kAmplifierEnablePin, LOW);
  i2s.setPins(kBclkPin, kWordSelectPin, kCodecDataInPin, kCodecDataOutPin,
              kMclkPin);
  if (!i2s.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH) ||
      !configureCodec()) {
    digitalWrite(kAmplifierEnablePin, LOW);
    i2s.end();
    Serial.println("Audio: startup texture unavailable; continuing boot.");
    return false;
  }

  // Prime one DMA block before opening the amplifier to avoid a startup pop.
  int16_t silence[kChunkFrames * 2] = {};
  i2s.write(reinterpret_cast<const uint8_t*>(silence), sizeof(silence));
  digitalWrite(kAmplifierEnablePin, HIGH);

  bool complete = true;
  for (int frame = 0; frame < kSoundFrames && complete;) {
    const int frames = min(kChunkFrames, kSoundFrames - frame);
    for (int index = 0; index < frames; ++index) {
      const int absoluteFrame = frame + index;
      const float progress = static_cast<float>(absoluteFrame) / kSoundFrames;
      const float fadeIn = min(1.0f, progress * 5.0f);
      const float fadeOut = min(1.0f, (1.0f - progress) * 10.0f);
      const float envelope = fadeIn * fadeOut;

      // Small-speaker CRT ignition: stay inside the physically audible range
      // and create the apparent rise through amplitude/harmonic bloom, not a
      // conspicuous pitch sweep. The previous sub-126Hz candidate was
      // physically inaudible and reduced to the amplifier switching transient.
      const float fundamentalHz = 205.0f + 18.0f * progress;
      fundamentalPhase +=
          2.0f * static_cast<float>(PI) * fundamentalHz / kSampleRate;
      detunedPhase += 2.0f * static_cast<float>(PI) *
                      (fundamentalHz * 1.018f) / kSampleRate;
      const float pulse = 0.84f + 0.16f * sinf(progress * 30.0f * PI);
      const float harmonicBloom = min(1.0f, progress * 2.4f);
      const float buzz = sinf(fundamentalPhase) * 3900.0f +
                         sinf(detunedPhase) * 1800.0f +
                         sinf(fundamentalPhase * 2.0f) *
                             1350.0f * harmonicBloom;
      // A tiny harmonic release supplies the final "sht" without broadband
      // noise, which the physical speaker previously rendered as static.
      const float tail = progress > 0.86f
                             ? sinf(fundamentalPhase * 2.0f) *
                                   1000.0f * ((progress - 0.86f) / 0.14f)
                             : 0.0f;
      const int16_t sample =
          static_cast<int16_t>((buzz + tail) * envelope * pulse);
      samples[index * 2] = sample;
      samples[index * 2 + 1] = sample;
    }
    const size_t bytes = frames * 2 * sizeof(int16_t);
    complete =
        i2s.write(reinterpret_cast<const uint8_t*>(samples), bytes) == bytes;
    frame += frames;
  }
  delay(70);
  digitalWrite(kAmplifierEnablePin, LOW);
  i2s.end();
  Serial.println(
      complete ? "Audio: Glossaray startup texture played."
               : "Audio: startup texture incomplete; continuing boot.");
  return complete;
}

bool streamPcm16kMonoWhileHeld(int buttonPin, PcmChunkCallback callback,
                              void* context, size_t* totalFrames) {
  if (callback == nullptr || totalFrames == nullptr) return false;
  *totalFrames = 0;
  constexpr size_t kInputFrames = 2400;  // 100ms at 24kHz.
  constexpr size_t kOutputFrames = 1600; // 100ms at 16kHz.
  constexpr size_t kInputBytes = kInputFrames * 2 * sizeof(int16_t);
  auto* input = static_cast<int16_t*>(
      heap_caps_malloc(kInputBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto* mono = static_cast<int16_t*>(heap_caps_malloc(
      kOutputFrames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (input == nullptr || mono == nullptr) {
    heap_caps_free(input);
    heap_caps_free(mono);
    audioSummary = "PSRAM allocation failed for streaming capture";
    return false;
  }

  pinMode(kAmplifierEnablePin, OUTPUT);
  digitalWrite(kAmplifierEnablePin, HIGH);
  i2s.setPins(kBclkPin, kWordSelectPin, kCodecDataInPin, kCodecDataOutPin,
              kMclkPin);
  if (!i2s.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH) ||
      !configureCodec() || !playTone(1000, 180)) {
    heap_caps_free(input);
    heap_caps_free(mono);
    digitalWrite(kAmplifierEnablePin, LOW);
    i2s.end();
    audioSummary = "streaming microphone initialization failed";
    return false;
  }
  delay(150);
  digitalWrite(kAmplifierEnablePin, LOW);
  delay(100);

  bool complete = true;
  size_t capturedInputFrames = 0;
  while (digitalRead(buttonPin) == LOW &&
         capturedInputFrames < static_cast<size_t>(kSampleRate * kMaxTalkSeconds)) {
    const size_t bytes = i2s.readBytes(reinterpret_cast<char*>(input), kInputBytes);
    const size_t inputFrames = bytes / (2 * sizeof(int16_t));
    if (inputFrames == 0) {
      complete = false;
      break;
    }
    const size_t outputFrames = inputFrames * kUploadSampleRate / kSampleRate;
    for (size_t outputFrame = 0; outputFrame < outputFrames; ++outputFrame) {
      const size_t inputFrame = outputFrame * kSampleRate / kUploadSampleRate;
      const int32_t left = input[inputFrame * 2];
      const int32_t right = input[inputFrame * 2 + 1];
      mono[outputFrame] = static_cast<int16_t>((left + right) / 2);
    }
    if (!callback(mono, outputFrames, context)) {
      complete = false;
      break;
    }
    capturedInputFrames += inputFrames;
    *totalFrames += outputFrames;
  }
  i2s.end();
  heap_caps_free(input);
  heap_caps_free(mono);
  if (*totalFrames < static_cast<size_t>(kUploadSampleRate / 2)) {
    audioSummary = "streaming hold-to-talk recording too short";
    return false;
  }
  audioSummary = complete ? "streaming 16kHz mono microphone PCM captured"
                          : "streaming microphone callback failed";
  Serial.printf("Audio: streamed %u mono PCM frames.\n",
                static_cast<unsigned>(*totalFrames));
  return complete;
}

bool recordWav16kMonoWhileHeld(int buttonPin, uint8_t** wav,
                              size_t* wavBytes) {
  if (wav == nullptr || wavBytes == nullptr) {
    return false;
  }
  *wav = nullptr;
  *wavBytes = 0;

  auto* capture = static_cast<int16_t*>(
      heap_caps_malloc(kMaxTalkCaptureBytes,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto* upload = static_cast<uint8_t*>(
      heap_caps_malloc(kMaxUploadWavBytes,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (capture == nullptr || upload == nullptr) {
    heap_caps_free(capture);
    heap_caps_free(upload);
    audioSummary = "PSRAM allocation failed for microphone upload";
    return false;
  }

  pinMode(kAmplifierEnablePin, OUTPUT);
  digitalWrite(kAmplifierEnablePin, HIGH);
  i2s.setPins(kBclkPin, kWordSelectPin, kCodecDataInPin, kCodecDataOutPin,
              kMclkPin);
  if (!i2s.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH) ||
      !configureCodec()) {
    heap_caps_free(capture);
    heap_caps_free(upload);
    digitalWrite(kAmplifierEnablePin, LOW);
    i2s.end();
    audioSummary = "microphone upload audio initialization failed";
    return false;
  }

  Serial.println("Audio: cue — speak after the beep; release when done.");
  if (!playTone(1000, 180)) {
    heap_caps_free(capture);
    heap_caps_free(upload);
    digitalWrite(kAmplifierEnablePin, LOW);
    i2s.end();
    audioSummary = "microphone upload cue failed";
    return false;
  }
  delay(150);
  digitalWrite(kAmplifierEnablePin, LOW);
  delay(100);

  size_t captured = 0;
  constexpr size_t kReadChunkBytes = 240 * 2 * sizeof(int16_t);
  while (captured + kReadChunkBytes <= kMaxTalkCaptureBytes &&
         digitalRead(buttonPin) == LOW) {
    const size_t bytes = i2s.readBytes(
        reinterpret_cast<char*>(capture) + captured, kReadChunkBytes);
    if (bytes == 0) break;
    captured += bytes;
  }
  i2s.end();
  const size_t capturedFrames = captured / (2 * sizeof(int16_t));
  if (capturedFrames < kMinimumTalkFrames) {
    heap_caps_free(capture);
    heap_caps_free(upload);
    audioSummary = "hold-to-talk recording too short";
    return false;
  }

  const size_t uploadFrames =
      capturedFrames * kUploadSampleRate / kSampleRate;
  const size_t uploadBytes = 44 + uploadFrames * sizeof(int16_t);
  writeUploadWavHeader(upload, uploadFrames);
  auto* mono = reinterpret_cast<int16_t*>(upload + 44);
  for (size_t outputFrame = 0; outputFrame < uploadFrames; ++outputFrame) {
    const size_t inputFrame = outputFrame * kSampleRate / kUploadSampleRate;
    const int32_t left = capture[inputFrame * 2];
    const int32_t right = capture[inputFrame * 2 + 1];
    mono[outputFrame] = static_cast<int16_t>((left + right) / 2);
  }
  heap_caps_free(capture);

  *wav = upload;
  *wavBytes = uploadBytes;
  audioSummary = "hold-to-talk 16kHz mono microphone WAV captured";
  Serial.printf("Audio: microphone upload WAV ready, %u bytes.\n",
                static_cast<unsigned>(uploadBytes));
  return true;
}

bool playWav(const uint8_t* wav, size_t wavBytes) {
  const uint8_t* pcm = nullptr;
  size_t pcmBytes = 0;
  if (!findPcm(wav, wavBytes, &pcm, &pcmBytes)) {
    audioSummary = "returned WAV format invalid";
    return false;
  }

  pinMode(kAmplifierEnablePin, OUTPUT);
  digitalWrite(kAmplifierEnablePin, LOW);
  i2s.setPins(kBclkPin, kWordSelectPin, kCodecDataInPin, kCodecDataOutPin,
              kMclkPin);
  if (!i2s.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH) ||
      !configureCodec()) {
    audioSummary = "returned WAV audio initialization failed";
    i2s.end();
    return false;
  }

  digitalWrite(kAmplifierEnablePin, HIGH);
  delay(100);
  if (!writePlaybackPreroll()) {
    digitalWrite(kAmplifierEnablePin, LOW);
    i2s.end();
    audioSummary = "returned WAV playback preroll failed";
    return false;
  }
  size_t played = 0;
  while (played < pcmBytes) {
    const size_t bytes = i2s.write(pcm + played, pcmBytes - played);
    if (bytes == 0) break;
    played += bytes;
  }
  delay(250);
  digitalWrite(kAmplifierEnablePin, LOW);
  i2s.end();
  const bool complete = played == pcmBytes;
  audioSummary = complete ? "returned Welsh WAV playback completed"
                          : "returned Welsh WAV playback incomplete";
  Serial.printf("Audio: returned WAV %u/%u PCM bytes played.\n",
                static_cast<unsigned>(played),
                static_cast<unsigned>(pcmBytes));
  return complete;
}

bool runMicrophoneLoopback() {
  audioSummary = "initializing";
  Serial.println("Audio: initializing ES8311.");
  pinMode(kAmplifierEnablePin, OUTPUT);
  digitalWrite(kAmplifierEnablePin, HIGH);

  i2s.setPins(kBclkPin, kWordSelectPin, kCodecDataInPin, kCodecDataOutPin,
              kMclkPin);
  if (!i2s.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    audioSummary = "I2S initialization failed";
    Serial.println("Audio: I2S initialization failed.");
    return false;
  }
  if (!configureCodec()) {
    Serial.println("Audio: ES8311 configuration failed.");
    i2s.end();
    return false;
  }

  auto* capture = static_cast<uint8_t*>(
      heap_caps_malloc(kCaptureBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (capture == nullptr) {
    audioSummary = "PSRAM allocation failed for microphone capture";
    digitalWrite(kAmplifierEnablePin, LOW);
    i2s.end();
    return false;
  }

  // One short cue marks the exact start of the independently testable capture.
  delay(500);
  Serial.println("Audio: cue — speak for three seconds after the beep.");
  if (!playTone(1000, 180)) {
    audioSummary = "I2S cue write failed";
    heap_caps_free(capture);
    digitalWrite(kAmplifierEnablePin, LOW);
    i2s.end();
    return false;
  }
  delay(150);
  digitalWrite(kAmplifierEnablePin, LOW);
  delay(100);

  Serial.println("Audio: recording microphone for three seconds.");
  size_t captured = 0;
  while (captured < kCaptureBytes) {
    const size_t bytes =
        i2s.readBytes(reinterpret_cast<char*>(capture + captured),
                      kCaptureBytes - captured);
    if (bytes == 0) {
      break;
    }
    captured += bytes;
  }
  Serial.printf("Audio: captured %u/%u bytes; playing back.\n",
                static_cast<unsigned>(captured),
                static_cast<unsigned>(kCaptureBytes));

  digitalWrite(kAmplifierEnablePin, HIGH);
  delay(100);
  size_t played = 0;
  while (played < captured) {
    const size_t bytes = i2s.write(capture + played, captured - played);
    if (bytes == 0) {
      break;
    }
    played += bytes;
  }
  delay(250);
  digitalWrite(kAmplifierEnablePin, LOW);
  i2s.end();
  heap_caps_free(capture);
  audioSummary =
      (captured == kCaptureBytes && played == captured)
          ? "ES8311 microphone capture and local playback completed"
          : "microphone capture or playback was incomplete";
  Serial.printf("Audio: loopback complete (%u captured, %u played).\n",
                static_cast<unsigned>(captured),
                static_cast<unsigned>(played));
  return captured == kCaptureBytes && played == captured;
}

const char* diagnosticSummary() {
  return audioSummary;
}

}  // namespace GlossarayAudio
