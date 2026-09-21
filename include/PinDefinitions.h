/*
 * File: PinDefinitions.h
 * Purpose: Defines the board-specific GPIO assignment and reserved-pin constraints.
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

/*
 * Talking Skull V5 pin map
 * Board: OceanLabz / GOOUUU-style ESP32-S3-WROOM-1-N16R8 CAM board
 *
 * Important fixed/reserved connections:
 * - GPIO35, GPIO36, GPIO37: internal Octal PSRAM -- never use
 * - GPIO38 CMD, GPIO39 CLK, GPIO40 D0: onboard SD_MMC in 1-bit mode
 * - GPIO19/GPIO20: native USB
 * - GPIO48: onboard WS2812
 * - GPIO0/GPIO3/GPIO45/GPIO46: boot/strapping pins; avoid where practical
 *
 * GPIO4-7 also reach the unused camera connector, but that grouped I2S route
 * remains under hardware investigation. This stabilization build deliberately
 * uses the user's currently verified MAX98357A wiring below.
 */

static const gpio_num_t JAW_PIN = GPIO_NUM_21;
#define JAW_FREQ_HZ 50
#define JAW_RESOLUTION 13

// MAX98357A I2S amplifier -- currently verified working baseline.
// I2S_DOUT_PIN is ESP32 data-out and connects to MAX98357A DIN.
// I2S_SD_PIN is MAX98357A SD_MODE/enable, not SD-card data.
static const gpio_num_t I2S_SD_PIN   = GPIO_NUM_1;
static const gpio_num_t I2S_DOUT_PIN = GPIO_NUM_2;
static const gpio_num_t I2S_BCLK_PIN = GPIO_NUM_42;
static const gpio_num_t I2S_LRC_PIN  = GPIO_NUM_41;

#define I2S_SAMPLE_RATE 16000
#define I2S_PORT I2S_NUM_0

// GC9A01 eyes on two independent SPI buses.
// Left eye - SPI2
static const gpio_num_t GC9A01_SCL_LEFT_PIN  = GPIO_NUM_15;
static const gpio_num_t GC9A01_SDA_LEFT_PIN  = GPIO_NUM_16;
static const gpio_num_t GC9A01_DC_LEFT_PIN   = GPIO_NUM_17;
static const gpio_num_t GC9A01_CS_LEFT_PIN   = GPIO_NUM_18;
static const gpio_num_t GC9A01_RES_LEFT_PIN  = GPIO_NUM_8;

// Right eye - SPI3
static const gpio_num_t GC9A01_SCL_RIGHT_PIN = GPIO_NUM_10;
static const gpio_num_t GC9A01_SDA_RIGHT_PIN = GPIO_NUM_11;
static const gpio_num_t GC9A01_DC_RIGHT_PIN  = GPIO_NUM_12;
static const gpio_num_t GC9A01_CS_RIGHT_PIN  = GPIO_NUM_13;
static const gpio_num_t GC9A01_RES_RIGHT_PIN = GPIO_NUM_14;

// Compatibility aliases used by older eye code.
static const gpio_num_t GC9A01_SCL_PIN  = GC9A01_SCL_LEFT_PIN;
static const gpio_num_t GC9A01_SDA_PIN  = GC9A01_SDA_LEFT_PIN;
static const gpio_num_t GC9A01_DC_PIN   = GC9A01_DC_LEFT_PIN;
static const gpio_num_t GC9A01_RES_PIN  = GC9A01_RES_LEFT_PIN;
static const gpio_num_t GC9A01_BLK_PIN  = GPIO_NUM_NC;

// Onboard microSD slot -- fixed board wiring, 1-bit SD_MMC mode.
static const gpio_num_t SD_MMC_CMD_PIN = GPIO_NUM_38;
static const gpio_num_t SD_MMC_CLK_PIN = GPIO_NUM_39;
static const gpio_num_t SD_MMC_D0_PIN  = GPIO_NUM_40;

