#include "DisplayManager.h"
#include "TapManager.h"
#include "config.h"
#include <WiFi.h>

// ============================================================
// Singleton
// ============================================================
DisplayManager& DisplayManager::getInstance() {
    static DisplayManager instance;
    return instance;
}

// ============================================================
// Helpers
//
// NOTE: these draw directly to the panel (LGFX_TFT), not to an
// off-screen LGFX_Sprite. This board is an ESP32-S3 N8 with no
// PSRAM, and a full-frame 480x320 sprite needs a single contiguous
// 153,600-byte allocation — ESP.getMaxAllocHeap() on this hardware
// tops out well under that even right after boot, so createSprite()
// for a full screen buffer reliably fails. Drawing straight to the
// panel needs no big buffer at all, at the cost of the (acceptable,
// since these redraw at most every 500ms) flicker of no double
// buffering.
// ============================================================
uint16_t DisplayManager::levelColor(LGFX_TFT& d, float pct) {
    if (pct > 0.7f) return d.color565(0,   200,  0);
    if (pct > 0.3f) return d.color565(220, 220,  0);
    if (pct > 0.1f) return d.color565(255, 140,  0);
    return d.color565(220, 0, 0);
}

uint16_t DisplayManager::amberColor(LGFX_TFT& d) {
    return d.color565(255, 176, 0);
}

// Parse a "#rrggbb" hex string (as stored in TapConfig::beerColor) into RGB565.
// Falls back to amber if the string is missing/malformed.
uint16_t DisplayManager::hexToColor(LGFX_TFT& d, const String& hex) {
    if (hex.length() < 7 || hex[0] != '#') return amberColor(d);
    long val = strtol(hex.c_str() + 1, nullptr, 16);
    uint8_t r = (val >> 16) & 0xFF;
    uint8_t g = (val >> 8)  & 0xFF;
    uint8_t b =  val        & 0xFF;
    return d.color565(r, g, b);
}

// Frosted-glass style card: a soft rounded panel a shade lighter than the
// background, with a subtle top highlight to fake a glassy reflection.
void DisplayManager::drawGlassPanel(LGFX_TFT& d, int x, int y, int w, int h, int radius) {
    d.fillSmoothRoundRect(x, y, w, h, radius, d.color565(30, 32, 38));
    d.drawRoundRect(x, y, w, h, radius, d.color565(60, 64, 74));
    // Top highlight sliver — reads as a glassy sheen
    d.drawFastHLine(x + radius, y + 1, w - radius * 2, d.color565(80, 84, 96));
}

// Modern radial gauge — a 270° ring (gap at the bottom) used for keg level.
// Angle convention: 0deg = 3 o'clock, increasing clockwise (LovyanGFX arc default).
void DisplayManager::drawRadialGauge(LGFX_TFT& d, int cx, int cy, int rOuter, int rInner, float pct, uint16_t fillColor, uint16_t trackColor) {
    pct = constrain(pct, 0.0f, 1.0f);
    static const float START = 135.0f;
    static const float SWEEP = 270.0f;
    d.fillArc(cx, cy, rOuter, rInner, START, START + SWEEP, trackColor);
    if (pct > 0.002f) {
        d.fillArc(cx, cy, rOuter, rInner, START, START + SWEEP * pct, fillColor);
    }
}

// Small label/value stat card, used in the idle & pour screens' side panels.
static void drawStatChip(LGFX_TFT& d, int x, int y, int w, int h, const char* label, const String& value, uint16_t valueColor) {
    d.fillSmoothRoundRect(x, y, w, h, 8, d.color565(44, 47, 56));
    d.setTextColor(d.color565(140, 144, 154));
    d.setTextSize(1);
    d.setCursor(x + 8, y + 6);
    d.print(label);
    d.setTextColor(valueColor);
    d.setTextSize(2);
    d.setCursor(x + 8, y + 19);
    d.print(value);
}

static const char* kegStatusText(float pct) {
    if (pct > 0.7f) return "PLENTY";
    if (pct > 0.3f) return "GOOD";
    if (pct > 0.1f) return "LOW";
    return "CRITICAL";
}

