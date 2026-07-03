/*
 * SBPixel16.ino
 *
 * 16-port WS2812 pixel controller for ESP32-P4-EV
 * Input: E1.31 (sACN) or DDP over Ethernet
 * Config: web UI + REST API, stored in LittleFS as /config.json
 * Output: PARLIO parallel DMA — all 16 strips driven simultaneously
 *
 * Libraries required (install via Arduino Library Manager):
 *   ESPAsyncWebServer  github.com/me-no-dev/ESPAsyncWebServer
 *   AsyncTCP           github.com/me-no-dev/AsyncTCP
 *   ArduinoJson        arduinojson.org
 *
 * Board: ESP32-P4 (Arduino-ESP32 3.x)
 * Partition: Default 4MB with SPIFFS (1.2MB APP / 1.5MB FS)
 */

#include <ETH.h>
#include <NetworkUdp.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "SBPixel16.h"
#include "parlio_ws2812.h"
#include "dmx_output.h"
#include "fseq_player.h"
#include "net_menu.h"
#include "page_index.h"
#include "page_config.h"
#include "page_ota.h"

// ── Globals ───────────────────────────────────────────────────────────────────

AppConfig cfg;

// Raw channel data received from E1.31 / DDP (RGB, per logical pixel)
static uint8_t rawBuf[NUM_PORTS][MAX_PIXELS * 3];

uint32_t g_e131Packets = 0;
uint32_t g_ddpPackets  = 0;
uint32_t g_fps         = 0;
uint32_t g_lastPacket  = 0;
uint32_t g_vin1_mv     = 0;
uint32_t g_vin2_mv     = 0;
uint8_t  g_testMode    = 0;

bool     g_sdMounted   = false;
uint32_t g_sdSizeMB    = 0;
uint16_t g_fseqCount   = 0;
uint32_t g_fseqFrame   = 0;
uint32_t g_fseqFrames  = 0;
char     g_fseqName[32] = {0};

static bool          s_ethConnected = false;
static AsyncWebServer web(HTTP_PORT);
static NetworkUDP    e131Udp;
static NetworkUDP    ddpUdp;
static ParlioWs2812  parlio;
static DmxOutput     dmx;
static FseqPlayer    fseq;
static NetMenu       netMenu;

static uint8_t dmxBuf[512];

static Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
static bool             s_oledFound = false;

// ── E1.31 packet buffer (638 bytes) ──────────────────────────────────────────
static uint8_t e131Buf[638];

static const uint8_t ACN_ID[12] = {
    0x41,0x53,0x43,0x2d,0x45,0x31,0x2e,0x31,0x37,0x00,0x00,0x00
};

// ── DDP packet buffer ─────────────────────────────────────────────────────────
static uint8_t ddpBuf[1450];

// ── Config load / save ────────────────────────────────────────────────────────

void configToJSON(String &out) {
    JsonDocument doc;

    JsonObject net   = doc["network"].to<JsonObject>();
    net["dhcp"]      = cfg.dhcp;
    net["ip"]        = cfg.ip;
    net["subnet"]    = cfg.subnet;
    net["gateway"]   = cfg.gateway;

    doc["protocol"]      = cfg.protocol;
    doc["universeStart"] = cfg.uniStart;
    doc["universeSize"]  = cfg.uniSize;

    JsonArray ports = doc["ports"].to<JsonArray>();
    for (int i = 0; i < NUM_PORTS; i++) {
        JsonObject p      = ports.add<JsonObject>();
        p["pixelCount"]   = cfg.ports[i].pixelCount;
        p["startChannel"] = cfg.ports[i].startChannel;
        p["colorOrder"]   = cfg.ports[i].colorOrder;
        p["nullPixels"]   = cfg.ports[i].nullPixels;
        p["grouping"]     = cfg.ports[i].grouping;
        p["brightness"]   = cfg.ports[i].brightness;
    }

    doc["dmxEnabled"]  = cfg.dmxEnabled;
    doc["dmxStartCh"]  = cfg.dmxStartCh;
    doc["dmxChannels"] = cfg.dmxChannels;

    serializeJson(doc, out);
}

