#pragma once
#include <Update.h>
#include "SBPixel16.h"

static void otaUpload(AsyncWebServerRequest*, size_t, uint8_t*, size_t, bool, int, bool);

void api_ota_firmware_upload(AsyncWebServerRequest *req, const String &filename,
                             size_t index, uint8_t *data, size_t len, bool final)
{
    otaUpload(req, index, data, len, final, U_FLASH, true /* size known */);
}

void api_ota_filesystem_upload(AsyncWebServerRequest *req, const String &filename,
                               size_t index, uint8_t *data, size_t len, bool final)
{
    if (index == 0) LittleFS.end();
    // FS image exactly fills its partition, so don't pass an inflated size
    // (content-length includes multipart framing) — that would over-run it.
    otaUpload(req, index, data, len, final, U_SPIFFS, false /* size unknown */);
}

static void otaUpload(AsyncWebServerRequest *req, size_t index,
                      uint8_t *data, size_t len, bool final, int command, bool sizeKnown)
{
    // Suspend pixel rendering during the upload (see loop()).
    g_otaActive = true;
    g_otaLastMs = millis();

    if (index == 0) {
        // Pass the total size so esp_ota_begin() pre-erases the region up front
        // in fast 64 KB block erases. With UPDATE_SIZE_UNKNOWN it instead erases
        // one 4 KB sector before every write, which throttled the drain to
        // ~16 KB/s and let AsyncTCP buffer up to a heap-exhausting ~320 KB
        // (the upload would reset mid-transfer). Content-length slightly exceeds
        // the image (multipart framing); Update.end(true) reconciles the size.
        size_t sz = (sizeKnown && req->contentLength()) ? req->contentLength()
                                                        : UPDATE_SIZE_UNKNOWN;
        if (!Update.begin(sz, command))
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
