#pragma once
#include <Update.h>

static void otaUpload(AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool, int);

void api_ota_firmware_upload(AsyncWebServerRequest *req, const String &filename,
                             size_t index, uint8_t *data, size_t len, bool final)
{
    otaUpload(req, filename, index, data, len, final, U_FLASH);
}

void api_ota_filesystem_upload(AsyncWebServerRequest *req, const String &filename,
                               size_t index, uint8_t *data, size_t len, bool final)
{
    if (index == 0) LittleFS.end();
    otaUpload(req, filename, index, data, len, final, U_SPIFFS);
}

static void otaUpload(AsyncWebServerRequest*, const String&, size_t index,
                      uint8_t *data, size_t len, bool final, int command)
{
    if (index == 0) {
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, command))
            Serial.printf("OTA begin failed: %s\n", Update.errorString());
    }
    if (Update.isRunning())
        Update.write(data, len);
    if (final && !Update.end(true))
        Serial.printf("OTA end failed: %s\n", Update.errorString());
}

void api_ota_complete(AsyncWebServerRequest *req) {
    bool ok = !Update.hasError();
    req->send(ok ? 200 : 400, "application/json",
              ok ? "{\"ok\":true}"
                 : "{\"error\":\"" + String(Update.errorString()) + "\"}");
    if (ok) { delay(500); ESP.restart(); }
}