bool jsonToConfig(const String &json) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;

    JsonObject net = doc["network"];
    if (!net.isNull()) {
        if (net["dhcp"].is<bool>())           cfg.dhcp    = net["dhcp"].as<bool>();
        if (net["ip"].is<const char*>())      cfg.ip      = net["ip"].as<String>();
        if (net["subnet"].is<const char*>())  cfg.subnet  = net["subnet"].as<String>();
        if (net["gateway"].is<const char*>()) cfg.gateway = net["gateway"].as<String>();
    }

    if (doc["protocol"].is<int>())      cfg.protocol = doc["protocol"].as<uint8_t>();
    if (doc["universeStart"].is<int>()) cfg.uniStart = doc["universeStart"].as<uint16_t>();
    if (doc["universeSize"].is<int>())  cfg.uniSize  = doc["universeSize"].as<uint16_t>();

    JsonArray ports = doc["ports"].as<JsonArray>();
    int i = 0;
    for (JsonObject p : ports) {
        if (i >= NUM_PORTS) break;
        if (p["pixelCount"].is<int>())   cfg.ports[i].pixelCount   = p["pixelCount"].as<uint16_t>();
        if (p["startChannel"].is<int>()) cfg.ports[i].startChannel = p["startChannel"].as<uint32_t>();
        if (p["colorOrder"].is<int>())   cfg.ports[i].colorOrder   = p["colorOrder"].as<uint8_t>();
        if (p["nullPixels"].is<int>())   cfg.ports[i].nullPixels   = p["nullPixels"].as<uint8_t>();
        if (p["grouping"].is<int>())     cfg.ports[i].grouping     = p["grouping"].as<uint8_t>();
        if (p["brightness"].is<int>())   cfg.ports[i].brightness   = p["brightness"].as<uint8_t>();
        i++;
    }
    if (doc["dmxEnabled"].is<bool>())  cfg.dmxEnabled  = doc["dmxEnabled"].as<bool>();
    if (doc["dmxStartCh"].is<int>())   cfg.dmxStartCh  = doc["dmxStartCh"].as<uint32_t>();
    if (doc["dmxChannels"].is<int>())  cfg.dmxChannels = doc["dmxChannels"].as<uint16_t>();

    return true;
}

void defaultConfig() {
    cfg.dhcp     = true;
    cfg.ip       = "192.168.1.200";
    cfg.subnet   = "255.255.255.0";
    cfg.gateway  = "192.168.1.1";
    cfg.protocol   = PROTO_E131;
    cfg.uniStart   = 1;
    cfg.uniSize    = 510;
    for (int i = 0; i < NUM_PORTS; i++) {
        cfg.ports[i].pixelCount   = 100;
        cfg.ports[i].startChannel = (uint32_t)(i * 510) + 1;
        cfg.ports[i].colorOrder   = CO_RGB;
        cfg.ports[i].nullPixels   = 0;
        cfg.ports[i].grouping     = 1;
        cfg.ports[i].brightness   = 30;
    }
}

void saveConfig() {
    String json;
    configToJSON(json);
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (f) { f.print(json); f.close(); }
    else    Serial.println("saveConfig: open failed");
}

void loadConfig() {
    defaultConfig();
    if (!LittleFS.exists(CONFIG_FILE)) {
        Serial.println("No config — writing defaults");
        saveConfig();
        return;
    }
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) return;
    String json = f.readString();
    f.close();
    if (!jsonToConfig(json))
        Serial.println("Config parse failed — using defaults");
}

// ── Ethernet ──────────────────────────────────────────────────────────────────

void ethEvent(arduino_event_id_t event) {
    switch (event) {
    case ARDUINO_EVENT_ETH_START:
        Serial.println("ETH: started");
        ETH.setHostname("SBPixel16");
        break;
    case ARDUINO_EVENT_ETH_CONNECTED:
        Serial.println("ETH: link up");
        updateOLED();
        break;
    case ARDUINO_EVENT_ETH_GOT_IP:
        Serial.print("ETH: IP = "); Serial.println(ETH.localIP());
        s_ethConnected = true;
        e131Udp.begin(E131_UDP_PORT);
        ddpUdp.begin(DDP_UDP_PORT);
        Serial.println("UDP listeners started");
        updateOLED();
        break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
        Serial.println("ETH: link down");
        s_ethConnected = false;
        updateOLED();
        break;
    default: break;
    }
}

