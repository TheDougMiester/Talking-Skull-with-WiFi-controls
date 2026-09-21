/*
 * File: I2SWavPlayer.cpp
 * Purpose: Implements PCM WAV parsing, LittleFS/SD streaming, I2S output, live PCM, volume scaling, and FFT feeding.
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

#include "I2SWavPlayer.h"

#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "SDMutex.h"

namespace {
constexpr size_t PRELOAD_LIMIT = 3 * 1024 * 1024;
// At volume 30, recover the 6 dB intentionally lost by the stereo (L+R)/2
// downmix. The MAX98357A GAIN pin can remain floating so its analog noise is
// not amplified. Individual samples are still hard-limited to valid int16_t.
constexpr float MAX_DIGITAL_VOLUME_FACTOR = 2.0f;

inline int16_t scaledSample(int16_t sample, float factor) {
    int32_t value = static_cast<int32_t>(sample * factor);
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return static_cast<int16_t>(value);
}
}

bool I2SWavPlayer::begin() {
    if (initialized) return true;

    stateMutex = xSemaphoreCreateMutex();
    taskDone = xSemaphoreCreateBinary();
    pcmStream = xStreamBufferCreate(LIVE_STREAM_BYTES, sizeof(int16_t));
    if (!stateMutex || !taskDone || !pcmStream) {
        Serial.println("[I2S] ERROR: control object allocation failed");
        return false;
    }

    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chanCfg.auto_clear = true;
    chanCfg.dma_desc_num = 8;
    chanCfg.dma_frame_num = AUDIO_FRAMES;

    esp_err_t err = i2s_new_channel(&chanCfg, &txChan, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: new channel failed: %s\n", esp_err_to_name(err));
        txChan = nullptr;
        return false;
    }

    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_LRC_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(txChan, &stdCfg);
    if (err == ESP_OK) err = i2s_channel_enable(txChan);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: channel init/enable failed: %s\n",
                      esp_err_to_name(err));
        i2s_del_channel(txChan);
        txChan = nullptr;
        return false;
    }

    if (I2S_SD_PIN != GPIO_NUM_NC) {
        pinMode(I2S_SD_PIN, OUTPUT);
        digitalWrite(I2S_SD_PIN, LOW);
        delay(20);
        digitalWrite(I2S_SD_PIN, HIGH);
    }

    initialized = true;
    setVolume(volume);
    return true;
}

void I2SWavPlayer::setVolume(int vol) {
    vol = constrain(vol, 0, 30);
    float factor = 0.0f;
    if (vol > 0) {
        const float normalized = static_cast<float>(vol) / 30.0f;
        factor = normalized * normalized * MAX_DIGITAL_VOLUME_FACTOR;
        if (factor > MAX_DIGITAL_VOLUME_FACTOR) {
            factor = MAX_DIGITAL_VOLUME_FACTOR;
        }
    }

    if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY);
    volume = vol;
    volumeFactor = factor;
    if (stateMutex) xSemaphoreGive(stateMutex);
}

float I2SWavPlayer::getVolumeFactor() {
    if (!stateMutex) return volumeFactor;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const float factor = volumeFactor;
    xSemaphoreGive(stateMutex);
    return factor;
}

int I2SWavPlayer::getVolume() {
    if (!stateMutex) return volume;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const int value = volume;
    xSemaphoreGive(stateMutex);
    return value;
}

bool I2SWavPlayer::getControlSnapshot(bool& stop, bool& paused, bool& live,
                                      bool& streaming) {
    if (!stateMutex) return false;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    stop = stopRequested;
    paused = pausedFlag;
    live = liveMode;
    streaming = streamingMode;
    const bool active = taskActive;
    xSemaphoreGive(stateMutex);
    return active;
}

void I2SWavPlayer::resetLiveStream() {
    if (pcmStream) xStreamBufferReset(pcmStream);
}

void I2SWavPlayer::cleanupPlaybackResources() {
    if (streamFile) {
        if (streamOnSD) {
            SDLockGuard lock;
            streamFile.close();
        } else {
            streamFile.close();
        }
    }
    if (psramBuffer) {
        heap_caps_free(psramBuffer);
        psramBuffer = nullptr;
    }
    psramBufferSize = 0;
    psramBufferPos = 0;
    streamBytesRemaining = 0;
}

bool I2SWavPlayer::stop(uint32_t timeoutMs) {
    if (!stateMutex) {
        cleanupPlaybackResources();
        return true;
    }

    bool waitForTask = false;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (taskActive) {
        waitForTask = true;
        if (!taskFinished) {
            stopRequested = true;
            pausedFlag = false;
            // Notify while stateMutex is held. The worker cannot publish
            // taskFinished and delete itself between this validity check and
            // the notification.
            if (playTask) xTaskNotifyGive(playTask);
        }
    }
    xSemaphoreGive(stateMutex);

    if (waitForTask) {
        if (xSemaphoreTake(taskDone, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            diagnostics.stopTimeouts++;
            xSemaphoreGive(stateMutex);
            Serial.printf("[I2S] ERROR: task did not stop within %u ms; resources retained\n",
                          static_cast<unsigned>(timeoutMs));
            return false;
        }
    } else {
        cleanupPlaybackResources();
    }

    resetLiveStream();
    AudioFFTProcessorFast::reset();

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    taskActive = false;
    taskFinished = false;
    playTask = nullptr;
    playingFlag = false;
    liveMode = false;
    streamingMode = false;
    streamOnSD = false;
    pausedFlag = false;
    stopRequested = false;
    xSemaphoreGive(stateMutex);
    return true;
}

void I2SWavPlayer::pause() {
    bool changed = false;
    if (stateMutex) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        if (taskActive && playingFlag && !pausedFlag) {
            pausedFlag = true;
            changed = true;
        }
        xSemaphoreGive(stateMutex);
    }
    if (changed) {
        AudioFFTProcessorFast::reset();
    }
}

void I2SWavPlayer::resume() {
    TaskHandle_t task = nullptr;
    bool changed = false;
    if (stateMutex) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        if (taskActive && playingFlag && pausedFlag) {
            pausedFlag = false;
            task = playTask;
            changed = true;
        }
        xSemaphoreGive(stateMutex);
    }
    if (changed) {
        AudioFFTProcessorFast::reset();
        if (task) xTaskNotifyGive(task);
    }
}

bool I2SWavPlayer::playing() {
    if (!stateMutex) return false;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const bool value = playingFlag;
    xSemaphoreGive(stateMutex);
    return value;
}

bool I2SWavPlayer::isPaused() {
    if (!stateMutex) return false;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const bool value = pausedFlag;
    xSemaphoreGive(stateMutex);
    return value;
}

bool I2SWavPlayer::parseWavHeader(File& file, int& channels, int& sampleRate,
                                  int& bitsPerSample, uint32_t& dataSize,
                                  size_t& dataStart) {
    char id[4];
    uint32_t riffSize = 0;
    if (file.read(reinterpret_cast<uint8_t*>(id), 4) != 4 ||
        memcmp(id, "RIFF", 4) != 0 ||
        file.read(reinterpret_cast<uint8_t*>(&riffSize), 4) != 4 ||
        file.read(reinterpret_cast<uint8_t*>(id), 4) != 4 ||
        memcmp(id, "WAVE", 4) != 0) {
        return false;
    }

    bool haveFormat = false;
    uint16_t audioFormat = 0;
    uint16_t blockAlign = 0;
    channels = 0;
    sampleRate = 0;
    bitsPerSample = 0;
    dataSize = 0;
    dataStart = 0;

    const size_t fileSize = file.size();
    while (file.position() + 8 <= fileSize) {
        uint32_t chunkSize = 0;
        if (file.read(reinterpret_cast<uint8_t*>(id), 4) != 4 ||
            file.read(reinterpret_cast<uint8_t*>(&chunkSize), 4) != 4) {
            return false;
        }

        const size_t chunkStart = file.position();
        const size_t available = (chunkStart <= fileSize) ? fileSize - chunkStart : 0;
        if (chunkSize > available) return false;

        if (memcmp(id, "fmt ", 4) == 0) {
            if (chunkSize < 16) return false;
            uint16_t numChannels = 0;
            uint16_t bits = 0;
            uint32_t rate = 0;
            uint32_t byteRate = 0;
            if (file.read(reinterpret_cast<uint8_t*>(&audioFormat), 2) != 2 ||
                file.read(reinterpret_cast<uint8_t*>(&numChannels), 2) != 2 ||
                file.read(reinterpret_cast<uint8_t*>(&rate), 4) != 4 ||
                file.read(reinterpret_cast<uint8_t*>(&byteRate), 4) != 4 ||
                file.read(reinterpret_cast<uint8_t*>(&blockAlign), 2) != 2 ||
                file.read(reinterpret_cast<uint8_t*>(&bits), 2) != 2) {
                return false;
            }
            channels = numChannels;
            sampleRate = static_cast<int>(rate);
            bitsPerSample = bits;
            haveFormat = true;
        } else if (memcmp(id, "data", 4) == 0) {
            dataStart = chunkStart;
            dataSize = chunkSize;
            break;
        }

        const size_t next = chunkStart + chunkSize + (chunkSize & 1U);
        if (next > fileSize || !file.seek(next)) return false;
    }

    if (!haveFormat || dataStart == 0 || dataSize == 0) return false;
    if (audioFormat != 1 || (channels != 1 && channels != 2) ||
        bitsPerSample != 16 || sampleRate < 8000 || sampleRate > 48000) {
        return false;
    }

    const uint16_t expectedAlign = channels * sizeof(int16_t);
    if (blockAlign != expectedAlign) return false;
    dataSize -= dataSize % expectedAlign;
    return dataSize > 0;
}

bool I2SWavPlayer::startPlayerTask(const char* taskName) {
    while (xSemaphoreTake(taskDone, 0) == pdTRUE) {}

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    stopRequested = false;
    pausedFlag = false;
    taskFinished = false;
    taskActive = true;
    playingFlag = true;
    BaseType_t created = xTaskCreatePinnedToCore(
        playerTaskStatic, taskName, 16384, this, 5, &playTask, 0);
    if (created != pdPASS) {
        playTask = nullptr;
        taskActive = false;
        playingFlag = false;
        diagnostics.taskCreateFailures++;
    }
    xSemaphoreGive(stateMutex);

    if (created != pdPASS) {
        Serial.printf("[I2S] ERROR: failed to create %s task\n", taskName);
        cleanupPlaybackResources();
        return false;
    }
    return true;
}

bool I2SWavPlayer::playFile(const char* fileName) {
    if (!initialized || !fileName || fileName[0] == '\0') return false;
    if (!stop()) return false;

    File file;
    bool onSD = false;

    {
        SDLockGuard lock;
        if (SD_MMC.cardType() != CARD_NONE) {
            String paths[] = {String("/wav/") + fileName,
                              String("/") + fileName,
                              String(fileName)};
            for (const auto& path : paths) {
                if (SD_MMC.exists(path)) {
                    file = SD_MMC.open(path, "r");
                    if (file) {
                        onSD = true;
                        break;
                    }
                }
            }
        }
    }

    if (!file) {
        String paths[] = {String("/wav/") + fileName, String("/") + fileName};
        for (const auto& path : paths) {
            if (LittleFS.exists(path)) {
                file = LittleFS.open(path, "r");
                if (file) {
                    break;
                }
            }
        }
    }

    if (!file) {
        return false;
    }

    uint32_t dataSize = 0;
    size_t dataStart = 0;
    bool headerOK = false;
    if (onSD) {
        SDLockGuard lock;
        headerOK = parseWavHeader(file, wavChannels, wavSampleRate, wavBits,
                                  dataSize, dataStart);
    } else {
        headerOK = parseWavHeader(file, wavChannels, wavSampleRate, wavBits,
                                  dataSize, dataStart);
    }

    if (!headerOK) {
        Serial.println("[I2S] ERROR: unsupported WAV; require PCM, mono/stereo, 16-bit, 8-48 kHz");
        if (onSD) {
            SDLockGuard lock;
            file.close();
        } else {
            file.close();
        }
        return false;
    }


    const bool useStream = onSD || dataSize > PRELOAD_LIMIT;
    streamOnSD = onSD;
    streamingMode = useStream;
    liveMode = false;

    if (useStream) {
        streamFile = file;
        bool seekOK;
        if (onSD) {
            SDLockGuard lock;
            seekOK = streamFile.seek(dataStart);
        } else {
            seekOK = streamFile.seek(dataStart);
        }
        if (!seekOK) {
            Serial.println("[I2S] ERROR: seek to WAV data failed");
            cleanupPlaybackResources();
            return false;
        }
        streamBytesRemaining = dataSize;
    } else {
        psramBuffer = static_cast<uint8_t*>(
            heap_caps_malloc(dataSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!psramBuffer) {
            psramBuffer = static_cast<uint8_t*>(
                heap_caps_malloc(dataSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }

        if (!psramBuffer) {
            streamFile = file;
            if (!streamFile.seek(dataStart)) {
                cleanupPlaybackResources();
                return false;
            }
            streamingMode = true;
            streamBytesRemaining = dataSize;
        } else {
            file.seek(dataStart);
            size_t readBytes = file.read(psramBuffer, dataSize);
            file.close();
            const size_t frameBytes = wavChannels * sizeof(int16_t);
            readBytes -= readBytes % frameBytes;
            if (readBytes == 0) {
                Serial.println("[I2S] ERROR: WAV data read failed");
                cleanupPlaybackResources();
                return false;
            }
            psramBufferSize = readBytes;
            psramBufferPos = 0;
            streamingMode = false;
        }
    }

    i2s_std_clk_config_t clkCfg = I2S_STD_CLK_DEFAULT_CONFIG(
        static_cast<uint32_t>(wavSampleRate));
    esp_err_t err = i2s_channel_disable(txChan);
    if (err == ESP_OK) err = i2s_channel_reconfig_std_clock(txChan, &clkCfg);
    if (err == ESP_OK) err = i2s_channel_enable(txChan);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: sample-rate reconfiguration failed: %s\n",
                      esp_err_to_name(err));
        cleanupPlaybackResources();
        return false;
    }

    strncpy(currentFile, fileName, sizeof(currentFile) - 1);
    currentFile[sizeof(currentFile) - 1] = '\0';
    AudioFFTProcessorFast::setSampleRate(static_cast<uint32_t>(wavSampleRate));
    AudioFFTProcessorFast::reset();
    return startPlayerTask("i2sWav");
}

bool I2SWavPlayer::playTestTone(int freqHz, int durationMs) {
    if (!initialized || freqHz <= 0 || durationMs <= 0) return false;
    if (!stop()) return false;

    const int sampleRate = I2S_SAMPLE_RATE;
    const size_t samples = static_cast<size_t>(sampleRate) * durationMs / 1000;
    const size_t bytes = samples * sizeof(int16_t);
    psramBuffer = static_cast<uint8_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!psramBuffer) {
        psramBuffer = static_cast<uint8_t*>(
            heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!psramBuffer) {
        Serial.printf("[I2S] ERROR: test-tone allocation failed (%u bytes)\n",
                      static_cast<unsigned>(bytes));
        return false;
    }

    int16_t* pcm = reinterpret_cast<int16_t*>(psramBuffer);
    for (size_t i = 0; i < samples; ++i) {
        const float phase = 2.0f * PI * freqHz * static_cast<float>(i) / sampleRate;
        pcm[i] = static_cast<int16_t>(sinf(phase) * 12000.0f);
    }

    psramBufferSize = bytes;
    psramBufferPos = 0;
    wavChannels = 1;
    wavSampleRate = sampleRate;
    wavBits = 16;
    streamingMode = false;
    streamOnSD = false;
    liveMode = false;
    strncpy(currentFile, "TEST TONE", sizeof(currentFile) - 1);

    i2s_std_clk_config_t clkCfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
    esp_err_t err = i2s_channel_disable(txChan);
    if (err == ESP_OK) err = i2s_channel_reconfig_std_clock(txChan, &clkCfg);
    if (err == ESP_OK) err = i2s_channel_enable(txChan);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: tone clock setup failed: %s\n", esp_err_to_name(err));
        cleanupPlaybackResources();
        return false;
    }

    AudioFFTProcessorFast::setSampleRate(sampleRate);
    AudioFFTProcessorFast::reset();
    return startPlayerTask("i2sTone");
}

bool I2SWavPlayer::startLive() {
    if (!initialized || !stop()) return false;

    resetLiveStream();
    wavSampleRate = I2S_SAMPLE_RATE;
    wavChannels = 1;
    wavBits = 16;
    liveMode = true;
    streamingMode = false;
    streamOnSD = false;
    strncpy(currentFile, "LIVE MIC", sizeof(currentFile) - 1);

    i2s_std_clk_config_t clkCfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE);
    esp_err_t err = i2s_channel_disable(txChan);
    if (err == ESP_OK) err = i2s_channel_reconfig_std_clock(txChan, &clkCfg);
    if (err == ESP_OK) err = i2s_channel_enable(txChan);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: live clock setup failed: %s\n", esp_err_to_name(err));
        liveMode = false;
        return false;
    }

    AudioFFTProcessorFast::setSampleRate(I2S_SAMPLE_RATE);
    AudioFFTProcessorFast::reset();
    return startPlayerTask("i2sLive");
}

void I2SWavPlayer::pushLivePCM(const int16_t* pcm, int samples) {
    if (!pcm || samples <= 0 || !pcmStream || !stateMutex) return;

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const bool accept = taskActive && playingFlag && liveMode && !stopRequested;
    xSemaphoreGive(stateMutex);
    if (!accept) return;

    const size_t requested = static_cast<size_t>(samples) * sizeof(int16_t);
    size_t sendBytes = min(requested, xStreamBufferSpacesAvailable(pcmStream));
    sendBytes &= ~static_cast<size_t>(1);  // never split a 16-bit sample
    size_t sent = 0;
    if (sendBytes > 0) {
        sent = xStreamBufferSend(pcmStream, pcm, sendBytes, 0);
        sent &= ~static_cast<size_t>(1);
    }

    if (sent < requested) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        diagnostics.liveBytesDropped += requested - sent;
        xSemaphoreGive(stateMutex);
    }
}

void I2SWavPlayer::recordI2SWriteResult(esp_err_t err, size_t requested,
                                        size_t written) {
    if (err == ESP_OK && written == requested) return;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    diagnostics.i2sWriteErrors++;
    xSemaphoreGive(stateMutex);
    Serial.printf("[I2S] ERROR: write failure err=%s requested=%u written=%u\n",
                  esp_err_to_name(err), static_cast<unsigned>(requested),
                  static_cast<unsigned>(written));
}

void I2SWavPlayer::updatePlayerStackWatermark() {
    const UBaseType_t words = uxTaskGetStackHighWaterMark(nullptr);
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (diagnostics.playerStackMinWords == 0 ||
        words < diagnostics.playerStackMinWords) {
        diagnostics.playerStackMinWords = words;
    }
    xSemaphoreGive(stateMutex);
}

I2SPlayerDiagnostics I2SWavPlayer::getDiagnostics() {
    I2SPlayerDiagnostics copy;
    if (!stateMutex) return copy;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    copy = diagnostics;
    xSemaphoreGive(stateMutex);
    if (pcmStream) copy.liveBufferedBytes = xStreamBufferBytesAvailable(pcmStream);
    return copy;
}

void I2SWavPlayer::playerTaskMain() {
    int16_t monoIn[AUDIO_FRAMES];
    int16_t stereoIn[AUDIO_FRAMES * 2];
    int16_t stereoOut[AUDIO_FRAMES * 2];
    int16_t leftForFFT[AUDIO_FRAMES];

    bool stop = false;
    bool paused = false;
    bool live = false;
    bool streaming = false;
    getControlSnapshot(stop, paused, live, streaming);

    uint32_t chunks = 0;
    bool liveTimedOut = false;

    auto writeMono = [&](const int16_t* source, int samples) {
        const float factor = getVolumeFactor();
        memcpy(leftForFFT, source, samples * sizeof(int16_t));
        for (int i = 0; i < samples; ++i) {
            const int16_t output = scaledSample(source[i], factor);
            stereoOut[2 * i] = output;
            stereoOut[2 * i + 1] = output;
        }
        size_t written = 0;
        const size_t bytes = samples * 2 * sizeof(int16_t);
        const esp_err_t err = i2s_channel_write(txChan, stereoOut, bytes,
                                                 &written, 1000);
        recordI2SWriteResult(err, bytes, written);
        AudioFFTProcessorFast::feedPCM(leftForFFT, samples);
    };

    auto writeStereo = [&](const int16_t* source, int frames) {
        const float factor = getVolumeFactor();
        for (int i = 0; i < frames; ++i) {
            const int16_t left = source[2 * i];
            const int16_t right = source[2 * i + 1];
            leftForFFT[i] = left;  // isolated vocal channel, unscaled
            const int32_t mix = (static_cast<int32_t>(left) + right) / 2;
            const int16_t output = scaledSample(static_cast<int16_t>(mix), factor);
            stereoOut[2 * i] = output;
            stereoOut[2 * i + 1] = output;
        }
        size_t written = 0;
        const size_t bytes = frames * 2 * sizeof(int16_t);
        const esp_err_t err = i2s_channel_write(txChan, stereoOut, bytes,
                                                 &written, 1000);
        recordI2SWriteResult(err, bytes, written);
        AudioFFTProcessorFast::feedPCM(leftForFFT, frames);
    };

    while (getControlSnapshot(stop, paused, live, streaming) && !stop) {
        if (paused) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }

        if (live) {
            size_t received = xStreamBufferReceive(
                pcmStream, monoIn, sizeof(monoIn), pdMS_TO_TICKS(100));
            received &= ~static_cast<size_t>(1);
            if (received == 0) {
                if (!liveTimedOut) {
                    AudioFFTProcessorFast::reset();
                    liveTimedOut = true;
                }
                continue;
            }
            liveTimedOut = false;
            writeMono(monoIn, received / sizeof(int16_t));
        } else if (streaming) {
            if (!streamFile || streamBytesRemaining == 0) break;

            const size_t frameBytes = wavChannels * sizeof(int16_t);
            size_t wanted = min(streamBytesRemaining,
                                AUDIO_FRAMES * frameBytes);
            wanted -= wanted % frameBytes;
            if (wanted == 0) break;

            uint8_t* destination = wavChannels == 2
                ? reinterpret_cast<uint8_t*>(stereoIn)
                : reinterpret_cast<uint8_t*>(monoIn);
            size_t received = 0;
            if (streamOnSD) {
                SDLockGuard lock;
                received = streamFile.read(destination, wanted);
            } else {
                received = streamFile.read(destination, wanted);
            }
            received -= received % frameBytes;
            if (received == 0) {
                Serial.printf("[I2S] WARNING: stream ended with %u WAV bytes remaining\n",
                              static_cast<unsigned>(streamBytesRemaining));
                break;
            }
            streamBytesRemaining -= min(streamBytesRemaining, received);

            getControlSnapshot(stop, paused, live, streaming);
            if (stop) break;
            if (wavChannels == 2) {
                writeStereo(stereoIn, received / (2 * sizeof(int16_t)));
            } else {
                writeMono(monoIn, received / sizeof(int16_t));
            }
        } else {
            if (!psramBuffer || psramBufferPos >= psramBufferSize) break;
            const size_t frameBytes = wavChannels * sizeof(int16_t);
            size_t wanted = min(psramBufferSize - psramBufferPos,
                                AUDIO_FRAMES * frameBytes);
            wanted -= wanted % frameBytes;
            if (wanted == 0) break;

            if (wavChannels == 2) {
                memcpy(stereoIn, psramBuffer + psramBufferPos, wanted);
                psramBufferPos += wanted;
                writeStereo(stereoIn, wanted / (2 * sizeof(int16_t)));
            } else {
                memcpy(monoIn, psramBuffer + psramBufferPos, wanted);
                psramBufferPos += wanted;
                writeMono(monoIn, wanted / sizeof(int16_t));
            }
        }

        ++chunks;
        if ((chunks & 0x1fU) == 0) updatePlayerStackWatermark();
    }

    updatePlayerStackWatermark();
    cleanupPlaybackResources();
    resetLiveStream();
    AudioFFTProcessorFast::reset();

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    playingFlag = false;
    liveMode = false;
    streamingMode = false;
    streamOnSD = false;
    pausedFlag = false;
    stopRequested = false;
    // Keep taskActive/playTask intact until stop() consumes taskDone. This
    // prevents a new task from reusing the shared completion semaphore while
    // this worker is still in its final self-delete sequence.
    taskFinished = true;
    xSemaphoreGive(stateMutex);

    xSemaphoreGive(taskDone);
    vTaskDelete(nullptr);
}