// ============================================================
// Boot — initialise all 6 displays
// ============================================================
bool DisplayManager::begin() {
    LOG_I("Initializing displays (one per tap)...");

    for (int i = 0; i < NUM_TAPS; i++) {
        pinMode(TFT_CS_PINS[i], OUTPUT);
        digitalWrite(TFT_CS_PINS[i], HIGH);
    }

    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, HIGH); delay(50);
    digitalWrite(TFT_RST, LOW);  delay(50);
    digitalWrite(TFT_RST, HIGH); delay(250);
    LOG_I("Hardware reset done");

    static const int DISPLAYS_CONNECTED[] = { TFT_CS_PINS[0], TFT_CS_PINS[1], TFT_CS_PINS[2], TFT_CS_PINS[3], TFT_CS_PINS[4], TFT_CS_PINS[5] };
    static const int NUM_CONNECTED = sizeof(DISPLAYS_CONNECTED) / sizeof(DISPLAYS_CONNECTED[0]);

    int found = 0;
    for (int i = 0; i < NUM_TAPS; i++) {
        bool connected = false;
        for (int j = 0; j < NUM_CONNECTED; j++) {
            if (TFT_CS_PINS[i] == DISPLAYS_CONNECTED[j]) { connected = true; break; }
        }

        if (!connected) {
            LOG_I("  Display %d: CS=%d not in connected list — skipping", i + 1, TFT_CS_PINS[i]);
            m_displayOk[i] = false;
            continue;
        }

        m_display[i].init(TFT_CS_PINS[i], (int)SPI2_HOST, i % 8);
        delay(20);
        m_displayOk[i] = true;
        drawSplash(i);
        LOG_I("  Display %d: CS=%d OK", i + 1, TFT_CS_PINS[i]);
        found++;
    }

    delay(2000);

    for (int i = 0; i < NUM_TAPS; i++) {
        if (m_displayOk[i]) {
            drawTapIdle(i);
            m_tap[i].lastDraw = millis();
        }
    }

    m_available = (found > 0);
    LOG_I("%d/%d displays found and initialised", found, NUM_TAPS);
    return m_available;
}

// ============================================================
// Splash screen
// ============================================================
void DisplayManager::drawSplash(int tap) {
    LGFX_TFT& d = m_display[tap];
    d.startWrite();

    // Gradient background
    for (int i = 0; i < dispH(tap); i++) {
        uint8_t b = map(i, 0, dispH(tap), 20, 0);
        d.drawFastHLine(0, i, dispW(tap), d.color565(0, 0, b));
    }

    // Beer mug icon (centred at x=240 for 480px wide display)
    uint16_t amber = amberColor(d);
    d.fillRect(207, 95, 70, 100, amber);   // mug body
    d.fillRect(277, 115, 22, 60, amber);   // handle
    d.drawRect(277, 115, 22, 60, TFT_WHITE);                     // handle outline
    d.fillRoundRect(200, 82, 84, 22, 6, TFT_WHITE);             // foam top
    d.fillCircle(222, 108, 4, TFT_WHITE);                        // bubble 1
    d.fillCircle(255, 115, 3, TFT_WHITE);                        // bubble 2

    d.setTextColor(TFT_WHITE);
    d.setTextSize(3);
    d.setCursor(159, 35);   d.print("Beer Flow");
    d.setTextSize(2);
    d.setCursor(198, 68);   d.print("Monitor");

    d.setTextColor(amber);
    d.setTextSize(3);
    d.setCursor(195, 220);
    d.printf("TAP %d", tap + 1);

    d.setTextColor(d.color565(150, 150, 150));
    d.setTextSize(1);
    d.setCursor(222, 265);
    d.printf("v%s", FIRMWARE_VERSION);
    d.setCursor(192, 285);
    d.print("Initialising...");

    d.endWrite();
}

