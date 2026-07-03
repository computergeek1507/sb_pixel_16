#pragma once

// These are defined in the .ino:
void configToJSON(String &out);
bool jsonToConfig(const String &json);
void saveConfig();

// ── Validation ────────────────────────────────────────────────────────────────

static bool validateConfig(const AppConfig &c, String &err) {
    if (c.protocol > PROTO_FSEQ) {
        err = "protocol invalid (0=E1.31, 1=DDP, 2=FSEQ)"; return false;
    }
    if (c.uniSize < 1 || c.uniSize > 512) {
        err = "universeSize must be 1-512"; return false;
    }
    if (c.dmxEnabled) {
        if (c.dmxStartCh < 1)                         { err = "dmxStartCh must be >= 1";   return false; }
        if (c.dmxChannels < 1 || c.dmxChannels > 512) { err = "dmxChannels must be 1-512"; return false; }
    }
    for (int i = 0; i < NUM_PORTS; i++) {
        if (c.ports[i].pixelCount < 1 || c.ports[i].pixelCount > MAX_PIXELS) {
            err = "port " + String(i) + " pixelCount out of range (1-" + MAX_PIXELS + ")";
            return false;
        }
        if (c.ports[i].startChannel < 1) {
            err = "port " + String(i) + " startChannel must be >= 1";
            return false;
        }
        if (c.ports[i].colorOrder > CO_BRG) {
            err = "port " + String(i) + " colorOrder invalid (0-5)";
            return false;
        }
        if (c.ports[i].nullPixels > MAX_NULL_PIXELS) {
            err = "port " + String(i) + " nullPixels max " + MAX_NULL_PIXELS;
            return false;
        }
        if (c.ports[i].grouping < 1 || c.ports[i].grouping > MAX_GROUPING) {
            err = "port " + String(i) + " grouping must be 1-" + MAX_GROUPING;
            return false;
        }
        if (c.ports[i].brightness > 100) {
            err = "port " + String(i) + " brightness must be 0-100";
            return false;
        }
    }
    return true;
}

// ── GET /api/config ───────────────────────────────────────────────────────────

void api_config_get(AsyncWebServerRequest *request) {
    String json;
    configToJSON(json);
    request->send(200, "application/json", json);
}

// ── POST /api/config ──────────────────────────────────────────────────────────

void api_config_post(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                     size_t index, size_t total)
{
    static String body;
    if (index == 0) body = "";
    body += String((char*)data, len);

    if (index + len < total) return;  // wait for full body

    AppConfig newCfg = cfg;
    if (!jsonToConfig(body)) {
        request->send(400, "application/json", "{\"error\":\"JSON parse error\"}");
        return;
    }

    String err;
    if (!validateConfig(cfg, err)) {
        cfg = newCfg;  // restore
        request->send(400, "application/json", "{\"error\":\"" + err + "\"}");
        return;
    }

    saveConfig();
    request->send(200, "application/json", "{\"ok\":true}");
}

// ── POST /api/reboot ──────────────────────────────────────────────────────────

void api_reboot(AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
}
