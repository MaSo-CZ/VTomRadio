// v0.9.720
#include "options.h"
#include <ESPmDNS.h>
#include "time.h"
#include "rtcsupport.h"
#include "network.h"
#include "display.h"
#include "config.h"
#include "netserver.h"
#include "player.h"
#include "mqtt.h"
#include "timekeeper.h"
#include "../pluginsManager/pluginsManager.h"

#ifndef WIFI_CONNECT_TIMEOUT_MS
    #define WIFI_CONNECT_TIMEOUT_MS 45000
#endif

#ifndef SEARCH_WIFI_CORE_ID
    #define SEARCH_WIFI_CORE_ID 0
#endif

#ifndef WIFI_SCAN_TIMEOUT_MS
    #define WIFI_SCAN_TIMEOUT_MS 12000
#endif
MyNetwork network;
static portMUX_TYPE networkTimeMux = portMUX_INITIALIZER_UNLOCKED;

void network_get_timeinfo_snapshot(struct tm* out) {
    if (!out) return;
    portENTER_CRITICAL(&networkTimeMux);
    *out = network.timeinfo;
    portEXIT_CRITICAL(&networkTimeMux);
}

void network_set_timeinfo(const struct tm& in) {
    portENTER_CRITICAL(&networkTimeMux);
    network.timeinfo = in;
    portEXIT_CRITICAL(&networkTimeMux);
}

void MyNetwork::WiFiReconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
    network.justReconnected = true;
}

void MyNetwork::WiFiLostConnection(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (!network.beginReconnect) {
        network.disconnectStartMs = millis();
        network.lastScanAttemptMs = millis();
        network.scanningOtherNetworks = false;
        if (config.store.lastSSID > 0 && config.store.lastSSID <= config.ssidsCount) {
            Serial.printf("Lost connection, reconnecting to %s...\n", config.ssids[config.store.lastSSID - 1].ssid);
        } else {
            Serial.println("Lost connection, reconnecting...");
        }
        if (config.getMode() == PM_SDCARD) {
            network.status = SDREADY;
            display.putRequest(NEWIP, 0);
        } else {
            network.lostPlaying = (player.status() == PLAYING || player.isRunning());
            if (network.lostPlaying) {
                player.lockOutput = true;
                player.sendCommand({PR_STOP, 0});
            }
            display.putRequest(NEWMODE, LOST);
        }
        network.beginReconnect = true;
        WiFi.reconnect();
    }
}

void MyNetwork::loop() {
    if (justReconnected) {
        justReconnected = false;
        beginReconnect = false;
        scanningOtherNetworks = false;
        status = CONNECTED;
        player.lockOutput = false;
        setWifiParams();
        display.putRequest(NEWMODE, PLAYER);
        display.putRequest(NEWIP, 0);
        if (config.getMode() != PM_SDCARD) {
            if (lostPlaying) {
                lostPlaying = false;
                player.sendCommand({PR_PLAY, config.lastStation()});
            }
        }
#ifdef MQTT_ROOT_TOPIC
        connectToMqtt();
#endif
        return;
    }

    if (!beginReconnect) return;

    if (WiFi.status() == WL_CONNECTED) {
        justReconnected = true;
        return;
    }

    uint32_t now = millis();

    // 1. Szakadozó hálózat (Flaky connection):
    // Give 15s grace period for fast auto-reconnect to the same SSID.
    constexpr uint32_t GRACE_PERIOD_MS = 13000;
    constexpr uint32_t RESCAN_INTERVAL_MS = 12000;

    if (now - disconnectStartMs < GRACE_PERIOD_MS) {
        return;
    }

    // 2. Megszűnt hálózat (Lost connection / Hotspot turned off):
    // Periodically scan and attempt all other saved Wi-Fi networks in wifi.csv.
    if (now - lastScanAttemptMs >= RESCAN_INTERVAL_MS) {
        lastScanAttemptMs = now;
        scanningOtherNetworks = true;
        Serial.println("[WIFI] Reconnect grace period expired. Scanning for all saved Wi-Fi networks...");

        if (wifiBegin(true)) {
            if (config.store.lastSSID > 0 && config.store.lastSSID <= config.ssidsCount) {
                Serial.printf("[WIFI] Successfully reconnected to %s!\n", config.ssids[config.store.lastSSID - 1].ssid);
            }
            justReconnected = true;
        } else {
            Serial.println("[WIFI] No saved Wi-Fi network in range. Retrying scan in 12s...");
        }
    }
}