// ============================================================
// Idle screen
// ============================================================
void DisplayManager::drawTapIdle(int tap) {
    TapConfig cfg = TapManager::getInstance().getConfig(tap);
    TapState  st  = TapManager::getInstance().getState(tap);
    float pct = (cfg.kegSize > 0) ? constrain(st.currentKegLevel / cfg.kegSize, 0.0f, 1.0f) : 0;

    LGFX_TFT& d = m_display[tap];
    uint16_t beerColor = hexToColor(d, cfg.beerColor);
    uint16_t levelCol  = levelColor(d, pct);

    d.startWrite();

    // ── Background: subtle vertical gradient tinted with the beer's color ──
    uint8_t br = (beerColor >> 11) & 0x1F, bg2 = (beerColor >> 5) & 0x3F, bb = beerColor & 0x1F;
    for (int y = 0; y < dispH(tap); y++) {
        float t = (float)y / dispH(tap);
        uint8_t r = (uint8_t)(br * 8 * 0.12f * t);
        uint8_t g = (uint8_t)(bg2 * 4 * 0.12f * t);
        uint8_t b = (uint8_t)(bb * 8 * 0.12f * t);
        d.drawFastHLine(0, y, dispW(tap), d.color565(10 + r, 10 + g, 12 + b));
    }

    uint16_t midGray = d.color565(150, 154, 162);

    // ── Header glass panel ──────────────────────────────────────────────
    drawGlassPanel(d, 6, 6, 468, 54, 14);
    d.fillSmoothCircle(40, 33, 22, beerColor);
    d.setTextColor(TFT_BLACK);
    d.setTextSize(3);
    d.setCursor(tap < 9 ? 29 : 20, 20);
    d.printf("%d", tap + 1);

    d.setTextColor(TFT_WHITE);
    d.setTextSize(2);
    d.setCursor(76, 11);
    d.print(cfg.tapName.substring(0, 20));

    d.setTextColor(beerColor);
    d.setTextSize(2);
    d.setCursor(76, 34);
    d.print(cfg.beerName.substring(0, 20));

    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    d.fillSmoothCircle(452, 20, 6, wifiOk ? d.color565(0, 200, 90) : d.color565(220, 60, 60));

    // ── Left: radial keg-level gauge ─────────────────────────────────────
    int gcx = 120, gcy = 196, gOuter = 108, gInner = 88;
    drawRadialGauge(d, gcx, gcy, gOuter, gInner, pct, levelCol, d.color565(42, 45, 52));

    d.setTextColor(TFT_WHITE);
    d.setTextSize(4);
    String ozStr = String((int)st.currentKegLevel);
    d.setCursor(gcx - d.textWidth(ozStr) / 2, gcy - 46);
    d.print(ozStr);

    d.setTextColor(midGray);
    d.setTextSize(2);
    String ozLbl = "oz remaining";
    d.setCursor(gcx - d.textWidth(ozLbl) / 2, gcy - 8);
    d.print(ozLbl);

    d.setTextColor(levelCol);
    d.setTextSize(3);
    String pctStr = String((int)(pct * 100)) + "%";
    d.setCursor(gcx - d.textWidth(pctStr) / 2, gcy + 20);
    d.print(pctStr);

    d.setTextColor(d.color565(110, 114, 122));
    d.setTextSize(1);
    String capLbl = "of " + String((int)cfg.kegSize) + " oz keg";
    d.setCursor(gcx - d.textWidth(capLbl) / 2, gcy + 52);
    d.print(capLbl);

    // ── Right: status panel ─────────────────────────────────────────────
    int rx = 242, rw = 232;
    drawGlassPanel(d, rx, 68, rw, 104, 14);
    d.setTextColor(d.color565(140, 144, 154));
    d.setTextSize(1);
    d.setCursor(rx + 14, 82);
    d.print("KEG LEVEL");
    d.setTextColor(levelCol);
    d.setTextSize(4);
    d.setCursor(rx + 14, 98);
    d.print(pctStr);
    d.setTextSize(2);
    d.setCursor(rx + 14, 148);
    d.print(kegStatusText(pct));

    // ── Right: last pour panel ──────────────────────────────────────────
    drawGlassPanel(d, rx, 180, rw, 134, 14);
    d.setTextColor(d.color565(140, 144, 154));
    d.setTextSize(1);
    d.setCursor(rx + 14, 194);
    d.print("LAST POUR");

    if (st.hasLastPour) {
        int chipY = 216, chipW = (rw - 28 - 8) / 2, chipH = 44;
        drawStatChip(d, rx + 14,              chipY,          chipW, chipH, "OUNCES", String(st.lastPour.ounces, 1), TFT_WHITE);
        drawStatChip(d, rx + 14 + chipW + 8,  chipY,          chipW, chipH, "TIME",   String(st.lastPour.duration, 1) + "s", TFT_WHITE);
        drawStatChip(d, rx + 14,              chipY + chipH+8, chipW, chipH, "PEAK",   String(st.lastPour.peakFlowRate, 1) + " oz/s", beerColor);
        drawStatChip(d, rx + 14 + chipW + 8,  chipY + chipH+8, chipW, chipH, "POURS",  String((int)st.history.size()), beerColor);
    } else {
        d.setTextColor(midGray);
        d.setTextSize(2);
        String noneLbl = "No pours yet";
        d.setCursor(rx + rw / 2 - d.textWidth(noneLbl) / 2, 232);
        d.print(noneLbl);
    }

    d.endWrite();
}

