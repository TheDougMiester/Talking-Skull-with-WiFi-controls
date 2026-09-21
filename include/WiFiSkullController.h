/*
 * File: WiFiSkullController.h
 * Purpose: Declares web-controller commands, state, and the interface used by the main application.
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
#include "PinDefinitions.h"
#include <WiFi.h>
#include <WebServer.h>
#include "ConfigManager.h"

#define WIFI_AP_CHANNEL 6
#define WIFI_AP_MAX_CONN 4
#define SKULL_MAX_TRACK 255
#define SKULL_MIN_TRACK 1

//commands are file-based not number-based
enum WiFiSkullCommand : uint32_t {
    WIFI_CMD_NONE = 0,
    WIFI_CMD_PLAY_BASE = 1000,
    WIFI_CMD_STOP = 2000,
    WIFI_CMD_VOLUME_BASE = 3000,
    WIFI_CMD_LIVE_MIC_START = 4000,
    WIFI_CMD_LIVE_MIC_STOP = 4001,
    WIFI_CMD_PAUSE = 5000,
    WIFI_CMD_RESUME = 5001
};

class WiFiSkullController {
private:
    static uint32_t nReceivedValue;
    static bool hasNewCommand;
    static uint8_t lastTrack;
    static bool isPlaying;
    static bool isLiveMic;
    static uint8_t lastVolume;
    static char lastFileName[64];
    static char lastStatusMsg[128];
    static WebServer* server;
    static int eyeManualX;
    static int eyeManualY;
    static int eyeManualLid;
    static int eyeManualPupil;
    static uint32_t eyeManualUntil;
    static bool eyeManualActive;

    static void handleRoot();
    static void handleUploadPage();
    static void handleEyesPage();
    static void handleEyeControl();
    static void handleList();
    static void handlePlay();
    static void handleStop();
    static void handleVolume();
    static void handleStatus();
    static void handleUpload();
    static void handleUploadPost();
    static void handleDelete();
    static void handleNotFound();

    static String formatFileName(uint8_t track);
    static bool isValidVolume(int vol);

public:
    static void init();
    static void init(const SkullConfig& cfg);
    static void loop();

    static bool available(){ return hasNewCommand; }
    static void resetAvailable(){ hasNewCommand=false; nReceivedValue=0; }
    static uint32_t getReceivedValue(){ return nReceivedValue; }

    static void pushCommand(uint32_t cmd);
    static void setPlayingStatus(const char* fileName, bool playing, bool live=false);
    static void setPlayingStatus(uint8_t track, bool playing); // compat
    static void setStatusMessage(const char* msg, bool isError=false);
    static void setVolume(uint8_t vol);
    static void setFinishedMessage(const char* fileName);
    static void setLiveMicStatus(bool active);
    static const char* getFileName(){ return lastFileName; }
    static bool isLive(){ return isLiveMic; }

    // Eyes manual control
    static bool getEyeManual(int &x, int &y, int &lid);
    static bool getEyeManualFull(int &x, int &y, int &lid, int &pupil, bool &active);
};
