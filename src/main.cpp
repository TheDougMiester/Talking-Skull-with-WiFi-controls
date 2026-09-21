/*
 * File: main.cpp
 * Purpose: Application entry point; coordinates storage, audio playback, FFT jaw control, web commands, and eye animation.
 *
 * MIT License
 *
 * Copyright (c) 2026 Doug Brann https://github.com/TheDougMiester
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <Arduino.h>
#include <WebSocketsServer.h>
#include <new>
#include "esp_timer.h"

#include "PinDefinitions.h"
#include "ConfigManager.h"
#include "WiFiSkullController.h"
#include "AudioFFTProcessor_ESP32_Optimized.h"
#include "I2SWavPlayer.h"
#include "GC9A01_Eyes.h"
#include "SDMutex.h"

AudioFFTProcessorFast audioFFT;
I2SWavPlayer wavPlayer;
GC9A01_Eyes skullEyes;
SemaphoreHandle_t sdMutex = nullptr;
WebSocketsServer* webSocket = nullptr;

// HS-65MG jaw servo, installed upside down: minimum pulse (1505 us) holds the
// jaw FULLY OPEN, maximum pulse (1774 us) holds it FULLY CLOSED. Both
// endpoints are now mechanically confirmed and must never be exceeded, so
// audio motion uses the entire 269 us range: jawFactor 0.0 -> fully closed,
// jawFactor 1.0 -> fully open.
//
// Slew limits are asymmetric: opening is slightly softer, closing is faster
// so short consonants (the "th" in "thriller") snap the teeth together
// convincingly. Both limits are sized so the servo's own ~60 ms full-sweep
// speed -- not the firmware ramp -- dominates how fast the jaw physically
// moves, while a single spurious FFT spike can still never slam the jaw
// more than a few percent of range in one 20 ms tick.
static constexpr bool ENABLE_JAW_MOTION = true;
static constexpr uint16_t JAW_PERIOD_US = 20000;  // 50 Hz, not 2000 us
static constexpr uint16_t JAW_OPEN_US = 1505;     // fully open (reversed install)
static constexpr uint16_t JAW_CLOSED_US = 1774;   // newly calibrated fully closed
static constexpr float JAW_OPEN_LIMIT_FRACTION = 1.00f;  // full confirmed range
static constexpr uint16_t JAW_FULL_RANGE_US = JAW_CLOSED_US - JAW_OPEN_US;
static constexpr uint16_t JAW_ACTIVE_RANGE_US =
    static_cast<uint16_t>(JAW_FULL_RANGE_US * JAW_OPEN_LIMIT_FRACTION + 0.5f);
static constexpr uint16_t JAW_ACTIVE_OPEN_US =
    JAW_CLOSED_US - JAW_ACTIVE_RANGE_US;
static constexpr uint16_t JAW_MAX_OPEN_STEP_US = 20;   // per 20 ms tick
static constexpr uint16_t JAW_MAX_CLOSE_STEP_US = 28;  // per 20 ms tick
static constexpr uint16_t JAW_TARGET_DEADBAND_US = 5;
static constexpr float JAW_ATTACK_ALPHA = 0.45f;
static constexpr float JAW_RELEASE_ALPHA = 0.22f;
static constexpr float JAW_ENVELOPE_THRESHOLD = 15.0f;

static volatile bool jawTick = false;
static esp_timer_handle_t jawTimer = nullptr;
static bool jawAttached = false;
static bool jawControlReady = false;
static bool wasPlaying = false;
static uint16_t jawCurrentUs = JAW_CLOSED_US;
static float jawFilteredFactor = 0.0f;

static void IRAM_ATTR jawTimerCallback(void*) { jawTick = true; }
static const esp_timer_create_args_t jawTimerConfig = {
    .callback = &jawTimerCallback,
    .arg = nullptr,
    .name = "jaw50Hz",
};

static uint16_t jawUsToDuty(uint16_t pulseUs) {
    pulseUs = constrain(pulseUs, JAW_OPEN_US, JAW_CLOSED_US);
    const uint32_t fullScale = (1UL << JAW_RESOLUTION) - 1UL;
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(pulseUs) * fullScale + JAW_PERIOD_US / 2) /
        JAW_PERIOD_US);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
inline void jawAttachHardware() {
    if (!jawAttached) {
        jawAttached = ledcAttach(JAW_PIN, JAW_FREQ_HZ, JAW_RESOLUTION);
        if (!jawAttached) Serial.println("[JAW] ERROR: LEDC attach failed");
    }
}
inline void jawDetachHardware() {
    if (jawAttached) ledcDetach(JAW_PIN);
    jawAttached = false;
}
inline void jawWriteDuty(uint16_t duty) {
    if (jawAttached) ledcWrite(JAW_PIN, duty);
}
#else
#define JAW_LEDC_CH 0
inline void jawAttachHardware() {
    if (!jawAttached) {
        ledcSetup(JAW_LEDC_CH, JAW_FREQ_HZ, JAW_RESOLUTION);
        ledcAttachPin(JAW_PIN, JAW_LEDC_CH);
        jawAttached = true;
    }
}
inline void jawDetachHardware() {
    if (jawAttached) ledcDetachPin(JAW_PIN);
    jawAttached = false;
}
inline void jawWriteDuty(uint16_t duty) {
    if (jawAttached) ledcWrite(JAW_LEDC_CH, duty);
}
#endif

static void jawWriteMicroseconds(uint16_t pulseUs) {
    pulseUs = constrain(pulseUs, JAW_OPEN_US, JAW_CLOSED_US);
    jawWriteDuty(jawUsToDuty(pulseUs));
    jawCurrentUs = pulseUs;
}

static void attachJawClosed() {
    if (!ENABLE_JAW_MOTION || !jawControlReady) return;
    jawAttachHardware();
    if (!jawAttached) return;
    jawFilteredFactor = 0.0f;
    jawWriteMicroseconds(JAW_CLOSED_US);
}

static bool slewJawToward(uint16_t targetUs) {
    targetUs = constrain(targetUs, JAW_ACTIVE_OPEN_US, JAW_CLOSED_US);
    const int errorUs = static_cast<int>(targetUs) - static_cast<int>(jawCurrentUs);

    // Ignore tiny active-speech target changes that would only make an analog
    // servo hunt between adjacent PWM counts. Always allow an exact return to
    // the closed pulse when speech ends.
    if (targetUs != JAW_CLOSED_US && abs(errorUs) < JAW_TARGET_DEADBAND_US) {
        return false;
    }

    const uint16_t previousUs = jawCurrentUs;
    if (jawCurrentUs < targetUs) {  // opening
        const uint16_t next = jawCurrentUs + JAW_MAX_OPEN_STEP_US;
        jawCurrentUs = next > targetUs ? targetUs : next;
    } else if (jawCurrentUs > targetUs) {  // closing
        const uint16_t next = jawCurrentUs > JAW_MAX_CLOSE_STEP_US
            ? jawCurrentUs - JAW_MAX_CLOSE_STEP_US : JAW_ACTIVE_OPEN_US;
        jawCurrentUs = next < targetUs ? targetUs : next;
    }

    if (jawCurrentUs == previousUs) return false;
    jawWriteMicroseconds(jawCurrentUs);
    return true;
}

static void closeMouth(bool detachAfterSettling = true) {
    jawFilteredFactor = 0.0f;
    if (!ENABLE_JAW_MOTION || !jawAttached) return;
    while (jawCurrentUs != JAW_CLOSED_US) {
        slewJawToward(JAW_CLOSED_US);
        delay(20);
    }
    if (detachAfterSettling) {
        delay(200);
        jawDetachHardware();
    }
}

void webSocketEvent(uint8_t number, WStype_t type, uint8_t* payload, size_t length) {
    (void)number;
    switch (type) {
        case WStype_BIN:
            if (WiFiSkullController::isLive() && wavPlayer.playing()) {
                const size_t evenLength = length & ~static_cast<size_t>(1);
                wavPlayer.pushLivePCM(reinterpret_cast<int16_t*>(payload),
                                      evenLength / sizeof(int16_t));
            }
            break;
        case WStype_CONNECTED:
        case WStype_DISCONNECTED:
            break;
        default:
            break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    sdMutex = xSemaphoreCreateMutex();
    if (!sdMutex) {
        Serial.println("[SETUP] FATAL: SD mutex allocation failed");
        while (true) delay(1000);
    }

    ConfigManager::load("/skull.conf");
    SkullConfig& config = ConfigManager::get();
    const String configError = ConfigManager::getLastError();

    audioFFT.initialize();
    audioFFT.begin();
    audioFFT.reset();
    // attack=0.70 release=0.20 gain=3.5 noise=20 hold=50ms sib=0.65
    // headroom=1.25 curve=1.70
    // Release was raised 0.15->0.20 and hold cut 90->50ms so word gaps make
    // the jaw actually close; headroom/curve keep sustained speech mid-open
    // instead of pegged open (see EnvelopeConfigFast for the full picture).
    audioFFT.setConfig(EnvelopeConfigFast(0.70f, 0.20f, 3.5f, 20.0f, 50, 0.65f,
                                          1.25f, 1.70f));

    if (!wavPlayer.begin()) {
        Serial.println("[SETUP] FATAL: I2S initialization failed");
        while (true) delay(1000);
    }
    wavPlayer.setVolume(config.volume);

    if (!skullEyes.begin()) {
        Serial.println("[SETUP] ERROR: eye initialization failed; continuing without eye updates");
    }

    WiFiSkullController::init(config);
    webSocket = new(std::nothrow) WebSocketsServer(81);
    if (webSocket) {
        webSocket->begin();
        webSocket->onEvent(webSocketEvent);
    } else {
        Serial.println("[WS] ERROR: WebSocket server allocation failed");
    }

    if (!configError.isEmpty()) {
        String message = "Config warning: " + configError.substring(0, 80);
        WiFiSkullController::setStatusMessage(message.c_str(), true);
    }

    if (wavPlayer.playTestTone(440, 1500)) delay(1800);
    else Serial.println("[SETUP] ERROR: test tone did not start");

    if (ENABLE_JAW_MOTION) {
        if (esp_timer_create(&jawTimerConfig, &jawTimer) == ESP_OK &&
            esp_timer_start_periodic(jawTimer, JAW_PERIOD_US) == ESP_OK) {
            jawControlReady = true;
        } else {
            Serial.println("[JAW] ERROR: 50 Hz timer creation failed; jaw remains detached");
        }
    }

}

static void handlePlaybackTransition() {
    const bool nowPlaying = wavPlayer.playing();
    if (wasPlaying && !nowPlaying) {
        const char* finished = wavPlayer.getCurrentFile();
        WiFiSkullController::setFinishedMessage(finished);
        closeMouth();
    }
    wasPlaying = nowPlaying;
}

static void handleJawTick() {
    if (!ENABLE_JAW_MOTION || !jawTick) return;
    jawTick = false;
    if (!wavPlayer.playing() || wavPlayer.isPaused() || !jawAttached) return;

    const AudioMetricsFast audio = AudioFFTProcessorFast::getMetrics();
    const bool valid = isfinite(audio.jawFactor) && isfinite(audio.envelope) &&
                       isfinite(audio.vowelEnergy) &&
                       isfinite(audio.sibilantEnergy);

    float rawJawFactor = valid ? constrain(audio.jawFactor, 0.0f, 1.0f) : 0.0f;
    if (!valid || audio.envelope < JAW_ENVELOPE_THRESHOLD) rawJawFactor = 0.0f;

    if (!valid) {
        // Invalid analysis fails closed immediately rather than being smoothed.
        jawFilteredFactor = 0.0f;
    } else {
        const float alpha = rawJawFactor > jawFilteredFactor
            ? JAW_ATTACK_ALPHA : JAW_RELEASE_ALPHA;
        jawFilteredFactor += alpha * (rawJawFactor - jawFilteredFactor);
        if (rawJawFactor == 0.0f && jawFilteredFactor < 0.01f) {
            jawFilteredFactor = 0.0f;
        }
    }
    if (!isfinite(jawFilteredFactor)) jawFilteredFactor = 0.0f;
    jawFilteredFactor = constrain(jawFilteredFactor, 0.0f, 1.0f);

    const uint16_t targetUs = JAW_CLOSED_US - static_cast<uint16_t>(
        jawFilteredFactor * JAW_ACTIVE_RANGE_US + 0.5f);
    slewJawToward(targetUs);
}

static void handleWebCommand() {
    if (!WiFiSkullController::available()) return;

    const uint32_t command = WiFiSkullController::getReceivedValue();

    if (command == WIFI_CMD_STOP) {
        const bool stopped = wavPlayer.stop();
        wasPlaying = false;  // explicit stop must not become "Finished" next loop
        WiFiSkullController::setStatusMessage(
            stopped ? "Stopped" : "Error: audio stop timeout", !stopped);
        closeMouth();
    } else if (command == WIFI_CMD_PAUSE) {
        wavPlayer.pause();
        WiFiSkullController::setStatusMessage("Paused", false);
        closeMouth();
    } else if (command == WIFI_CMD_RESUME) {
        wavPlayer.resume();
        WiFiSkullController::setPlayingStatus(
            WiFiSkullController::getFileName(), true, false);
        attachJawClosed();
    } else if (command >= WIFI_CMD_PLAY_BASE && command < WIFI_CMD_PLAY_BASE + 500) {
        const char* fileName = WiFiSkullController::getFileName();
        const bool started = wavPlayer.playFile(fileName);
        if (started) {
            WiFiSkullController::setPlayingStatus(fileName, true, false);
            attachJawClosed();
            wasPlaying = true;
        } else {
            WiFiSkullController::setPlayingStatus(static_cast<uint8_t>(0), false);
            WiFiSkullController::setStatusMessage("Error: WAV not found or unsupported", true);
            wasPlaying = false;
        }
    } else if (command == WIFI_CMD_LIVE_MIC_START) {
        const bool started = wavPlayer.startLive();
        WiFiSkullController::setLiveMicStatus(started);
        if (started) {
            attachJawClosed();
            wasPlaying = true;
        } else {
            WiFiSkullController::setStatusMessage("Error: live audio start failed", true);
            wasPlaying = false;
        }
    } else if (command == WIFI_CMD_LIVE_MIC_STOP) {
        const bool stopped = wavPlayer.stop();
        wasPlaying = false;
        WiFiSkullController::setFinishedMessage(
            stopped ? "live mic" : "live mic (stop timeout)");
        closeMouth();
    } else if (command >= WIFI_CMD_VOLUME_BASE && command < WIFI_CMD_VOLUME_BASE + 31) {
        const int volume = command - WIFI_CMD_VOLUME_BASE;
        wavPlayer.setVolume(volume);
        WiFiSkullController::setVolume(volume);
        SkullConfig& config = ConfigManager::get();
        config.volume = volume;
        if (!wavPlayer.playing()) ConfigManager::save("/skull.conf");
    }

    WiFiSkullController::resetAvailable();
}

void loop() {
    WiFiSkullController::loop();
    if (webSocket) webSocket->loop();

    static uint32_t lastEyeUpdate = 0;
    const uint32_t now = millis();
    if (now - lastEyeUpdate >= 35) {
        skullEyes.update();
        lastEyeUpdate = now;
    }

    handlePlaybackTransition();
    handleWebCommand();
    handleJawTick();

    vTaskDelay(1);
}
