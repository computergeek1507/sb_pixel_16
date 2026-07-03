#pragma once

// ── Board / port constants ────────────────────────────────────────────────────

#define NUM_PORTS       16
#define MAX_PIXELS      170     // max pixels per port; increase if needed (affects FPS)
#define DATA_TIMEOUT    1000    // ms before clearing pixels on no data
#define REFRESH         25      // ms between FastLED.show() calls (~40fps target)
#define HTTP_PORT       80
#define E131_UDP_PORT   5568
#define DDP_UDP_PORT    4048
#define CONFIG_FILE     "/config.json"

// ── GPIO output pins for the 16 pixel ports ───────────────────────────────────
// Pin assignments taken directly from the board schematic DATA1–DATA16 labels.
// NOTE: DATA14=GPIO24 (USB DM) and DATA16=GPIO25 (USB DP) — using these for
// pixel output disables USB on-the-fly; acceptable since USB is not needed here.
#define PIN_P0   19   // DATA1
#define PIN_P1   18   // DATA2
#define PIN_P2   17   // DATA3
#define PIN_P3   16   // DATA4
#define PIN_P4   15   // DATA5
#define PIN_P5   14   // DATA6
#define PIN_P6    6   // DATA7
#define PIN_P7    5   // DATA8
#define PIN_P8    4   // DATA9
#define PIN_P9    3   // DATA10
#define PIN_P10   2   // DATA11
#define PIN_P11  46   // DATA12
#define PIN_P12  47   // DATA13
#define PIN_P13  24   // DATA14  (USB DM — USB disabled when in use)
#define PIN_P14  48   // DATA15
#define PIN_P15  25   // DATA16  (USB DP — USB disabled when in use)

// ── Ethernet PHY (IP101GRI on Waveshare ESP32-P4-ETH) ─────────────────────────
#define ETH_PHY_TYPE    ETH_PHY_IP101
#define ETH_PHY_ADDR    1
#define ETH_PHY_MDC     31
#define ETH_PHY_MDIO    52
#define ETH_PHY_POWER   51      // PHY reset / power-enable
#define ETH_CLK_MODE    EMAC_CLK_EXT_IN  // 50 MHz ext crystal → GPIO50

// ── OLED (SSD1306 128×64, I2C) ───────────────────────────────────────────────
// Uses the Waveshare board's default I2C bus.
#define OLED_SDA    7
#define OLED_SCL    8
#define OLED_ADDR   0x3C
#define OLED_W      128
#define OLED_H      64

// ── ADC inputs ────────────────────────────────────────────────────────────────
#define VIN1_PIN    20   // VIN1_ADC
#define VIN2_PIN    21   // VIN2_ADC
#define ADC_SAMPLES  8   // oversampling count for noise reduction

// ── Buttons ───────────────────────────────────────────────────────────────────
#define BTN1_PIN    26   // test-cycle button (active LOW, internal pull-up)
#define BTN2_PIN    27   // test-stop  button (active LOW, internal pull-up)
#define BTN_DEBOUNCE_MS  50

// ── Pixel output limits ───────────────────────────────────────────────────────
#define MAX_NULL_PIXELS  50    // max null/sacrificial pixels per port
#define MAX_GROUPING      8    // max physical LEDs per logical channel group

// ── Color order (wire order of the physical LED) ──────────────────────────────
// FastLED NEOPIXEL sends bytes as G,R,B. These constants drive the permutation
// that makes each physical LED type receive the correct byte sequence.
#define CO_GRB  0   // WS2812 / WS2811 default
#define CO_RGB  1
#define CO_BGR  2
#define CO_RBG  3
#define CO_GBR  4
#define CO_BRG  5

// ── Input protocol ────────────────────────────────────────────────────────────
#define PROTO_E131  0
#define PROTO_DDP   1

// ── Config structs ────────────────────────────────────────────────────────────

typedef struct {
    uint16_t pixelCount;    // logical pixel count (unique channel groups)
    uint32_t startChannel;  // absolute 1-based channel index
    uint8_t  colorOrder;    // CO_GRB … CO_BRG (default CO_GRB = 0)
    uint8_t  nullPixels;    // sacrificial pixels sent before data (0–MAX_NULL_PIXELS)
    uint8_t  grouping;      // physical LEDs per logical pixel (1 = no grouping)
} PortConfig;

typedef struct {
    bool     dhcp      = false;
    String   ip        = "192.168.1.200";
    String   subnet    = "255.255.255.0";
    String   gateway   = "192.168.1.1";
    uint8_t  protocol  = PROTO_E131;
    uint16_t uniStart  = 1;
    uint16_t uniSize   = 510;
    PortConfig ports[NUM_PORTS];
    // DMX output
    bool     dmxEnabled  = false;
    uint32_t dmxStartCh  = 1;
    uint16_t dmxChannels = 512;

#define DMX_TX_PIN 54
} AppConfig;

// ── Runtime stats (updated in loop) ──────────────────────────────────────────
extern uint32_t g_e131Packets;
extern uint32_t g_ddpPackets;
extern uint32_t g_fps;
extern uint32_t g_lastPacket;

// ADC readings (millivolts, updated every 500 ms)
extern uint32_t g_vin1_mv;
extern uint32_t g_vin2_mv;

// Test mode: 0=off 1=red 2=green 3=blue 4=rainbow
extern uint8_t  g_testMode;

// ── Global config ─────────────────────────────────────────────────────────────
extern AppConfig cfg;
