/*
 * File: ConfigManager.cpp
 * Purpose: Loads, validates, prints to files, and atomically saves skull.conf on LittleFS and microSD.
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

#include "ConfigManager.h"

#include "PinDefinitions.h"
#include "SDMutex.h"
#include <SD_MMC.h>

SkullConfig ConfigManager::config;
bool ConfigManager::loaded = false;
String ConfigManager::lastError;
bool ConfigManager::initialized = false;
bool ConfigManager::littleFSReady = false;
bool ConfigManager::sdReady = false;

namespace {
bool writeConfig(File& file, const SkullConfig& cfg, const char* heading) {
    if (!file) return false;
    file.clearWriteError();
    file.printf("# %s\n", heading);
    file.println("# Password values are intentionally not printed to Serial.");
    file.printf("wifi_mode=%s\n", cfg.wifi_mode.c_str());
    file.printf("ap_ssid=%s\n", cfg.ap_ssid.c_str());
    file.printf("ap_password=%s\n", cfg.ap_password.c_str());
    file.printf("ap_ip=%s\n", cfg.ap_ip.toString().c_str());
    file.printf("ap_gateway=%s\n", cfg.ap_gateway.toString().c_str());
    file.printf("ap_subnet=%s\n", cfg.ap_subnet.toString().c_str());
    file.printf("sta_ssid=%s\n", cfg.sta_ssid.c_str());
    file.printf("sta_password=%s\n", cfg.sta_password.c_str());
    file.printf("sta_ip=%s\n",
                cfg.sta_dhcp ? "dhcp" : cfg.sta_ip.toString().c_str());
    file.printf("sta_gateway=%s\n", cfg.sta_gateway.toString().c_str());
    file.printf("sta_subnet=%s\n", cfg.sta_subnet.toString().c_str());
    file.printf("volume=%u\n", cfg.volume);
    file.flush();
    return file.getWriteError() == 0;
}
}

String ConfigManager::trim(const String& value) {
    int start = 0;
    int end = value.length() - 1;
    while (start <= end && isspace(value[start])) ++start;
    while (end >= start && isspace(value[end])) --end;
    return start > end ? String() : value.substring(start, end + 1);
}

String ConfigManager::toLowerStr(String value) {
    value.toLowerCase();
    return value;
}

bool ConfigManager::parseIP(const String& value, IPAddress& out) {
    if (value.equalsIgnoreCase("dhcp") ||
        value.equalsIgnoreCase("0.0.0.0") || value.isEmpty()) {
        return false;
    }
    return out.fromString(value);
}

bool ConfigManager::begin() {
    if (initialized) return littleFSReady || sdReady;
    initialized = true;

    // Mount once. A mount failure is reported but never triggers an automatic
    // format, preventing transient errors from erasing configuration or WAVs.
    littleFSReady = LittleFS.begin(false, "/littlefs", 10, "littlefs");
    if (littleFSReady) {
        if (!LittleFS.exists("/wav")) LittleFS.mkdir("/wav");
    } else {
        Serial.println("[FS] ERROR: LittleFS mount failed; automatic format disabled");
        lastError += "LittleFS mount failed; ";
    }

    {
        SDLockGuard lock;
        SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN);
        const bool mounted = SD_MMC.begin("/sdcard", true, false);
        sdReady = mounted && SD_MMC.cardType() != CARD_NONE;
    }

    return littleFSReady || sdReady;
}

bool ConfigManager::load(const char* path) {
    config = SkullConfig();
    loaded = false;
    lastError = "";
    if (!begin()) {
        lastError = "No filesystem mounted; ";
        Serial.println("[Config] ERROR: no filesystem mounted; using defaults");
        return false;
    }

    File file;
    bool sdHeld = false;

    if (sdReady && sdLock()) {
        sdHeld = true;
        String candidates[] = {"/skull.conf", "/SKULL.CONF", String(path)};
        for (const auto& candidate : candidates) {
            if (SD_MMC.exists(candidate)) {
                file = SD_MMC.open(candidate, "r");
                if (file) {
                    break;
                }
            }
        }
        if (!file) {
            sdUnlock();
            sdHeld = false;
        }
    }

    if (!file && littleFSReady) {
        String candidates[] = {String(path), "/skull.conf"};
        for (const auto& candidate : candidates) {
            if (LittleFS.exists(candidate)) {
                file = LittleFS.open(candidate, "r");
                if (file) {
                    break;
                }
            }
        }
    }

    if (!file) {
        save(path);
        return false;
    }

    int lineNumber = 0;
    while (file.available()) {
        String raw = file.readStringUntil('\n');
        ++lineNumber;
        String line = raw;
        const int comment = line.indexOf('#');
        if (comment >= 0) line = line.substring(0, comment);
        line = trim(line);
        if (line.isEmpty()) continue;

        const int equals = line.indexOf('=');
        if (equals < 0) {
            String error = "Line " + String(lineNumber) + ": missing '='; ";
            if (lastError.length() < 200) lastError += error;
            Serial.println("[Config] ERROR: " + error);
            continue;
        }

        String key = toLowerStr(trim(line.substring(0, equals)));
        String value = trim(line.substring(equals + 1));

        if (key == "wifi_mode" || key == "mode") {
            value = toLowerStr(value);
            if (value == "ap" || value == "host" || value == "hotspot" ||
                value == "accesspoint") config.wifi_mode = "ap";
            else if (value == "sta" || value == "client" || value == "join" ||
                     value == "station") config.wifi_mode = "sta";
            else lastError += "Line " + String(lineNumber) + ": bad wifi_mode; ";
        } else if (key == "ap_ssid" || key == "host_ssid" || key == "ssid_ap") {
            config.ap_ssid = value;
        } else if (key == "ap_password" || key == "ap_pass" || key == "host_password") {
            config.ap_password = value;
        } else if (key == "ap_ip" || key == "host_ip") {
            IPAddress ip; if (parseIP(value, ip)) config.ap_ip = ip;
            else lastError += "Line " + String(lineNumber) + ": bad ap_ip; ";
        } else if (key == "ap_gateway" || key == "host_gateway") {
            IPAddress ip; if (parseIP(value, ip)) config.ap_gateway = ip;
            else lastError += "Line " + String(lineNumber) + ": bad ap_gateway; ";
        } else if (key == "ap_subnet" || key == "host_subnet") {
            IPAddress ip; if (parseIP(value, ip)) config.ap_subnet = ip;
            else lastError += "Line " + String(lineNumber) + ": bad ap_subnet; ";
        } else if (key == "sta_ssid" || key == "client_ssid" ||
                   key == "ssid_sta" || key == "ssid") {
            config.sta_ssid = value;
        } else if (key == "sta_password" || key == "sta_pass" ||
                   key == "client_password" || key == "password") {
            config.sta_password = value;
        } else if (key == "sta_ip" || key == "client_ip" ||
                   key == "fixed_ip" || key == "ip") {
            if (value.equalsIgnoreCase("dhcp")) {
                config.sta_dhcp = true;
                config.sta_ip = IPAddress(0,0,0,0);
            } else {
                IPAddress ip;
                if (parseIP(value, ip)) {
                    config.sta_ip = ip;
                    config.sta_dhcp = false;
                } else lastError += "Line " + String(lineNumber) + ": bad sta_ip; ";
            }
        } else if (key == "sta_gateway" || key == "client_gateway" || key == "gateway") {
            IPAddress ip; if (parseIP(value, ip)) config.sta_gateway = ip;
        } else if (key == "sta_subnet" || key == "client_subnet" ||
                   key == "subnet" || key == "netmask") {
            IPAddress ip; if (parseIP(value, ip)) config.sta_subnet = ip;
        } else if (key == "volume" || key == "vol") {
            const int parsed = value.toInt();
            if (parsed >= 0 && parsed <= 30) config.volume = parsed;
            else lastError += "Line " + String(lineNumber) + ": bad volume; ";
        }
    }

    file.close();
    if (sdHeld) sdUnlock();
    loaded = true;
    return true;
}

bool ConfigManager::save(const char* path) {
    if (!initialized && !begin()) return false;
    bool anySaved = false;

    if (littleFSReady) {
        const String destination(path);
        const String temporary = destination + ".tmp";
        LittleFS.remove(temporary);
        File file = LittleFS.open(temporary, "w");
        const bool wrote = writeConfig(file, config, "Talking Skull Config");
        if (file) file.close();

        bool renamed = false;
        if (wrote) {
            if (LittleFS.exists(destination)) LittleFS.remove(destination);
            renamed = LittleFS.rename(temporary, destination);
        }
        if (!renamed) {
            LittleFS.remove(temporary);
            Serial.println("[Config] ERROR: LittleFS config save failed");
        }
        anySaved |= renamed;
    }

    if (sdReady) {
        SDLockGuard lock;
        const String destination = "/skull.conf";
        const String temporary = destination + ".tmp";
        SD_MMC.remove(temporary);
        File file = SD_MMC.open(temporary, "w");
        const bool wrote = writeConfig(file, config, "Talking Skull Config");
        if (file) file.close();

        bool renamed = false;
        if (wrote) {
            if (SD_MMC.exists(destination)) SD_MMC.remove(destination);
            renamed = SD_MMC.rename(temporary, destination);
        }
        if (!renamed) {
            SD_MMC.remove(temporary);
            Serial.println("[Config] ERROR: SD config save failed");
        }
        anySaved |= renamed;
    }

    return anySaved;
}
