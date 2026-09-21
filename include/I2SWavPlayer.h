/*
 * File: I2SWavPlayer.h
 * Purpose: Declares the asynchronous I2S WAV player and runtime audio diagnostics.
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

#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include "driver/i2s_std.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "PinDefinitions.h"
#include "AudioFFTProcessor_ESP32_Optimized.h"

struct I2SPlayerDiagnostics {
    uint32_t i2sWriteErrors = 0;
    uint32_t liveBytesDropped = 0;
    uint32_t taskCreateFailures = 0;
    uint32_t stopTimeouts = 0;
    UBaseType_t playerStackMinWords = 0;
    size_t liveBufferedBytes = 0;
};

class I2SWavPlayer {
private:
    static constexpr size_t AUDIO_FRAMES = 512;
    static constexpr size_t LIVE_STREAM_BYTES = 12 * 1024;
    static constexpr uint32_t STOP_TIMEOUT_MS = 3000;

    i2s_chan_handle_t txChan = nullptr;
    File streamFile;

    SemaphoreHandle_t stateMutex = nullptr;
    SemaphoreHandle_t taskDone = nullptr;
    StreamBufferHandle_t pcmStream = nullptr;
    TaskHandle_t playTask = nullptr;

    bool initialized = false;
    bool taskActive = false;
    bool taskFinished = false;  // protected by stateMutex; completion semaphore follows
    bool playingFlag = false;
    bool liveMode = false;
    bool streamingMode = false;
    bool streamOnSD = false;
    bool pausedFlag = false;
    bool stopRequested = false;

    char currentFile[64] = {0};
    int wavChannels = 1;
    int wavSampleRate = 16000;
    int wavBits = 16;

    uint8_t* psramBuffer = nullptr;
    size_t psramBufferSize = 0;
    size_t psramBufferPos = 0;
    size_t streamBytesRemaining = 0;

    int volume = 25;
    float volumeFactor = 0.85f;
    I2SPlayerDiagnostics diagnostics;

    static void playerTaskStatic(void* arg) {
        static_cast<I2SWavPlayer*>(arg)->playerTaskMain();
    }

    void playerTaskMain();
    bool startPlayerTask(const char* taskName);
    void cleanupPlaybackResources();
    void resetLiveStream();
    float getVolumeFactor();
    bool getControlSnapshot(bool& stop, bool& paused, bool& live,
                            bool& streaming);
    void recordI2SWriteResult(esp_err_t err, size_t requested, size_t written);
    void updatePlayerStackWatermark();

    bool parseWavHeader(File& f, int& channels, int& sampleRate,
                        int& bitsPerSample, uint32_t& dataSize,
                        size_t& dataStart);

public:
    I2SWavPlayer() = default;
    ~I2SWavPlayer() { stop(); }

    bool begin();
    bool stop(uint32_t timeoutMs = STOP_TIMEOUT_MS);
    bool playFile(const char* fileName);
    bool playTestTone(int freqHz = 440, int durationMs = 2000);
    bool startLive();
    void pushLivePCM(const int16_t* pcm, int samples);
    void stopLive() { stop(); }

    void setVolume(int vol);
    void pause();
    void resume();

    bool isPaused();
    bool playing();
    const char* getCurrentFile() const { return currentFile; }
    int getVolume();
    I2SPlayerDiagnostics getDiagnostics();
};