uint8_t MyNetwork::getSortedSavedWifiCandidates(uint8_t* outCandidates, uint8_t maxCandidates) {
    if (!outCandidates || maxCandidates == 0 || config.ssidsCount == 0) return 0;

    struct Candidate {
        uint8_t index;
        int32_t rssi;
    };

    Candidate candidates[5];
    uint8_t count = 0;

    WiFi.scanDelete();
    int networks = WiFi.scanNetworks(true, true);
    const uint32_t scanStart = millis();
    while (networks == WIFI_SCAN_RUNNING && millis() - scanStart < WIFI_SCAN_TIMEOUT_MS) {
        delay(50);
        networks = WiFi.scanComplete();
    }

    bool rejectedByRssi = false;

    if (networks > 0) {
        // Hidden APs report an empty SSID; remember the strongest one
        int32_t hiddenRssi = -128;
        bool hiddenSeen = false;
        for (int networkIndex = 0; networkIndex < networks; ++networkIndex) {
            if (WiFi.SSID(networkIndex).length() == 0) {
                int32_t r = WiFi.RSSI(networkIndex);
                if (!hiddenSeen || r > hiddenRssi) {
                    hiddenRssi = r;
                    hiddenSeen = true;
                }
            }
        }

        for (uint8_t savedIndex = 0; savedIndex < config.ssidsCount; ++savedIndex) {
            int32_t bestRssiForSsid = -128;
            bool found = false;
            for (int networkIndex = 0; networkIndex < networks; ++networkIndex) {
                if (WiFi.SSID(networkIndex) == config.ssids[savedIndex].ssid) {
                    int32_t r = WiFi.RSSI(networkIndex);
                    if (r > bestRssiForSsid) {
                        bestRssiForSsid = r;
                        found = true;
                    }
                }
            }
            if (!found && hiddenSeen) {
                bestRssiForSsid = hiddenRssi;
                found = true;
            }
            if (found && bestRssiForSsid < config.store.wifiMinRssi) {
                rejectedByRssi = true;
            } else if (found && count < 5) {
                candidates[count].index = savedIndex;
                candidates[count].rssi = bestRssiForSsid;
                count++;
            }
        }
        WiFi.scanDelete();

        // Sort candidates by RSSI descending
        for (uint8_t i = 0; i < count; i++) {
            for (uint8_t j = i + 1; j < count; j++) {
                if (candidates[j].rssi > candidates[i].rssi) {
                    Candidate tmp = candidates[i];
                    candidates[i] = candidates[j];
                    candidates[j] = tmp;
                }
            }
        }
    } else {
        WiFi.scanDelete();
    }

    // No candidates: fall back to lastSSID unless networks were seen but too weak
    if (count == 0) {
        if (!rejectedByRssi) {
            uint8_t startIdx = (config.store.lastSSID > 0 && config.store.lastSSID <= config.ssidsCount)
                ? (config.store.lastSSID - 1)
                : 0;
            for (uint8_t i = 0; i < config.ssidsCount && count < maxCandidates; i++) {
                uint8_t idx = (startIdx + i) % config.ssidsCount;
                outCandidates[count++] = idx;
            }
            return count;
        } else {
            return 0;
        }
    }

    uint8_t retCount = 0;
    for (uint8_t i = 0; i < count && retCount < maxCandidates; i++) {
        outCandidates[retCount++] = candidates[i].index;
    }

    // Append any remaining saved SSIDs not found in scan as last resort
    for (uint8_t s = 0; s < config.ssidsCount && retCount < maxCandidates; s++) {
        bool alreadyIn = false;
        for (uint8_t c = 0; c < retCount; c++) {
            if (outCandidates[c] == s) {
                alreadyIn = true;
                break;
            }
        }
        if (!alreadyIn) {
            outCandidates[retCount++] = s;
        }
    }

    return retCount;
}

