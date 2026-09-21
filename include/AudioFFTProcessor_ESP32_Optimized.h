/*
 * File: AudioFFTProcessor_ESP32_Optimized.h
 * Purpose: Declares FFT metrics, envelope configuration, and the PCM analysis interface.
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
/*
 * AudioFFTProcessorFast
 *
 * Receives PCM directly from the WAV/I2S playback task. For stereo WAVs, the
 * player passes only the isolated LEFT vocal channel here; the speaker path
 * separately downmixes LEFT+RIGHT. There is no ADC pin and no analogRead task,
 * deliberately freeing GPIO4 for I2S SD_MODE.
 *
 * The analysis sample rate follows the current WAV. FFT band limits therefore
 * continue to represent the same physical frequencies at 16, 44.1, or 48 kHz.
 */

#include <Arduino.h>
#include "freertos/semphr.h"

#define FFT_REFERENCE_FREQ 16000U
#define FFT_SIZE 512

#define JAW_OPEN_LOW_HZ    300
#define JAW_OPEN_HIGH_HZ  1000
#define JAW_CLOSE_LOW_HZ  2500
#define JAW_CLOSE_HIGH_HZ 6000

struct EnvelopeConfigFast {
    float attackAlpha = 0.70f;
    float releaseAlpha = 0.15f;
    // Analysis sensitivity only. This never scales I2S speaker samples.
    float jawAnalysisGain = 3.5f;
    float noiseFloor = 20.0f;
    uint16_t holdMs = 90;
    float sibilantSuppression = 0.65f;

    // Dynamic-range shaping for the jaw (added for mouth realism):
    //  - peakHeadroom > 1.0 scales the normalization ceiling above the
    //    tracked envelope peak, so sustained speech maps BELOW 100% open
    //    instead of pegging the mouth open the whole time.
    //  - jawCurveExp > 1.0 expands contrast: mid-level speech maps lower
    //    (mouth more closed overall) while emphasized syllable onsets --
    //    which overshoot the lagging peak tracker -- still reach full open.
    float peakHeadroom = 1.25f;
    float jawCurveExp = 1.7f;

    EnvelopeConfigFast() = default;
    EnvelopeConfigFast(float a, float r, float analysisGain, float nf,
                       uint16_t h, float s = 0.65f,
                       float headroom = 1.25f, float curveExp = 1.7f)
        : attackAlpha(a), releaseAlpha(r), jawAnalysisGain(analysisGain),
          noiseFloor(nf), holdMs(h), sibilantSuppression(s),
          peakHeadroom(headroom), jawCurveExp(curveExp) {}
};

// One coherent cross-core snapshot for the jaw and eyes.
struct AudioMetricsFast {
    float envelope = 0.0f;
    float peak = 200.0f;
    float vowelEnergy = 0.0f;
    float sibilantEnergy = 0.0f;
    float jawFactor = 0.0f;
    uint32_t sampleRate = FFT_REFERENCE_FREQ;
};

class AudioFFTProcessorFast {
private:
    static SemaphoreHandle_t ringMutex;
    static SemaphoreHandle_t metricsMutex;

    alignas(16) static float window[FFT_SIZE];
    alignas(16) static float fftWork[FFT_SIZE * 2];
    alignas(16) static float pcmRing[FFT_SIZE];
    static int pcmRingPos;

    static float envelope;
    static float envelopePeak;
    static float envelopeMin;
    static uint32_t lastVoiceMs;
    static uint32_t analysisSampleRate;
    static EnvelopeConfigFast envCfg;
    static AudioMetricsFast metrics;
    static bool fftReady;

    static void buildWindow();
    static void processBuffer();
    static void publishMetrics(float vowelEnergy, float sibilantEnergy,
                               float jawFactor);

public:
    // No GPIO parameter: PCM comes from I2S/WAV, not an analog ADC pin.
    void initialize();
    void begin();

    static void reset();
    static void setConfig(const EnvelopeConfigFast& c);
    static void setSampleRate(uint32_t sampleRate);

    // pcm contains unscaled signed 16-bit analysis samples.
    static void feedPCM(const int16_t* pcm, int len);

    static AudioMetricsFast getMetrics();
    static float getJawFactor()       { return getMetrics().jawFactor; }
    static float getEnvelope()        { return getMetrics().envelope; }
    static float getVowelEnergy()     { return getMetrics().vowelEnergy; }
    static float getSibilantEnergy()  { return getMetrics().sibilantEnergy; }
};
