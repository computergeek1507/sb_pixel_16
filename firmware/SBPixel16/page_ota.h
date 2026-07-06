#pragma once
#include <Update.h>
#include "SBPixel16.h"

// Streaming OTA: write each incoming chunk straight to flash. This board's flash
// erase is slow and AsyncTCP has no backpressure, so a full-speed upload buffers
// in heap until it exhausts (~200-320 KB) and resets — throttle the client
// (curl --limit-rate 32k, or the paced web uploader) to keep the drain matched.
// NOTE: a PSRAM-buffered variant was tried but crashes under heavy Ethernet RX
// (DMA/PSRAM hazard), so streaming is the reliable path on this hardware.

static void otaUpload(AsyncWebServerRequest*, size_t, uint8_t*, size_t, bool, int);

void api_ota_firmware_upload(AsyncWebServerRequest *req, const String &filename,
                             size_t index, uint8_t *data, size_t len, bool final)
{
    otaUpload(req, index, data, len, final, U_FLASH);
}

void api_ota_filesystem_upload(AsyncWebServerRequest *req, const String &filename,
                               size_t index, uint8_t *data, size_t len, bool final)
{
    if (index == 0) LittleFS.end();
    otaUpload(req, index, data, len, final, U_SPIFFS);
}

static void otaUpload(AsyncWebServerRequest*, size_t index,
                      uint8_t *data, size_t len, bool final, int command)
{
    // Suspend pixel rendering during the upload (see loop()).
    g_otaActive = true;
    g_otaLastMs = millis();
    g_otaMode   = 2;   // streaming

    if (index == 0) {
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, command))
            Serial.printf("OTA begin failed: %s\n", Update.errorString());
    }
    if (Update.isRunning())
        Update.write(data, len);
    if (final) {
        if (!Update.end(true))
            Serial.printf("OTA end failed: %s\n", Update.errorString());
        g_otaActive = false;
    }
}

void api_ota_complete(AsyncWebServerRequest *req) {
    bool ok = !Update.hasError();
    req->send(ok ? 200 : 400, "application/json",
              ok ? "{\"ok\":true}"
                 : "{\"error\":\"" + String(Update.errorString()) + "\"}");
    if (ok) { delay(500); ESP.restart(); }
}
