#pragma once

// FSEQ v2 player for microSD (uncompressed, sparse or full frames).
// Plays every *.fseq in FSEQ_DIR alphabetically, then loops the whole set
// forever. Frames are scattered into the per-port rawBuf using the same
// absolute-channel model as the E1.31 / DDP parsers, so per-port start
// channel, color order, brightness and grouping all apply unchanged.
//
// Only compression type 0 (uncompressed) is supported; compressed files are
// skipped. Reference: FPP/xLights FSEQ v2 file format.

#include <SD_MMC.h>
#include "esp_heap_caps.h"
#include "SBPixel16.h"

#define FSEQ_MAX_FILES   64
#define FSEQ_MAX_RANGES  32
#define FSEQ_MAX_FRAME   262144u   // cap on per-frame bytes we will buffer

class FseqPlayer {
public:
    // Mount the card (variant auto-config: SLOT-0 IOMUX, on-chip LDO ch4, power
    // pin GPIO45) and scan the sequence folder. Safe to call repeatedly — runs
    // once. Call at boot AFTER setupParlio() so PARLIO gets its DMA first; early
    // mount is what works on this board.
    bool begin() {
        if (_begun) return _mounted;
        _begun = true;
        // The esp32p4 Arduino variant already defines the SD slot (SLOT-0 IOMUX),
        // the on-chip LDO power channel (4) and the card power pin (GPIO45, active
        // LOW). Let SD_MMC.begin() apply all of that — do NOT call setPins()
        // (SLOT-0 uses fixed IOMUX pins and setPins would fight it).
        const char *how = "fail";
        bool ok = SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT);  // 4-bit 20MHz
        if (ok) how = "4bit";
        if (!ok) { SD_MMC.end(); delay(10);
                   ok = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT);   // 1-bit
                   if (ok) how = "1bit"; }

        strncpy(g_sdError, how, sizeof(g_sdError) - 1);
        g_sdError[sizeof(g_sdError) - 1] = 0;

        if (!ok) {
            Serial.println("SD: mount failed");
            _mounted = false; g_sdMounted = false;
            return false;
        }
        _mounted    = true;
        g_sdMounted = true;
        g_sdSizeMB  = (uint32_t)(SD_MMC.cardSize() / (1024ULL * 1024ULL));
        Serial.printf("SD: mounted (%s), %lu MB\n", how, (unsigned long)g_sdSizeMB);

        scanPlaylist();
        return true;
    }

    // Called from loop() when in FSEQ mode. Advances/scatters frames on the
    // sequence's own step-time cadence; between frames it does nothing.
    void tick(uint8_t rawBuf[][MAX_PIXELS * 3], const AppConfig &cfg, uint8_t *dmxBuf) {
        if (!_mounted || _fileCount == 0) return;

        if (!_file) { if (!openNextPlayable()) return; }   // open first file

        uint32_t now = millis();
        if (_started && (now - _lastFrameMs) < _stepMs) return;
        _started      = true;
        _lastFrameMs  = now;

        applyFrame(rawBuf, cfg, dmxBuf);

        if (++_curFrame >= _frameCount) openNextPlayable();  // wraps at end of list
    }