void setupEthernet() {
    Network.onEvent(ethEvent);

    if (!cfg.dhcp) {
        IPAddress ipAddr, ipSub, ipGw;
        if (!ipAddr.fromString(cfg.ip) || !ipSub.fromString(cfg.subnet) || !ipGw.fromString(cfg.gateway)) {
            Serial.println("Invalid IP in config — using defaults");
            ipAddr.fromString("192.168.1.200");
            ipSub.fromString("255.255.255.0");
            ipGw.fromString("192.168.1.1");
        }
        ETH.config(ipAddr, ipGw, ipSub);
        Serial.print("ETH: static IP = "); Serial.println(cfg.ip);
    } else {
        Serial.println("ETH: DHCP enabled");
    }

    ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO,
              ETH_PHY_POWER, ETH_CLK_MODE);
}

// ── OLED ──────────────────────────────────────────────────────────────────────

void updateOLED() {
    if (!s_oledFound) return;
    if (netMenu.active()) { netMenu.render(oled); return; }
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    oled.setCursor(0, 0);
    oled.print("SBPixel16");
    if (g_testMode > 0) {
        oled.setCursor(54, 0);
        oled.print("[TEST:");
        oled.print(testModeName());
        oled.print("]");
    }

    oled.setCursor(0, 9);
    if (s_ethConnected) {
        oled.print(ETH.localIP().toString());
    } else {
        oled.print(cfg.dhcp ? "DHCP: waiting..." : "No link");
    }

    oled.setCursor(0, 18);
    if (cfg.protocol == PROTO_FSEQ) {
        oled.print("FSEQ  FPS:");
        oled.print(g_fps);
        oled.setCursor(0, 27);
        if (!g_sdMounted)         oled.print("No SD card");
        else if (g_fseqCount == 0) oled.print("No sequences");
        else {
            oled.print(g_fseqName);
            oled.setCursor(0, 45);
            oled.print(g_fseqFrame);
            oled.print("/");
            oled.print(g_fseqFrames);
        }
    } else {
        oled.print(cfg.protocol == PROTO_DDP ? "DDP" : "E1.31");
        oled.print("  FPS:");
        oled.print(g_fps);

        oled.setCursor(0, 27);
        oled.print("Pkts:");
        oled.print(cfg.protocol == PROTO_DDP ? g_ddpPackets : g_e131Packets);
    }

    oled.setCursor(0, 36);
    char vbuf[22];
    snprintf(vbuf, sizeof(vbuf), "V1:%u.%02uV V2:%u.%02uV",
             g_vin1_mv / 1000, (g_vin1_mv % 1000) / 10,
             g_vin2_mv / 1000, (g_vin2_mv % 1000) / 10);
    oled.print(vbuf);

    oled.display();
}

void setupOLED() {
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("OLED not found — display disabled");
        return;
    }
    s_oledFound = true;
    oled.setRotation(2);
    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("SBPixel16");
    oled.setTextSize(1);
    oled.setCursor(0, 20);
    oled.println("Starting...");
    oled.display();
    Serial.println("OLED: SSD1306 128x64 found");
}

// ── PARLIO LED output ─────────────────────────────────────────────────────────

void setupParlio() {
    if (!parlio.begin(cfg))
        Serial.println("PARLIO init failed!");
}

// ── DMX output ────────────────────────────────────────────────────────────────

void setupDmx() {
    if (!cfg.dmxEnabled) return;
    dmx.begin(DMX_TX_PIN);
}

// ── E1.31 packet parsing ──────────────────────────────────────────────────────

