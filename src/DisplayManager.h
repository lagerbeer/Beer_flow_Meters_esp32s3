#pragma once

// ============================================================
// 6x ILI9488 3.5" 480×320 TFT displays — one per tap
// All share MOSI/SCLK/DC/RST, each has its own CS pin
// NOTE: boards are labelled "ILI9341" but the actual chip is
//       ILI9488 — the only controller that supports 480×320 SPI
// ============================================================
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#define DISPLAY_W  480   // logical width  (landscape)
#define DISPLAY_H  320   // logical height (landscape)

// Shared SPI pins
#define TFT_MOSI    11
#define TFT_SCLK    12
#define TFT_DC       8
#define TFT_RST      9
#define TFT_BL      45

// CS pin per tap (index 0-5)
static const int TFT_CS_PINS[6] = { 10, 13, 14, 18, 21, 38 };

// ── LovyanGFX ILI9488 device ─────────────────────────────────
class LGFX_TFT : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9486 _panel;  // try ILI9488 if this doesn't work
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _light;
public:
    LGFX_TFT() {}

    bool init(int csPin, int spiHost, int pwmChannel) {
        {
            auto cfg        = _bus.config();
            cfg.spi_host    = (spi_host_device_t)spiHost;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 16000000;  // conservative — ILI9486/9488 safe max
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = TFT_SCLK;
            cfg.pin_mosi    = TFT_MOSI;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = TFT_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg             = _panel.config();
            cfg.pin_cs           = csPin;
            cfg.pin_rst          = -1;   // RST handled manually — shared pin, single reset at boot
            cfg.pin_busy         = -1;
            cfg.panel_width      = 320;  // ILI9488 physical columns (portrait)
            cfg.panel_height     = 480;  // ILI9488 physical rows    (portrait)
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = false; // ILI9488 does not need inversion
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = true;
            _panel.config(cfg);
        }
        // Only wire backlight PWM on the first display (channel 0).
        // All screens share the same BL pin so one PWM controls all.
        if (pwmChannel == 0) {
            auto cfg        = _light.config();
            cfg.pin_bl      = TFT_BL;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 0;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
        lgfx::LGFX_Device::init();
        setRotation(3);   // landscape 480×320 rotated 180°
        fillScreen(TFT_BLACK);
        setBrightness(255);
        return true;
    }

    int dispW() const { return DISPLAY_W; }
    int dispH() const { return DISPLAY_H; }
};

// ============================================================

class DisplayManager {
public:
    static DisplayManager& getInstance();

    bool begin();
    void update();

    void onPourStart(int tapIndex);
    void onPourEnd(int tapIndex, float ounces, float duration);
    void showLowKegAlert(int tapIndex, float level);

    bool isAvailable() const { return m_available; }

private:
    DisplayManager() = default;

    void drawTapIdle(int tap);
    void drawTapPouring(int tap);
    void drawTapPourComplete(int tap);
    void drawSplash(int tap);

    void     drawRadialGauge(LGFX_TFT& d, int cx, int cy, int rOuter, int rInner, float pct, uint16_t fillColor, uint16_t trackColor);
    void     drawGlassPanel(LGFX_TFT& d, int x, int y, int w, int h, int radius);
    uint16_t levelColor(LGFX_TFT& d, float pct);
    uint16_t amberColor(LGFX_TFT& d);
    uint16_t hexToColor(LGFX_TFT& d, const String& hex);

    int dispW(int tap) const { return DISPLAY_W; }
    int dispH(int tap) const { return DISPLAY_H; }

    bool    m_available  = false;
    uint8_t m_brightness = 255;
    uint8_t m_animFrame  = 0;

    struct TapDisplay {
        bool          pouring       = false;
        bool          showComplete  = false;
        unsigned long completeUntil = 0;
        unsigned long lastDraw      = 0;
        float         lastOz        = 0;
        float         lastDuration  = 0;
    };
    TapDisplay m_tap[6];

    LGFX_TFT m_display[6];
    bool      m_displayOk[6] = {false};
};
