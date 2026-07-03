#pragma once

// PARLIO-based parallel WS2812 driver for ESP32-P4.
// Drives all 16 strips simultaneously via DMA — no RMT required.
//
// Encoding: 3 PARLIO ticks per WS2812 bit @ 2.4 MHz (416.7 ns/tick)
//   Bit-0 wire:  HIGH(417ns)  LOW(833ns)   ← T0H/T0L ✓
//   Bit-1 wire:  HIGH(833ns)  LOW(417ns)   ← T1H/T1L ✓
//   Reset:  200 idle ticks = 83 µs > 50 µs ✓
//
// Each buffer word (uint16_t) = state of 16 GPIO pins for one tick.
// Bit 0 → port 0 (PIN_P0), bit 1 → port 1 (PIN_P1), …, bit 15 → port 15.

#include "driver/parlio_tx.h"
#include "esp_heap_caps.h"
#include "SBPixel16.h"

#define PWS_CLK_HZ      2400000u
#define PWS_TICKS_BIT   3
#define PWS_RESET_TICKS 200

// GPIO → PARLIO data pin mapping (index = port number = PARLIO data bit)
static const gpio_num_t kWsPins[NUM_PORTS] = {
    (gpio_num_t)PIN_P0,  (gpio_num_t)PIN_P1,  (gpio_num_t)PIN_P2,  (gpio_num_t)PIN_P3,
    (gpio_num_t)PIN_P4,  (gpio_num_t)PIN_P5,  (gpio_num_t)PIN_P6,  (gpio_num_t)PIN_P7,
    (gpio_num_t)PIN_P8,  (gpio_num_t)PIN_P9,  (gpio_num_t)PIN_P10, (gpio_num_t)PIN_P11,
    (gpio_num_t)PIN_P12, (gpio_num_t)PIN_P13, (gpio_num_t)PIN_P14, (gpio_num_t)PIN_P15,
};

class ParlioWs2812 {
public:
    // Call once after loadConfig(). Returns false on error.
    bool begin(const AppConfig &cfg) {
        _maxPhys = 0;
        for (int i = 0; i < NUM_PORTS; i++) {
            int g = max(1, (int)cfg.ports[i].grouping);
            int phys = cfg.ports[i].nullPixels + cfg.ports[i].pixelCount * g;
            if (phys > _maxPhys) _maxPhys = phys;
        }

        _bufWords = (size_t)_maxPhys * 24 * PWS_TICKS_BIT + PWS_RESET_TICKS;
        size_t bytes = _bufWords * sizeof(uint16_t);

        _buf = (uint16_t *)heap_caps_calloc(_bufWords, sizeof(uint16_t),
                                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!_buf) {
            // Fallback to PSRAM if internal DMA-capable memory is insufficient
            _buf = (uint16_t *)heap_caps_calloc(_bufWords, sizeof(uint16_t), MALLOC_CAP_DMA);
        }
        if (!_buf) {
            Serial.println("PARLIO: DMA buffer alloc failed");
            return false;
        }

        parlio_tx_unit_config_t ucfg = {};
        ucfg.clk_src             = PARLIO_CLK_SRC_DEFAULT;
        ucfg.clk_in_gpio_num     = (gpio_num_t)-1;
        ucfg.output_clk_freq_hz  = PWS_CLK_HZ;
        ucfg.data_width          = NUM_PORTS;
        for (int i = 0; i < NUM_PORTS; i++)
            ucfg.data_gpio_nums[i] = kWsPins[i];
        ucfg.clk_out_gpio_num    = (gpio_num_t)-1;
        ucfg.valid_gpio_num      = (gpio_num_t)-1;
        ucfg.trans_queue_depth   = 4;
        ucfg.max_transfer_size   = bytes;
        ucfg.dma_burst_size      = 16;
        ucfg.sample_edge         = PARLIO_SAMPLE_EDGE_POS;
        ucfg.bit_pack_order      = PARLIO_BIT_PACK_ORDER_MSB;

        esp_err_t err = parlio_new_tx_unit(&ucfg, &_tx);
        if (err != ESP_OK) {
            Serial.printf("PARLIO: new_tx_unit failed: %s\n", esp_err_to_name(err));
            return false;
        }
        err = parlio_tx_unit_enable(_tx);
        if (err != ESP_OK) {
            Serial.printf("PARLIO: enable failed: %s\n", esp_err_to_name(err));
            return false;
        }
        Serial.printf("PARLIO: ready — maxPhys=%d buf=%u B\n", _maxPhys, (unsigned)bytes);
        return true;
    }