static void parseE131() {
    int pktLen = e131Udp.parsePacket();
    if (pktLen < 638) return;
    e131Udp.read(e131Buf, sizeof(e131Buf));

    if (memcmp(&e131Buf[4], ACN_ID, 12) != 0) return;
    uint32_t rootVec  = ((uint32_t)e131Buf[18] << 24) | ((uint32_t)e131Buf[19] << 16) |
                        ((uint32_t)e131Buf[20] << 8)  |  e131Buf[21];
    uint32_t frameVec = ((uint32_t)e131Buf[40] << 24) | ((uint32_t)e131Buf[41] << 16) |
                        ((uint32_t)e131Buf[42] << 8)  |  e131Buf[43];
    if (rootVec != 0x04 || frameVec != 0x02) return;
    if (e131Buf[117] != 0x02) return;
    if (e131Buf[125] != 0x00) return;

    uint16_t universe  = ((uint16_t)e131Buf[113] << 8) | e131Buf[114];
    const uint8_t *chanData = &e131Buf[126];
    uint16_t chanCount = (uint16_t)(((e131Buf[123] << 8) | e131Buf[124]) - 1);
    if (chanCount > 512) chanCount = 512;

    if (universe < cfg.uniStart) return;

    for (int port = 0; port < NUM_PORTS; port++) {
        uint32_t portAbs0 = cfg.ports[port].startChannel - 1;
        uint32_t portAbs1 = portAbs0 + (uint32_t)cfg.ports[port].pixelCount * 3 - 1;
        uint32_t uniAbs0  = (uint32_t)(universe - cfg.uniStart) * cfg.uniSize;
        uint32_t uniAbs1  = uniAbs0 + chanCount - 1;

        uint32_t ov0 = max(portAbs0, uniAbs0);
        uint32_t ov1 = min(portAbs1, uniAbs1);
        if (ov0 > ov1) continue;

        uint32_t copyLen  = ov1 - ov0 + 1;
        uint32_t uniOff   = ov0 - uniAbs0;
        uint32_t portOff  = ov0 - portAbs0;

        memcpy(rawBuf[port] + portOff, chanData + uniOff, copyLen);
    }

    // DMX output fill
    if (cfg.dmxEnabled) {
        uint32_t dmxAbs0 = cfg.dmxStartCh - 1;
        uint32_t dmxAbs1 = dmxAbs0 + 511;
        uint32_t uniAbs0 = (uint32_t)(universe - cfg.uniStart) * cfg.uniSize;
        uint32_t uniAbs1 = uniAbs0 + chanCount - 1;
        uint32_t ov0 = max(dmxAbs0, uniAbs0);
        uint32_t ov1 = min(dmxAbs1, uniAbs1);
        if (ov0 <= ov1)
            memcpy(dmxBuf + (ov0 - dmxAbs0), chanData + (ov0 - uniAbs0), ov1 - ov0 + 1);
    }

    g_lastPacket = millis();
    g_e131Packets++;
}

// ── DDP packet parsing ────────────────────────────────────────────────────────

static void parseDDP() {
    int pktLen = ddpUdp.parsePacket();
    if (pktLen < 10) return;
    int len = ddpUdp.read(ddpBuf, sizeof(ddpBuf));

    uint8_t  flags  = ddpBuf[0];
    uint8_t  type   = ddpBuf[2];
    uint32_t offset = ((uint32_t)ddpBuf[4] << 24) | ((uint32_t)ddpBuf[5] << 16) |
                      ((uint32_t)ddpBuf[6] << 8)  |  ddpBuf[7];
    uint16_t dlen   = ((uint16_t)ddpBuf[8] << 8)  |  ddpBuf[9];

    if (!(flags & 0x01)) return;
    if (type != 0x01)    return;

    if (10 + dlen > len) dlen = (uint16_t)(len - 10);
    const uint8_t *data = &ddpBuf[10];

    uint32_t pktAbs0 = offset;
    uint32_t pktAbs1 = offset + dlen - 1;

    for (int port = 0; port < NUM_PORTS; port++) {
        uint32_t portAbs0 = cfg.ports[port].startChannel - 1;
        uint32_t portAbs1 = portAbs0 + (uint32_t)cfg.ports[port].pixelCount * 3 - 1;

        uint32_t ov0 = max(portAbs0, pktAbs0);
        uint32_t ov1 = min(portAbs1, pktAbs1);
        if (ov0 > ov1) continue;

        uint32_t copyLen = ov1 - ov0 + 1;
        uint32_t pktOff  = ov0 - pktAbs0;
        uint32_t portOff = ov0 - portAbs0;

        memcpy(rawBuf[port] + portOff, data + pktOff, copyLen);
    }

    // DMX output fill
    if (cfg.dmxEnabled) {
        uint32_t dmxAbs0 = cfg.dmxStartCh - 1;
        uint32_t dmxAbs1 = dmxAbs0 + 511;
        uint32_t ov0 = max(dmxAbs0, pktAbs0);
        uint32_t ov1 = min(dmxAbs1, pktAbs1);
        if (ov0 <= ov1)
            memcpy(dmxBuf + (ov0 - dmxAbs0), data + (ov0 - pktAbs0), ov1 - ov0 + 1);
    }

    g_lastPacket = millis();
    g_ddpPackets++;
}