// ============================================================
// Live pour screen
// ============================================================
void DisplayManager::drawTapPouring(int tap) {
    TapConfig cfg = TapManager::getInstance().getConfig(tap);
    TapState  st  = TapManager::getInstance().getState(tap);
    unsigned long elapsed = st.pourStartTime > 0 ? (millis() - st.pourStartTime) / 1000 : 0;
    float flowRate = (elapsed > 0) ? (st.currentPourOz / (float)elapsed) : 0;
    float fillPct  = constrain(st.currentPourOz / 20.0f, 0.0f, 1.0f);

    LGFX_TFT& d = m_display[tap];
    uint16_t beerColor = hexToColor(d, cfg.beerColor);
    uint8_t  beerR = ((beerColor >> 11) & 0x1F) << 3;
    uint8_t  beerG = ((beerColor >> 5)  & 0x3F) << 2;
    uint8_t  beerB = ( beerColor        & 0x1F) << 3;

    d.startWrite();

    // Background gradient tinted by the beer's actual color
    for (int y = 0; y < dispH(tap); y++) {
        float t = (float)y / dispH(tap);
        uint8_t r = (uint8_t)(beerR * 0.16f * t);
        uint8_t g = (uint8_t)(beerG * 0.16f * t);
        uint8_t b = (uint8_t)(beerB * 0.16f * t);
        d.drawFastHLine(0, y, dispW(tap), d.color565(12 + r, 12 + g, 14 + b));
    }

    // Pulsing glow border — alternates between the beer's color and hot amber
    uint16_t borderCol = (m_animFrame % 2 == 0) ? beerColor : d.color565(255, 200, 80);
    d.drawRect(0, 0, dispW(tap),     dispH(tap),     borderCol);
    d.drawRect(1, 1, dispW(tap) - 2, dispH(tap) - 2, borderCol);
    d.drawRect(2, 2, dispW(tap) - 4, dispH(tap) - 4, borderCol);

    // ── Header glass panel ──────────────────────────────────────────────
    drawGlassPanel(d, 6, 6, 468, 48, 12);
    d.setTextColor(beerColor);
    d.setTextSize(2);
    d.setCursor(14, 8);
    d.printf("TAP %d", tap + 1);
    d.setTextColor(TFT_WHITE);
    d.setCursor(14, 28);
    d.print(cfg.beerName.substring(0, 24));

    d.setTextColor(d.color565(255, 210, 120));
    d.setTextSize(2);
    String pouringLbl = "POURING";
    for (int k = 0; k < (int)(m_animFrame % 4); k++) pouringLbl += ".";
    d.setCursor(462 - d.textWidth(pouringLbl), 18);
    d.print(pouringLbl);

    // ── Beer glass (left) — filled with the beer's real color ───────────
    int bx = 24, by = 62, bw = 108, gh = 196;
    uint16_t glassLine = d.color565(210, 214, 222);
    d.drawLine(bx,      by + gh, bx + bw,     by + gh, glassLine);
    d.drawLine(bx,      by + gh, bx - 10,     by,      glassLine);
    d.drawLine(bx + bw, by + gh, bx + bw + 10, by,     glassLine);
    d.drawFastHLine(bx - 10, by, bw + 21, glassLine);

    int liquidH = (int)(fillPct * (gh - 10));
    for (int row = 0; row < liquidH; row++) {
        float t  = (float)row / gh;
        int lx   = (int)(bx - 10 * t) + 2;
        int lw   = (int)(bw + 20 * t) - 4;
        int ly   = by + gh - 5 - row;
        float shade = 0.55f + 0.45f * ((float)row / (float)max(liquidH, 1));
        uint8_t rr = (uint8_t)constrain(beerR * shade, 0.0f, 255.0f);
        uint8_t gg = (uint8_t)constrain(beerG * shade, 0.0f, 255.0f);
        uint8_t bb = (uint8_t)constrain(beerB * shade, 0.0f, 255.0f);
        d.drawFastHLine(lx, ly, lw, d.color565(rr, gg, bb));
    }
    if (liquidH > 5) {
        int foamY = by + gh - 5 - liquidH;
        d.fillSmoothRoundRect(bx - 8, foamY - 7, bw + 18, 11, 4, d.color565(245, 245, 245));
    }
    // Rising bubbles — several, phase-offset so they drift independently
    if (liquidH > 16) {
        static const int NB = 5;
        static const int phase[NB] = {0, 3, 6, 9, 12};
        static const int xoff[NB]  = {-24, -8, 4, 16, 26};
        int travel = max(liquidH - 14, 1);
        for (int b = 0; b < NB; b++) {
            int by2  = by + gh - 16 - ((m_animFrame * 6 + phase[b] * 5) % travel);
            int rad  = 2 + (b % 3);
            d.fillSmoothCircle(bx + bw / 2 + xoff[b], by2, rad, d.color565(255, 235, 190));
        }
    }

    // ── Right: live stats ────────────────────────────────────────────────
    int rx = 160;
    d.setTextColor(TFT_WHITE);
    d.setTextSize(5);
    String ozStr = String(st.currentPourOz, 2);
    d.setCursor(rx, 66);
    d.print(ozStr);
    d.setTextColor(d.color565(150, 154, 162));
    d.setTextSize(2);
    d.setCursor(rx + d.textWidth(ozStr) + 10, 96);
    d.print("oz");

    int chipY = 150, chipW = 148, chipH = 46;
    drawStatChip(d, rx,              chipY, chipW, chipH, "FLOW RATE", String(flowRate, 2) + " oz/s", d.color565(80, 220, 220));
    drawStatChip(d, rx + chipW + 10, chipY, chipW, chipH, "ELAPSED",   String(elapsed) + "s",         d.color565(255, 210, 120));

    // ── Bottom: keg remaining ────────────────────────────────────────────
    TapConfig c2 = TapManager::getInstance().getConfig(tap);
    TapState  s2 = TapManager::getInstance().getState(tap);
    float kegPct = (c2.kegSize > 0) ? constrain(s2.currentKegLevel / c2.kegSize, 0.0f, 1.0f) : 0;
    uint16_t kegCol = levelColor(d, kegPct);

    drawGlassPanel(d, 6, 266, 468, 48, 12);
    d.setTextColor(d.color565(150, 154, 162));
    d.setTextSize(1);
    d.setCursor(18, 274);
    d.printf("KEG REMAINING: %.0f oz (%.0f%%)", s2.currentKegLevel, kegPct * 100);
    d.fillSmoothRoundRect(18, 290, 444, 14, 7, d.color565(50, 53, 60));
    int fill = (int)(kegPct * 440);
    if (fill > 0)
        d.fillSmoothRoundRect(20, 292, fill, 10, 5, kegCol);

    d.endWrite();
}

