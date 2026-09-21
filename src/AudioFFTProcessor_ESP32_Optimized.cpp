/*
 * File: AudioFFTProcessor_ESP32_Optimized.cpp
 * Purpose: Implements PCM FFT analysis, vowel/sibilant energy extraction, and the jaw envelope.
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

#include "AudioFFTProcessor_ESP32_Optimized.h"
#include <math.h>
#include <string.h>

#if __has_include("esp_dsp.h")
  #include "esp_dsp.h"
  #define SKULL_HAS_ESP_DSP 1
#else
  #define SKULL_HAS_ESP_DSP 0
#endif

SemaphoreHandle_t AudioFFTProcessorFast::ringMutex = nullptr;
SemaphoreHandle_t AudioFFTProcessorFast::metricsMutex = nullptr;

alignas(16) float AudioFFTProcessorFast::window[FFT_SIZE];
alignas(16) float AudioFFTProcessorFast::fftWork[FFT_SIZE * 2];
alignas(16) float AudioFFTProcessorFast::pcmRing[FFT_SIZE];
int AudioFFTProcessorFast::pcmRingPos = 0;

float AudioFFTProcessorFast::envelope = 0.0f;
float AudioFFTProcessorFast::envelopePeak = 200.0f;
float AudioFFTProcessorFast::envelopeMin = 0.0f;
uint32_t AudioFFTProcessorFast::lastVoiceMs = 0;
uint32_t AudioFFTProcessorFast::analysisSampleRate = FFT_REFERENCE_FREQ;
EnvelopeConfigFast AudioFFTProcessorFast::envCfg;
AudioMetricsFast AudioFFTProcessorFast::metrics;
bool AudioFFTProcessorFast::fftReady = false;

static float scaledAlpha(float referenceAlpha, float frameMs) {
    referenceAlpha = constrain(referenceAlpha, 0.0f, 1.0f);
    if (referenceAlpha <= 0.0f) return 0.0f;
    if (referenceAlpha >= 1.0f) return 1.0f;
    const float referenceFrameMs = 1000.0f * FFT_SIZE / FFT_REFERENCE_FREQ;
    return 1.0f - powf(1.0f - referenceAlpha, frameMs / referenceFrameMs);
}

void AudioFFTProcessorFast::buildWindow() {
    for (int i = 0; i < FFT_SIZE; ++i) {
        window[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_SIZE - 1)));
    }
}

void AudioFFTProcessorFast::initialize() {
    if (ringMutex == nullptr) ringMutex = xSemaphoreCreateMutex();
    if (metricsMutex == nullptr) metricsMutex = xSemaphoreCreateMutex();

    if (ringMutex == nullptr || metricsMutex == nullptr) {
        Serial.println("[FFT] ERROR: mutex allocation failed");
        return;
    }

    reset();
}

void AudioFFTProcessorFast::begin() {
    buildWindow();
#if SKULL_HAS_ESP_DSP
    esp_err_t err = dsps_fft2r_init_fc32(nullptr, FFT_SIZE);
    fftReady = (err == ESP_OK);
#else
    fftReady = false;
    Serial.println("[FFT] WARNING: esp_dsp.h unavailable; using amplitude fallback");
#endif
}

void AudioFFTProcessorFast::setConfig(const EnvelopeConfigFast& c) {
    if (ringMutex) xSemaphoreTake(ringMutex, portMAX_DELAY);
    envCfg = c;
    if (ringMutex) xSemaphoreGive(ringMutex);

}

void AudioFFTProcessorFast::setSampleRate(uint32_t sampleRate) {
    if (sampleRate < 8000U || sampleRate > 96000U) {
        sampleRate = FFT_REFERENCE_FREQ;
    }

    if (ringMutex) xSemaphoreTake(ringMutex, portMAX_DELAY);
    analysisSampleRate = sampleRate;
    pcmRingPos = 0; // never combine samples from two rates in one FFT frame
    memset(pcmRing, 0, sizeof(pcmRing));
    if (ringMutex) xSemaphoreGive(ringMutex);

}

void AudioFFTProcessorFast::reset() {
    if (ringMutex) xSemaphoreTake(ringMutex, portMAX_DELAY);

    pcmRingPos = 0;
    memset(pcmRing, 0, sizeof(pcmRing));
    envelope = 0.0f;
    envelopePeak = 200.0f;
    envelopeMin = 0.0f;
    lastVoiceMs = millis();

    if (metricsMutex) xSemaphoreTake(metricsMutex, portMAX_DELAY);
    metrics = AudioMetricsFast();
    metrics.sampleRate = analysisSampleRate;
    if (metricsMutex) xSemaphoreGive(metricsMutex);

    if (ringMutex) xSemaphoreGive(ringMutex);
}

void AudioFFTProcessorFast::publishMetrics(float vowelEnergy,
                                           float sibilantEnergy,
                                           float jawFactor) {
    // Last-line containment: consumers never receive non-finite servo data.
    const float safeEnvelope = isfinite(envelope) ? envelope : 0.0f;
    const float safePeak = isfinite(envelopePeak) ? envelopePeak : 200.0f;
    const float safeVowel = isfinite(vowelEnergy) ? vowelEnergy : 0.0f;
    const float safeSibilant = isfinite(sibilantEnergy) ? sibilantEnergy : 0.0f;
    const float safeJaw = isfinite(jawFactor)
        ? constrain(jawFactor, 0.0f, 1.0f) : 0.0f;
    if (metricsMutex) xSemaphoreTake(metricsMutex, portMAX_DELAY);
    metrics.envelope = safeEnvelope;
    metrics.peak = safePeak;
    metrics.vowelEnergy = safeVowel;
    metrics.sibilantEnergy = safeSibilant;
    metrics.jawFactor = safeJaw;
    metrics.sampleRate = analysisSampleRate;
    if (metricsMutex) xSemaphoreGive(metricsMutex);
}

AudioMetricsFast AudioFFTProcessorFast::getMetrics() {
    AudioMetricsFast copy;
    if (metricsMutex) xSemaphoreTake(metricsMutex, portMAX_DELAY);
    copy = metrics;
    if (metricsMutex) xSemaphoreGive(metricsMutex);
    return copy;
}

void AudioFFTProcessorFast::feedPCM(const int16_t* pcm, int len) {
    if (pcm == nullptr || len <= 0 || ringMutex == nullptr) return;

    xSemaphoreTake(ringMutex, portMAX_DELAY);
    for (int i = 0; i < len; ++i) {
        pcmRing[pcmRingPos++] = static_cast<float>(pcm[i]);
        if (pcmRingPos >= FFT_SIZE) {
            pcmRingPos = 0;
            // Processing remains under ringMutex. reset() and sample-rate changes
            // therefore cannot modify this frame while the FFT is running.
            processBuffer();
        }
    }
    xSemaphoreGive(ringMutex);
}

void AudioFFTProcessorFast::processBuffer() {
    double sum = 0.0;
    for (int i = 0; i < FFT_SIZE; ++i) sum += pcmRing[i];
    const float dc = static_cast<float>(sum / FFT_SIZE);

    float meanAbs = 0.0f;
    for (int i = 0; i < FFT_SIZE; ++i) {
        const float centered = pcmRing[i] - dc;
        meanAbs += fabsf(centered);
        // ESP-DSP complex FFT input must be Re0,Im0,Re1,Im1,...
        fftWork[2 * i] = centered * window[i];
        fftWork[2 * i + 1] = 0.0f;
    }
    meanAbs /= FFT_SIZE;

    float vowelEnergy = 0.0f;
    float sibilantEnergy = 0.0f;

#if SKULL_HAS_ESP_DSP
    if (fftReady) {
        dsps_fft2r_fc32(fftWork, FFT_SIZE);
        dsps_bit_rev_fc32(fftWork, FFT_SIZE);

        const int maxBin = FFT_SIZE / 2 - 1;
        auto hzToBin = [maxBin](uint32_t hz) -> int {
            int bin = static_cast<int>((static_cast<uint64_t>(hz) * FFT_SIZE)
                                       / analysisSampleRate);
            return constrain(bin, 1, maxBin);
        };

        const int openLow = hzToBin(JAW_OPEN_LOW_HZ);
        const int openHigh = hzToBin(JAW_OPEN_HIGH_HZ);
        const int closeLow = hzToBin(JAW_CLOSE_LOW_HZ);
        const int closeHigh = hzToBin(JAW_CLOSE_HIGH_HZ);

        // The Hann-windowed complex FFT produces useful relative band energy
        // with this normalization. Normalize bin sums to the 16 kHz reference
        // so thresholds do not collapse at higher sample rates.
        const float magnitudeScale = 2.0f / FFT_SIZE;
        const float rateScale = static_cast<float>(analysisSampleRate)
                                / FFT_REFERENCE_FREQ;

        bool validSpectrum = true;
        for (int k = 1; k <= maxBin; ++k) {
            const float re = fftWork[2 * k];
            const float im = fftWork[2 * k + 1];
            const float magnitude = sqrtf(re * re + im * im) * magnitudeScale;
            if (!isfinite(re) || !isfinite(im) || !isfinite(magnitude)) {
                validSpectrum = false;
                break;
            }
            if (k >= openLow && k <= openHigh) vowelEnergy += magnitude;
            if (k >= closeLow && k <= closeHigh) sibilantEnergy += magnitude;
        }
        if (validSpectrum) {
            vowelEnergy *= rateScale;
            sibilantEnergy *= rateScale;
        } else {
            vowelEnergy = NAN;
            sibilantEnergy = NAN;
        }
    } else
#endif
    {
        // Graceful build/runtime fallback: jaw follows amplitude, while
        // sibilant-specific behavior is disabled.
        vowelEnergy = meanAbs;
        sibilantEnergy = 0.0f;
    }

    float jawRaw = vowelEnergy - envCfg.sibilantSuppression * sibilantEnergy;
    if (jawRaw < 0.0f) jawRaw = 0.0f;
    jawRaw *= envCfg.jawAnalysisGain;

    // A damaged frame must fail closed instead of latching a NaN into the
    // envelope and eventually into a servo command.
    if (!isfinite(vowelEnergy) || !isfinite(sibilantEnergy) ||
        !isfinite(jawRaw)) {
        envelope = 0.0f;
        envelopePeak = 200.0f;
        envelopeMin = 0.0f;
        lastVoiceMs = millis();
        publishMetrics(0.0f, 0.0f, 0.0f);
        static uint32_t lastInvalidLog = 0;
        if (millis() - lastInvalidLog > 1000) {
            lastInvalidLog = millis();
            Serial.println("[FFT] WARNING: invalid spectrum rejected; jaw forced closed");
        }
        return;
    }

    const float frameMs = 1000.0f * FFT_SIZE / analysisSampleRate;
    const float attack = scaledAlpha(envCfg.attackAlpha, frameMs);
    const float release = scaledAlpha(envCfg.releaseAlpha, frameMs);

    if (jawRaw > envelope) {
        envelope = attack * jawRaw + (1.0f - attack) * envelope;
    } else {
        envelope = release * jawRaw + (1.0f - release) * envelope;
    }
    if (!isfinite(envelope)) {
        envelope = 0.0f;
        envelopePeak = 200.0f;
        envelopeMin = 0.0f;
        publishMetrics(0.0f, 0.0f, 0.0f);
        return;
    }

    const uint32_t now = millis();
    if (envelope > envCfg.noiseFloor) {
        lastVoiceMs = now;
    } else if (now - lastVoiceMs > envCfg.holdMs) {
        // Decay toward silence between words. 0.88 per reference frame drops
        // the envelope to ~43% in a 200 ms word gap so the jaw visibly closes
        // between words instead of hovering open.
        const float referenceFrameMs = 1000.0f * FFT_SIZE / FFT_REFERENCE_FREQ;
        envelope *= powf(0.88f, frameMs / referenceFrameMs);
    }

    const float peakRise = scaledAlpha(0.05f, frameMs);
    const float peakFall = scaledAlpha(0.0008f, frameMs);
    const float minFollow = scaledAlpha(0.001f, frameMs);

    if (envelope > envelopePeak) {
        envelopePeak = (1.0f - peakRise) * envelopePeak + peakRise * envelope;
    } else {
        envelopePeak = (1.0f - peakFall) * envelopePeak + peakFall * envelope;
        if (envelopePeak < 50.0f) envelopePeak = 50.0f;
    }

    if (envelope < envelopeMin) {
        envelopeMin = envelope;
    } else {
        envelopeMin = (1.0f - minFollow) * envelopeMin + minFollow * envelope;
    }

    const float minF = envelopeMin + envCfg.noiseFloor;
    // Headroom above the tracked peak: sustained speech no longer maps to
    // ~100% open. Emphasized syllable onsets overshoot the lagging peak
    // tracker and still reach full open (normalized clamps to 1.0).
    const float headroom = envCfg.peakHeadroom >= 1.0f ? envCfg.peakHeadroom : 1.0f;
    float maxF = envelopePeak * headroom;
    if (maxF < minF + 10.0f) maxF = minF + 10.0f;

    float normalized = (envelope - minF) / (maxF - minF);
    if (!isfinite(normalized)) normalized = 0.0f;
    normalized = constrain(normalized, 0.0f, 1.0f);
    // Expansion curve (>1): pulls typical speech well below full open so the
    // jaw has real travel between words, while peaks still hit full open.
    const float curveExp = envCfg.jawCurveExp >= 0.25f ? envCfg.jawCurveExp : 1.0f;
    float jawFactor = powf(normalized, curveExp);
    if (!isfinite(jawFactor)) jawFactor = 0.0f;

    publishMetrics(vowelEnergy, sibilantEnergy, jawFactor);
}