private:
    bool     _begun   = false;
    bool     _mounted = false;
    int      _fileCount = 0;
    String   _files[FSEQ_MAX_FILES];
    int      _curFile = -1;

    File     _file;
    uint32_t _dataStart  = 0;
    uint32_t _frameCount = 0;
    uint32_t _frameSize  = 0;   // bytes per frame on disk
    uint8_t  _stepMs     = 25;
    uint32_t _curFrame   = 0;
    uint32_t _lastFrameMs = 0;
    bool     _started    = false;

    struct Range { uint32_t start; uint32_t len; };
    Range    _ranges[FSEQ_MAX_RANGES];
    int      _rangeCount = 0;

    uint8_t *_frameBuf = nullptr;
    size_t   _frameCap = 0;

    // ── helpers ───────────────────────────────────────────────────────────────
    static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
    static uint32_t rd24(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16); }
    static uint32_t rd32(const uint8_t *p) {
        return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    void scanPlaylist() {
        _fileCount = 0;
        File dir = SD_MMC.open(FSEQ_DIR);
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); dir = SD_MMC.open("/"); }
        if (!dir) { g_fseqCount = 0; return; }

        for (File f = dir.openNextFile(); f && _fileCount < FSEQ_MAX_FILES; f = dir.openNextFile()) {
            if (!f.isDirectory()) {
                String lower = String(f.name()); lower.toLowerCase();
                if (lower.endsWith(".fseq")) {
                    const char *p = f.path();               // full path incl. folder
                    _files[_fileCount++] = (p && *p) ? String(p) : String(f.name());
                }
            }
            f.close();
        }
        dir.close();

        // Alphabetical order (simple insertion sort — list is small).
        for (int i = 1; i < _fileCount; i++) {
            String key = _files[i]; int j = i - 1;
            while (j >= 0 && _files[j] > key) { _files[j + 1] = _files[j]; j--; }
            _files[j + 1] = key;
        }
        g_fseqCount = (uint16_t)_fileCount;
        Serial.printf("FSEQ: %d sequence(s) found\n", _fileCount);
    }

    // Advance _curFile and open the next uncompressed file. Wraps the list;
    // returns false only if no file in the whole list is playable.
    bool openNextPlayable() {
        if (_file) _file.close();
        for (int tries = 0; tries < _fileCount; tries++) {
            _curFile = (_curFile + 1) % _fileCount;
            if (openFile(_curFile)) return true;
        }
        return false;
    }

    bool openFile(int idx) {
        if (_file) _file.close();
        _file = SD_MMC.open(_files[idx], FILE_READ);
        if (!_file) { Serial.printf("FSEQ: open failed: %s\n", _files[idx].c_str()); return false; }

        uint8_t h[32];
        if (_file.read(h, 32) != 32) { _file.close(); return false; }

        if (h[0] != 'P' || h[1] != 'S' || h[2] != 'E' || h[3] != 'Q' || h[7] != 2) {
            Serial.printf("FSEQ: %s not a v2 file\n", _files[idx].c_str());
            _file.close(); return false;
        }

        _dataStart          = rd16(h + 4);
        uint32_t channels   = rd32(h + 10);
        _frameCount         = rd32(h + 14);
        _stepMs             = h[18] ? h[18] : 25;
        uint8_t  comp       = h[20] & 0x0F;
        uint16_t compBlocks = (uint16_t)(((h[20] & 0xF0) << 4) | h[21]);
        uint8_t  numRanges  = h[22];

        if (comp != 0) {
            Serial.printf("FSEQ: %s is compressed — skipping\n", _files[idx].c_str());
            _file.close(); return false;
        }
        if (_frameCount == 0 || channels == 0) {
            Serial.printf("FSEQ: %s empty — skipping\n", _files[idx].c_str());
            _file.close(); return false;
        }
        if (numRanges > FSEQ_MAX_RANGES) {
            Serial.printf("FSEQ: %s has too many sparse ranges (%u)\n", _files[idx].c_str(), numRanges);
            _file.close(); return false;
        }

        // Sparse range table follows the compression block index.
        _rangeCount = 0;
        _frameSize  = 0;
        if (numRanges == 0) {
            _ranges[0] = { 0, channels };           // full frame = one implicit range
            _rangeCount = 1;
            _frameSize  = channels;
        } else {
            uint32_t rangeOff = 32u + (uint32_t)compBlocks * 8u;
            _file.seek(rangeOff);
            uint8_t rb[6];
            for (int i = 0; i < numRanges; i++) {
                if (_file.read(rb, 6) != 6) { _file.close(); return false; }
                uint32_t start = rd24(rb);
                uint32_t len   = rd24(rb + 3);
                _ranges[_rangeCount++] = { start, len };
                _frameSize += len;
            }
        }

        if (_frameSize == 0 || _frameSize > FSEQ_MAX_FRAME) {
            Serial.printf("FSEQ: %s frame size %lu unsupported\n",
                          _files[idx].c_str(), (unsigned long)_frameSize);
            _file.close(); return false;
        }
        if (!ensureBuf(_frameSize)) { _file.close(); return false; }

        _curFrame    = 0;
        _started     = false;
        strncpy(g_fseqName, _files[idx].c_str(), sizeof(g_fseqName) - 1);
        g_fseqName[sizeof(g_fseqName) - 1] = 0;
        g_fseqFrames = _frameCount;
        g_fseqFrame  = 0;
        Serial.printf("FSEQ: playing %s (%lu frames @ %ums, %d range(s))\n",
                      g_fseqName, (unsigned long)_frameCount, _stepMs, _rangeCount);
        return true;
    }

    bool ensureBuf(size_t need) {
        if (_frameCap >= need && _frameBuf) return true;
        if (_frameBuf) free(_frameBuf);
        _frameBuf = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        if (!_frameBuf) _frameBuf = (uint8_t *)malloc(need);   // fall back to internal
        _frameCap = _frameBuf ? need : 0;
        if (!_frameBuf) Serial.println("FSEQ: frame buffer alloc failed");
        return _frameBuf != nullptr;
    }

    void applyFrame(uint8_t rawBuf[][MAX_PIXELS * 3], const AppConfig &cfg, uint8_t *dmxBuf) {
        uint32_t pos = _dataStart + (uint64_t)_curFrame * _frameSize;
        _file.seek(pos);
        if (_file.read(_frameBuf, _frameSize) != (int)_frameSize) return;

        // Unmapped channels go dark each frame.
        for (int p = 0; p < NUM_PORTS; p++) memset(rawBuf[p], 0, MAX_PIXELS * 3);

        uint32_t off = 0;   // running offset of this range within the frame buffer
        for (int r = 0; r < _rangeCount; r++) {
            const uint32_t absStart = _ranges[r].start;
            const uint32_t len      = _ranges[r].len;
            const uint8_t *data     = _frameBuf + off;
            off += len;

            scatter(rawBuf, cfg, absStart, len, data);
            if (cfg.dmxEnabled) scatterDmx(dmxBuf, cfg, absStart, len, data);
        }
        g_fseqFrame = _curFrame;
    }

    // Copy the overlap of [absStart, absStart+len) into each port's rawBuf.
    static void scatter(uint8_t rawBuf[][MAX_PIXELS * 3], const AppConfig &cfg,
                        uint32_t absStart, uint32_t len, const uint8_t *data) {
        uint32_t rngEnd = absStart + len - 1;
        for (int port = 0; port < NUM_PORTS; port++) {
            uint32_t portAbs0 = cfg.ports[port].startChannel - 1;
            uint32_t portAbs1 = portAbs0 + (uint32_t)cfg.ports[port].pixelCount * 3 - 1;
            uint32_t ov0 = max(portAbs0, absStart);
            uint32_t ov1 = min(portAbs1, rngEnd);
            if (ov0 > ov1) continue;
            memcpy(rawBuf[port] + (ov0 - portAbs0), data + (ov0 - absStart), ov1 - ov0 + 1);
        }
    }

    static void scatterDmx(uint8_t *dmxBuf, const AppConfig &cfg,
                           uint32_t absStart, uint32_t len, const uint8_t *data) {
        uint32_t rngEnd  = absStart + len - 1;
        uint32_t dmxAbs0 = cfg.dmxStartCh - 1;
        uint32_t dmxAbs1 = dmxAbs0 + 511;
        uint32_t ov0 = max(dmxAbs0, absStart);
        uint32_t ov1 = min(dmxAbs1, rngEnd);
        if (ov0 <= ov1) memcpy(dmxBuf + (ov0 - dmxAbs0), data + (ov0 - absStart), ov1 - ov0 + 1);
    }
};