// ============================================================
// Pour complete screen
// ============================================================
void DisplayManager::drawTapPourComplete(int tap) {
    TapConfig cfg = TapManager::getInstance().getConfig(tap);
    LGFX_TFT& d = m_display[tap];
    uint16_t beerColor = hexToColor(d, cfg.beerColor);

    d.startWrite();

    // Deep green background gradient — reads as a clean "success" screen
    for (int y = 0; y < dispH(tap); y++) {
        float t = (float)y / dispH(tap);
        d.drawFastHLine(0, y, dispW(tap), d.color565(6, 12 + (uint8_t)(10 * t), 8));
    }

    uint16_t green = d.color565(0, 220, 100);
    d.drawRect(0, 0, dispW(tap),     dispH(tap),     green);
    d.drawRect(1, 1, dispW(tap) - 2, dispH(tap) - 2, green);
    d.drawRect(2, 2, dispW(tap) - 4, dispH(tap) - 4, green);

    // Sparkle particles orbiting the checkmark — subtle animated flourish
    for (int s = 0; s < 8; s++) {
        float ang = (s * 45.0f + m_animFrame * 6.0f) * DEG_TO_RAD;
        int sr = 78 + (s % 2) * 6;
        int sx = 240 + (int)(cosf(ang) * sr);
        int sy = 95  + (int)(sinf(ang) * sr);
        d.fillSmoothCircle(sx, sy, 2, d.color565(180, 255, 210));
    }

    // Checkmark circle
    d.fillSmoothCircle(240, 95, 60, d.color565(0, 50, 30));
    d.drawCircle(240, 95, 60, green);
    d.drawLine(211, 95,  229, 116, green);
    d.drawLine(212, 95,  230, 116, green);
    d.drawLine(229, 116, 269,  74, green);
    d.drawLine(230, 116, 270,  74, green);

    d.setTextColor(green);
    d.setTextSize(3);
    String doneLbl = "POUR DONE!";
    d.setCursor(240 - d.textWidth(doneLbl) / 2, 170);
    d.print(doneLbl);

    d.setTextColor(beerColor);
    d.setTextSize(2);
    String beerLbl = cfg.beerName.substring(0, 22);
    d.setCursor(240 - d.textWidth(beerLbl) / 2, 198);
    d.print(beerLbl);

    // Big oz number
    d.setTextColor(TFT_WHITE);
    d.setTextSize(5);
    String ozStr = String(m_tap[tap].lastOz, 1);
    d.setCursor(240 - d.textWidth(ozStr) / 2, 225);
    d.print(ozStr);
    d.setTextSize(2);
    d.setCursor(240 + d.textWidth(ozStr) / 2 + 6, 245);
    d.print("oz");

    // Duration
    d.setTextColor(d.color565(180, 200, 190));
    d.setTextSize(1);
    String durLbl = "in " + String(m_tap[tap].lastDuration, 1) + " seconds";
    d.setCursor(240 - d.textWidth(durLbl) / 2, 302);
    d.print(durLbl);

    d.endWrite();
}