    // Encode rawBuf (per-port logical RGB, row=port, col=pixel*3) and transmit.
    void show(const uint8_t rawBuf[][MAX_PIXELS * 3], const AppConfig &cfg) {
        if (!_tx) return;
        waitDone();
        encode(rawBuf, cfg);
        transmit();
    }

    // All outputs low + reset pulse.
    void clear() {
        if (!_tx) return;
        waitDone();
        memset(_buf, 0, _bufWords * sizeof(uint16_t));
        transmit();
    }

    void waitDone() {
        if (_tx) parlio_tx_unit_wait_all_done(_tx, 500);
    }

private:
    parlio_tx_unit_handle_t _tx      = nullptr;
    uint16_t               *_buf     = nullptr;
    size_t                  _bufWords = 0;
    int                     _maxPhys  = 0;

    // Returns the wire byte (0=first/G, 1=second/R, 2=third/B for WS2812)
    // for physical pixel physPx on the given port, applying color order.
    // Returns 0 (black) for null pixels and pixels beyond the port's count.
    static uint8_t wireByte(int physPx, int wireIdx, int port,
                             const uint8_t rawBuf[][MAX_PIXELS * 3],
                             const AppConfig &cfg)
    {
        const int nullN  = cfg.ports[port].nullPixels;
        const int pixN   = cfg.ports[port].pixelCount;
        const int groupN = max(1, (int)cfg.ports[port].grouping);

        if (physPx < nullN || physPx >= nullN + pixN * groupN)
            return 0;

        const int logi = (physPx - nullN) / groupN;
        const uint8_t r = rawBuf[port][logi * 3 + 0];
        const uint8_t g = rawBuf[port][logi * 3 + 1];
        const uint8_t b = rawBuf[port][logi * 3 + 2];

        // Map logical RGB → physical wire byte order for this LED type.
        // WS2812 wire order is G, R, B (MSB first within each byte).
        uint8_t w[3];
        switch (cfg.ports[port].colorOrder) {
            case CO_RGB: w[0]=r; w[1]=g; w[2]=b; break;
            case CO_BGR: w[0]=b; w[1]=g; w[2]=r; break;
            case CO_RBG: w[0]=r; w[1]=b; w[2]=g; break;
            case CO_GBR: w[0]=g; w[1]=b; w[2]=r; break;
            case CO_BRG: w[0]=b; w[1]=r; w[2]=g; break;
            default:     w[0]=g; w[1]=r; w[2]=b; break; // CO_GRB (WS2812 default)
        }
        return w[wireIdx];
    }

    void encode(const uint8_t rawBuf[][MAX_PIXELS * 3], const AppConfig &cfg) {
        for (int p = 0; p < _maxPhys; p++) {
            for (int b = 0; b < 24; b++) {
                const int wireByteIdx = b / 8;          // 0=G, 1=R, 2=B
                const int wireBitPos  = 7 - (b & 7);   // MSB first

                uint16_t mask = 0;
                for (int port = 0; port < NUM_PORTS; port++) {
                    if ((wireByte(p, wireByteIdx, port, rawBuf, cfg) >> wireBitPos) & 1)
                        mask |= (uint16_t)(1u << port);
                }

                const size_t idx = ((size_t)p * 24 + b) * PWS_TICKS_BIT;
                _buf[idx + 0] = 0xFFFF; // sub-bit 0: all HIGH
                _buf[idx + 1] = mask;   // sub-bit 1: "1" ports stay HIGH
                _buf[idx + 2] = 0x0000; // sub-bit 2: all LOW
            }
        }
        // Reset words at tail are already 0 (calloc / previous clear)
        memset(_buf + (size_t)_maxPhys * 24 * PWS_TICKS_BIT, 0,
               PWS_RESET_TICKS * sizeof(uint16_t));
    }

    void transmit() {
        parlio_transmit_config_t tcfg = {};
        tcfg.idle_value = 0;
        parlio_tx_unit_transmit(_tx, _buf, _bufWords * 16, &tcfg);
    }
};
