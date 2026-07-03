#pragma once

// Two-button network editor shown on the OLED.
// Entered by long-pressing BTN1. While active:
//   BTN1 = change the value under the cursor (hold to auto-repeat)
//   BTN2 = advance to the next field / commit
// Lets the user set DHCP on/off and, when static, the IP and gateway.
// Subnet is left at its existing config value. Saving reboots the controller.

#include <Adafruit_SSD1306.h>
#include "SBPixel16.h"

// Provided by the .ino
void saveConfig();

class NetMenu {
public:
    bool active() const { return _active; }

    void enter() {
        _active  = true;
        _lastIn  = millis();
        _stage   = S_DHCP;
        _field   = 0;
        _saveYes = true;
        _dhcp    = cfg.dhcp;
        parseIp(cfg.ip,      _ip);
        parseIp(cfg.gateway, _gw);
    }

    // Call every loop — closes the menu after inactivity.
    void tick() {
        if (_active && (millis() - _lastIn) > MENU_TIMEOUT_MS) _active = false;
    }

    // BTN1 — change the value under the cursor.
    void change() {
        _lastIn = millis();
        switch (_stage) {
            case S_DHCP: _dhcp = !_dhcp;        break;
            case S_IP:   _ip[_field]++;         break;  // uint8_t wraps 0..255
            case S_GW:   _gw[_field]++;         break;
            case S_SAVE: _saveYes = !_saveYes;  break;
        }
    }

    // BTN2 — advance to the next field / commit.
    void next() {
        _lastIn = millis();
        switch (_stage) {
            case S_DHCP:
                if (_dhcp) _stage = S_SAVE;              // static fields not needed
                else     { _stage = S_IP; _field = 0; }
                break;
            case S_IP:
                if (++_field > 3) { _stage = S_GW; _field = 0; }
                break;
            case S_GW:
                if (++_field > 3) _stage = S_SAVE;
                break;
            case S_SAVE:
                if (_saveYes) commit();                  // saves + reboots
                else          _active = false;           // cancel
                break;
        }
    }

    void render(Adafruit_SSD1306 &d) {
        d.clearDisplay();
        d.setTextSize(1);
        d.setTextColor(SSD1306_WHITE);

        d.setCursor(0, 0);
        d.print("NETWORK SETUP");

        d.setCursor(0, 12);
        d.print(cur(S_DHCP)); d.print("DHCP:"); d.print(_dhcp ? "ON" : "OFF");

        d.setCursor(0, 24);
        d.print(cur(S_IP)); d.print("IP "); printIp(d, _ip, _stage == S_IP ? _field : -1);

        d.setCursor(0, 36);
        d.print(cur(S_GW)); d.print("GW "); printIp(d, _gw, _stage == S_GW ? _field : -1);

        d.setCursor(0, 52);
        if (_stage == S_SAVE) {
            d.print(">SAVE & REBOOT: ");
            d.print(_saveYes ? "YES" : "NO");
        } else {
            d.print("B1:edit  B2:next");
        }
        d.display();
    }

private:
    enum Stage { S_DHCP, S_IP, S_GW, S_SAVE };
    static const uint32_t MENU_TIMEOUT_MS = 30000;

    bool     _active  = false;
    Stage    _stage   = S_DHCP;
    int      _field   = 0;
    bool     _dhcp    = false;
    bool     _saveYes = true;
    uint8_t  _ip[4]   = {0, 0, 0, 0};
    uint8_t  _gw[4]   = {0, 0, 0, 0};
    uint32_t _lastIn  = 0;

    const char* cur(Stage s) const { return _stage == s ? ">" : " "; }

    void commit() {
        cfg.dhcp = _dhcp;
        if (!_dhcp) {
            cfg.ip      = fmtIp(_ip);
            cfg.gateway = fmtIp(_gw);
            if (cfg.subnet.length() == 0) cfg.subnet = "255.255.255.0";
        }
        saveConfig();
        _active = false;
        delay(150);
        ESP.restart();
    }

    static void parseIp(const String &s, uint8_t out[4]) {
        out[0] = out[1] = out[2] = out[3] = 0;
        int part = 0; uint16_t v = 0;
        for (size_t i = 0; i <= s.length() && part < 4; i++) {
            char c = (i < s.length()) ? s[i] : '.';
            if (c == '.')                 { out[part++] = (uint8_t)v; v = 0; }
            else if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); if (v > 255) v = 255; }
        }
    }

    static String fmtIp(const uint8_t ip[4]) {
        char b[16];
        snprintf(b, sizeof(b), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        return String(b);
    }

    // Render an IP; the octet at index `hi` is bracketed to mark the cursor.
    static void printIp(Adafruit_SSD1306 &d, const uint8_t ip[4], int hi) {
        for (int i = 0; i < 4; i++) {
            if (i == hi) { d.print('['); d.print(ip[i]); d.print(']'); }
            else         { d.print(ip[i]); }
            if (i < 3) d.print('.');
        }
    }
};