// ============================================================
// Main update loop
// ============================================================
void DisplayManager::update() {
    if (!m_available) return;
    unsigned long now = millis();

    static unsigned long lastAnim = 0;
    if (now - lastAnim >= 500) {
        m_animFrame++;
        lastAnim = now;
    }

    for (int i = 0; i < NUM_TAPS; i++) {
        TapDisplay& td = m_tap[i];

        if (!m_displayOk[i]) continue;

        if (td.pouring) {
            if (now - td.lastDraw >= 500) {
                drawTapPouring(i);
                td.lastDraw = now;
            }
        } else if (td.showComplete && td.completeUntil > now) {
            if (now - td.lastDraw >= 1000) {
                drawTapPourComplete(i);
                td.lastDraw = now;
            }
        } else {
            if (td.showComplete) td.showComplete = false;
            if (now - td.lastDraw >= 15000) {
                drawTapIdle(i);
                td.lastDraw = now;
            }
        }
    }
}

// ============================================================
// Event hooks
// ============================================================
void DisplayManager::onPourStart(int tapIndex) {
    if (!m_available || tapIndex >= NUM_TAPS) return;
    m_tap[tapIndex].pouring   = true;
    m_tap[tapIndex].lastDraw  = 0;
}

void DisplayManager::onPourEnd(int tapIndex, float ounces, float duration) {
    if (!m_available || tapIndex >= NUM_TAPS) return;
    TapDisplay& td     = m_tap[tapIndex];
    td.pouring         = false;
    td.showComplete    = true;
    td.completeUntil   = millis() + 5000;
    td.lastOz          = ounces;
    td.lastDuration    = duration;
    td.lastDraw        = 0;
}

void DisplayManager::showLowKegAlert(int tapIndex, float level) {
    if (!m_available || tapIndex >= NUM_TAPS) return;
    m_tap[tapIndex].lastDraw = 0;
    (void)level;
}
