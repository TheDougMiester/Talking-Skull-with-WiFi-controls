/*
 * File: ConfigManager.h
 * Purpose: Declares persistent Wi-Fi, network, and volume configuration types.
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
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>

struct SkullConfig {
    String wifi_mode = "ap";
    String ap_ssid = "TalkingSkull";
    String ap_password = "12345678";
    IPAddress ap_ip = IPAddress(192,168,1,62);
    IPAddress ap_gateway = IPAddress(192,168,1,1);
    IPAddress ap_subnet = IPAddress(255,255,255,0);
    String sta_ssid = "";
    String sta_password = "";
    IPAddress sta_ip = IPAddress(0,0,0,0);
    IPAddress sta_gateway = IPAddress(192,168,1,1);
    IPAddress sta_subnet = IPAddress(255,255,255,0);
    bool sta_dhcp = true;
    uint8_t volume = 25;
};

class ConfigManager {
private:
    static String trim(const String& s);
    static String toLowerStr(String s);
    static bool parseIP(const String& str, IPAddress& out);
    static bool initialized;
    static bool littleFSReady;
    static bool sdReady;

public:
    static SkullConfig config;
    static bool loaded;
    static String lastError;

    // Idempotent: filesystems are mounted at most once during startup.
    // LittleFS is never formatted automatically.
    static bool begin();
    static bool load(const char* path = "/skull.conf");
    static bool save(const char* path = "/skull.conf");

    static SkullConfig& get() { return config; }
    static String getLastError() { return lastError; }
    static bool isLittleFSReady() { return littleFSReady; }
    static bool isSDReady() { return sdReady; }
};