bool MyNetwork::wifiBegin(bool silent) {
    if (config.ssidsCount == 0) return false;

    // Hard Reset WiFi before starting
    if (WiFi.getMode() != WIFI_MODE_NULL) {
        WiFi.disconnect(true);
    }
    WiFi.mode(WIFI_OFF);
    delay(500);
    WiFi.mode(WIFI_STA);
    delay(250);

    if (!silent) {
        Serial.println("##[BOOT]#\tScanning available Wi-Fi networks...");
        display.putRequest(BOOTWIFISCAN);
    }

    uint8_t candidates[5];
    uint8_t numCandidates = getSortedSavedWifiCandidates(candidates, 5);
    if (numCandidates == 0) {
        return false;
    }

    for (uint8_t attempt = 0; attempt < numCandidates; attempt++) {
        uint8_t ls = candidates[attempt];

        if (!silent) {
            Serial.printf("##[BOOT]#\tAttempt to connect to [%d/%d] %s\n", attempt + 1, numCandidates, config.ssids[ls].ssid);
            Serial.print("##[BOOT]#\t");
            display.putRequest(BOOTSTRING, ls);
        }

        if (attempt > 0) {
            WiFi.disconnect(true);
            delay(100);
        }

        const char* pass = (strlen(config.ssids[ls].password) > 0) ? config.ssids[ls].password : NULL;
        WiFi.begin(config.ssids[ls].ssid, pass);
        const uint32_t connectStartMs = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - connectStartMs < WIFI_CONNECT_TIMEOUT_MS) {
            if (!silent) Serial.print(".");
            vTaskDelay(pdMS_TO_TICKS(500));
            if (REAL_LEDBUILTIN != 255 && !silent) digitalWrite(REAL_LEDBUILTIN, !digitalRead(REAL_LEDBUILTIN));
        }

        if (WiFi.status() == WL_CONNECTED) {
            config.setLastSSID(ls + 1);
            return true;
        }
        if (!silent) {
            Serial.printf("\n##[BOOT]#\tTimed out connecting to %s after %lu ms\n",
                config.ssids[ls].ssid, static_cast<unsigned long>(WIFI_CONNECT_TIMEOUT_MS));
        }
    }

    return false;
}

void searchWiFi(void* pvParameters) {
    if (!network.wifiBegin(true)) {
        delay(10000);
        xTaskCreatePinnedToCore(searchWiFi, "searchWiFi", 1024 * 4, NULL, 0, NULL, SEARCH_WIFI_CORE_ID); 
    } else {
        network.status = CONNECTED;
        netserver.begin(true);
        network.setWifiParams();
        display.putRequest(NEWIP, 0);
#ifdef MQTT_ROOT_TOPIC
        mqttInit();
#endif
    }
    vTaskDelete(NULL);
}

#define DBGAP false

void MyNetwork::begin() {
    BOOTLOG("network.begin");
    config.initNetwork();
    if (config.ssidsCount == 0 || DBGAP) {
        raiseSoftAP();
        return;
    }
    if (config.getMode() != PM_SDCARD) {
        if (!wifiBegin()) {
            raiseSoftAP();
            Serial.println("##[BOOT]#\tdone");
            return;
        }
        Serial.println(".");
        status = CONNECTED;
        setWifiParams();
#ifdef MQTT_ROOT_TOPIC
        mqttInit();
#endif
    } else {
        status = SDREADY;
        xTaskCreatePinnedToCore(searchWiFi, "searchWiFi", 1024 * 4, NULL, 0, NULL, SEARCH_WIFI_CORE_ID); 
    }

    Serial.println("##[BOOT]#\tdone");
    if (REAL_LEDBUILTIN != 255) digitalWrite(REAL_LEDBUILTIN, LOW);

#if RTCSUPPORTED
    if (config.isRTCFound()) {
        tm currentTime{};
        rtc.getTime(&currentTime);
        mktime(&currentTime);
        network_set_timeinfo(currentTime);
        display.putRequest(CLOCK);
    }
#endif
    if (network_on_connect) network_on_connect();
    pm.on_connect();
}

void MyNetwork::setWifiParams() {
    static bool wifiEventsRegistered = false;
    WiFi.setSleep(false);
    if (!wifiEventsRegistered) {
        WiFi.onEvent(WiFiReconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
        WiFi.onEvent(WiFiLostConnection, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        wifiEventsRegistered = true;
    }
    // config.setTimeConf(); //??
    if (strlen(config.store.mdnsname) > 0) {
        static bool mdnsStarted = false;
        if (!mdnsStarted) {
            if (MDNS.begin(config.store.mdnsname)) {
                MDNS.addService("http", "tcp", 80);
                mdnsStarted = true;
            }
        }
    }
    Serial.printf("##[BOOT]#\tWeb UI: http://%s/\n", WiFi.localIP().toString().c_str());
}

void MyNetwork::requestTimeSync() {
}

void rebootTime() {
    ESP.restart();
}

void MyNetwork::raiseSoftAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apSsid, apPassword);
    Serial.println("##[BOOT]#");
    BOOTLOG("************************************************");
    BOOTLOG("Running in AP mode");
    BOOTLOG("Connect to AP %s with password %s", apSsid, apPassword);
    BOOTLOG("and go to http://192.168.4.1/ to configure");
    Serial.println("##[BOOT]#\tWeb UI (AP): http://192.168.4.1/");
    BOOTLOG("************************************************");
    status = SOFT_AP;
    if (config.store.softapdelay > 0) timekeeper.waitAndDo(config.store.softapdelay * 60, rebootTime);
}

void MyNetwork::requestWeatherSync() {
    display.putRequest(NEWWEATHER);
}