// ── ADC ───────────────────────────────────────────────────────────────────────

static uint32_t s_adcTimer = 0;

static uint32_t oversample(int pin) {
    uint32_t sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) sum += analogReadMilliVolts(pin);
    return sum / ADC_SAMPLES;
}

void readADC() {
    uint32_t now = millis();
    if (now - s_adcTimer < 500) return;
    s_adcTimer = now;
    g_vin1_mv = oversample(VIN1_PIN);
    g_vin2_mv = oversample(VIN2_PIN);
}

void setupADC() {
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    g_vin1_mv = oversample(VIN1_PIN);
    g_vin2_mv = oversample(VIN2_PIN);
}

// ── Buttons ───────────────────────────────────────────────────────────────────

static bool     s_b1Prev = false;   // pressed = true
static bool     s_b2Prev = false;
static uint32_t s_b1Down = 0;
static uint32_t s_b1Rep  = 0;
static uint32_t s_b2Last = 0;
static bool     s_b1Consumed = false;   // long-press-to-enter consumed this hold

static const char* testModeName() {
    switch (g_testMode) {
        case 1: return "RED";
        case 2: return "GREEN";
        case 3: return "BLUE";
        case 4: return "RAINBOW";
        default: return "OFF";
    }
}

void setupButtons() {
    pinMode(BTN1_PIN, INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);
}

void checkButtons() {
    uint32_t now = millis();
    bool b1 = (digitalRead(BTN1_PIN) == LOW);   // active-LOW
    bool b2 = (digitalRead(BTN2_PIN) == LOW);

    // ── BTN1 ──────────────────────────────────────────────────────────────────
    if (b1 && !s_b1Prev) {              // press edge
        s_b1Down = now;
        s_b1Rep  = now;
        s_b1Consumed = false;
    }
    if (b1) {                           // held
        if (!netMenu.active()) {
            // Long-press opens the network menu.
            if (!s_b1Consumed && (now - s_b1Down) >= LONGPRESS_MS) {
                netMenu.enter();
                s_b1Consumed = true;    // suppress the test-cycle on release
                updateOLED();
            }
        } else if (!s_b1Consumed &&
                   (now - s_b1Down) >= REPEAT_DELAY_MS &&
                   (now - s_b1Rep)  >= REPEAT_RATE_MS) {
            s_b1Rep = now;              // hold-to-repeat value changes
            netMenu.change();
            updateOLED();
        }
    }
    if (!b1 && s_b1Prev) {              // release edge
        uint32_t held = now - s_b1Down;
        if (netMenu.active()) {
            if (!s_b1Consumed && held < REPEAT_DELAY_MS) { netMenu.change(); updateOLED(); }
        } else if (!s_b1Consumed && held >= BTN_DEBOUNCE_MS) {
            g_testMode = (g_testMode + 1) % 5;   // short press cycles test pattern
            Serial.printf("Test mode: %s\n", testModeName());
            updateOLED();
        }
        s_b1Consumed = false;
    }
    s_b1Prev = b1;

    // ── BTN2 ──────────────────────────────────────────────────────────────────
    if (b2 && !s_b2Prev && (now - s_b2Last) > BTN_DEBOUNCE_MS) {
        s_b2Last = now;
        if (netMenu.active()) {
            netMenu.next();                      // advance field / commit
            updateOLED();
        } else {
            g_testMode = 0;                      // stop test pattern
            memset(rawBuf, 0, sizeof(rawBuf));
            parlio.show(rawBuf, cfg);
            Serial.println("Test mode: OFF");
            updateOLED();
        }
    }
    s_b2Prev = b2;
}

// ── Test pattern ──────────────────────────────────────────────────────────────

static uint8_t s_hue = 0;

