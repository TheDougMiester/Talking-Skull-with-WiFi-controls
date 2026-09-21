/*
 * File: SDMutex.h
 * Purpose: Provides the shared recursive-safe guard used to serialize microSD access.
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
#include <freertos/semphr.h>

// All SD_MMC filesystem operations share this mutex. Keep each critical
// section short so the audio task can refill I2S DMA between directory entries
// and upload chunks.
extern SemaphoreHandle_t sdMutex;

inline bool sdLock(TickType_t timeout = portMAX_DELAY) {
    return !sdMutex || xSemaphoreTake(sdMutex, timeout) == pdTRUE;
}

inline void sdUnlock() {
    if (sdMutex) xSemaphoreGive(sdMutex);
}

class SDLockGuard {
public:
    explicit SDLockGuard(TickType_t timeout = portMAX_DELAY)
        : locked_(sdLock(timeout)) {}
    ~SDLockGuard() { if (locked_) sdUnlock(); }
    bool locked() const { return locked_; }

    SDLockGuard(const SDLockGuard&) = delete;
    SDLockGuard& operator=(const SDLockGuard&) = delete;

private:
    bool locked_;
};