// Simple full-saturation HSV → RGB
static void hsv2rgb(uint8_t h, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
    uint8_t region = h / 43;
    uint8_t rem    = (uint8_t)((h % 43) * 6);
    uint8_t q = (uint8_t)((uint16_t)v * (255 - rem) >> 8);
    uint8_t t = (uint8_t)((uint16_t)v * rem >> 8);
    switch (region) {
        case 0: r=v; g=t; b=0; break;
        case 1: r=q; g=v; b=0; break;
        case 2: r=0; g=v; b=t; break;
        case 3: r=0; g=q; b=v; break;
        case 4: r=t; g=0; b=v; break;
        default:r=v; g=0; b=q; break;
    }
}

void applyTestPattern() {
    for (int port = 0; port < NUM_PORTS; port++) {
        int pixN = cfg.ports[port].pixelCount;
        for (int px = 0; px < pixN; px++) {
            uint8_t r = 0, g = 0, b = 0;
            switch (g_testMode) {
                case 1: r = 255; break;
                case 2: g = 255; break;
                case 3: b = 255; break;
                case 4: hsv2rgb(s_hue + (uint8_t)(px * 8), 200, r, g, b); break;
            }
            rawBuf[port][px * 3 + 0] = r;
            rawBuf[port][px * 3 + 1] = g;
            rawBuf[port][px * 3 + 2] = b;
        }
    }
    if (g_testMode == 4) s_hue++;
}

// ── Power-up LED test ─────────────────────────────────────────────────────────

void powerUpTest() {
    const uint8_t colors[3][3] = {{255,0,0},{0,255,0},{0,0,255}};
    for (auto &c : colors) {
        for (int port = 0; port < NUM_PORTS; port++) {
            for (int px = 0; px < cfg.ports[port].pixelCount; px++) {
                rawBuf[port][px*3+0] = c[0];
                rawBuf[port][px*3+1] = c[1];
                rawBuf[port][px*3+2] = c[2];
            }
        }
        parlio.show(rawBuf, cfg);
        delay(500);
    }
    memset(rawBuf, 0, sizeof(rawBuf));
    parlio.show(rawBuf, cfg);
}

// ── Web server ────────────────────────────────────────────────────────────────

void setupWebServer() {
    web.on("/api/status", HTTP_GET, get_status);
    web.on("/api/config", HTTP_GET, api_config_get);

    web.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest *req) {},
        nullptr,
        api_config_post);

    web.on("/api/reboot", HTTP_POST, api_reboot);

    web.on("/api/ota/firmware",   HTTP_POST, api_ota_complete, api_ota_firmware_upload);
    web.on("/api/ota/filesystem", HTTP_POST, api_ota_complete, api_ota_filesystem_upload);

    web.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    web.onNotFound([](AsyncWebServerRequest *req) {
        req->send(404, "text/plain", "Not found");
    });

    web.begin();
    Serial.println("Web server started on port 80");
}

// ── setup / loop ──────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("\nSBPixel16 starting...");

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed — formatting");
        LittleFS.format();
        LittleFS.begin();
    }

    loadConfig();
    setupOLED();
    setupADC();
    setupButtons();
    setupParlio();
    setupDmx();
    fseq.begin();          // power + mount microSD, scan /sequences
    powerUpTest();
    setupEthernet();
    setupWebServer();

    Serial.println("Setup complete");
}

static uint32_t s_refreshTimer = 0;
static uint32_t s_frameCount   = 0;
static uint32_t s_fpsTimer     = 0;

void loop() {
    uint32_t now = millis();

    checkButtons();
    netMenu.tick();
    readADC();

    if (g_testMode == 0) {
        if (cfg.protocol == PROTO_FSEQ) {
            fseq.tick(rawBuf, cfg, dmxBuf);
        } else if (s_ethConnected) {
            if (cfg.protocol == PROTO_E131) parseE131();
            else                            parseDDP();
        }
    }

    if (now - s_refreshTimer >= REFRESH) {
        s_refreshTimer = now;

        if (g_testMode > 0) {
            applyTestPattern();
        } else if (g_lastPacket && (now - g_lastPacket) > DATA_TIMEOUT) {
            memset(rawBuf, 0, sizeof(rawBuf));
            g_lastPacket = 0;
        }

        parlio.show(rawBuf, cfg);
        if (cfg.dmxEnabled) dmx.send(dmxBuf, cfg.dmxChannels);
        s_frameCount++;
    }

    if (now - s_fpsTimer >= 1000) {
        g_fps        = s_frameCount;
        s_frameCount = 0;
        s_fpsTimer   = now;
        updateOLED();
    }
}
