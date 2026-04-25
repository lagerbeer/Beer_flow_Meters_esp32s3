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

// Simple thick progress bar
void DisplayManager::drawKegArc(LGFX_Sprite& spr, int cx, int cy, int r, float pct) {
    pct = constrain(pct, 0.0f, 1.0f);
    int bw = r * 2, bh = 22;
    int bx = cx - r, by = cy - bh / 2;
    (void)cx; (void)cy; (void)r;
    spr.fillRoundRect(bx, by, bw, bh, bh / 2, spr.color565(50, 50, 50));
    int fill = (int)(pct * (bw - 4));
    if (fill > 0) {
        uint16_t col = spr.color565(
            pct > 0.7f ? 0   : (pct > 0.3f ? 220 : 255),
            pct > 0.7f ? 200 : (pct > 0.3f ? 220 : (pct > 0.1f ? 140 : 0)),
            0
        );
        spr.fillRoundRect(bx + 2, by + 2, fill, bh - 4, (bh - 4) / 2, col);
    }
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
    LGFX_Sprite spr(&m_display[tap]);
    spr.setColorDepth(8);
    spr.createSprite(dispW(tap), dispH(tap));
    spr.fillScreen(TFT_BLACK);

    // Gradient background
    for (int i = 0; i < dispH(tap); i++) {
        uint8_t b = map(i, 0, dispH(tap), 20, 0);
        spr.drawFastHLine(0, i, dispW(tap), spr.color565(0, 0, b));
    }

    // Beer mug icon (centred at x=240 for 480px wide display)
    spr.fillRect(207, 95, 70, 100, amberColor(m_display[tap]));   // mug body
    spr.fillRect(277, 115, 22, 60, amberColor(m_display[tap]));   // handle
    spr.drawRect(277, 115, 22, 60, TFT_WHITE);                     // handle outline
    spr.fillRoundRect(200, 82, 84, 22, 6, TFT_WHITE);             // foam top
    spr.fillCircle(222, 108, 4, TFT_WHITE);                        // bubble 1
    spr.fillCircle(255, 115, 3, TFT_WHITE);                        // bubble 2

    spr.setTextColor(TFT_WHITE);
    spr.setTextSize(3);
    spr.setCursor(159, 35);   spr.print("Beer Flow");
    spr.setTextSize(2);
    spr.setCursor(198, 68);   spr.print("Monitor");

    spr.setTextColor(amberColor(m_display[tap]));
    spr.setTextSize(3);
    spr.setCursor(195, 220);
    spr.printf("TAP %d", tap + 1);

    spr.setTextColor(spr.color565(150, 150, 150));
    spr.setTextSize(1);
    spr.setCursor(222, 265);
    spr.printf("v%s", FIRMWARE_VERSION);
    spr.setCursor(192, 285);
    spr.print("Initialising...");

    spr.pushSprite(0, 0);
    spr.deleteSprite();
}

// ============================================================
// Idle screen
// ============================================================
void DisplayManager::drawTapIdle(int tap) {
    TapConfig cfg = TapManager::getInstance().getConfig(tap);
    TapState  st  = TapManager::getInstance().getState(tap);
    float pct = (cfg.kegSize > 0) ? constrain(st.currentKegLevel / cfg.kegSize, 0.0f, 1.0f) : 0;

    LGFX_Sprite spr(&m_display[tap]);
    spr.setColorDepth(8);
    if (!spr.createSprite(dispW(tap), dispH(tap))) {
        LOG_W("Tap %d: sprite alloc failed (heap=%d), drawing direct", tap + 1, (int)ESP.getFreeHeap());
        TapConfig cfg2 = TapManager::getInstance().getConfig(tap);
        TapState  st2  = TapManager::getInstance().getState(tap);
        float pct2 = cfg2.kegSize > 0 ? st2.currentKegLevel / cfg2.kegSize : 0;
        uint16_t amber = m_display[tap].color565(255, 176, 0);
        m_display[tap].fillScreen(TFT_BLACK);
        // Header
        m_display[tap].fillRect(0, 0, dispW(tap), 60, m_display[tap].color565(35,35,35));
        m_display[tap].fillCircle(36, 30, 24, amber);
        m_display[tap].setTextColor(TFT_BLACK);
        m_display[tap].setTextSize(3);
        m_display[tap].setCursor(tap < 9 ? 24 : 15, 19);
        m_display[tap].printf("%d", tap + 1);
        m_display[tap].setTextColor(TFT_WHITE);
        m_display[tap].setTextSize(2);
        m_display[tap].setCursor(74, 10);
        m_display[tap].print(cfg2.tapName.substring(0, 22));
        m_display[tap].setTextColor(amber);
        m_display[tap].setCursor(74, 35);
        m_display[tap].print(cfg2.beerName.substring(0, 22));
        // Big oz
        m_display[tap].setTextColor(TFT_WHITE);
        m_display[tap].setTextSize(4);
        m_display[tap].setCursor(140, 90);
        m_display[tap].printf("%d", (int)st2.currentKegLevel);
        m_display[tap].setTextColor(m_display[tap].color565(80,80,80));
        m_display[tap].setTextSize(2);
        m_display[tap].setCursor(228, 148);
        m_display[tap].print("oz");
        // Pct
        m_display[tap].setTextColor(levelColor(m_display[tap], pct2));
        m_display[tap].setTextSize(3);
        m_display[tap].setCursor(160, 185);
        m_display[tap].printf("%d%%", (int)(pct2*100));
        // Bar
        m_display[tap].fillRoundRect(10, 252, 460, 20, 10, m_display[tap].color565(40,40,40));
        int bfill = (int)(pct2 * 456);
        if (bfill > 0)
            m_display[tap].fillRoundRect(12, 254, bfill, 16, 8, levelColor(m_display[tap], pct2));
        return;
    }
    spr.fillScreen(TFT_BLACK);

    uint16_t darkGray  = spr.color565(35, 35, 35);
    uint16_t midGray   = spr.color565(80, 80, 80);
    uint16_t amber     = amberColor(m_display[tap]);

    // ── Header bar ───────────────────────────────────────────
    spr.fillRect(0, 0, dispW(tap), 60, darkGray);

    // Tap number badge
    spr.fillCircle(36, 30, 24, amber);
    spr.setTextColor(TFT_BLACK);
    spr.setTextSize(3);
    spr.setCursor(tap < 9 ? 24 : 15, 19);
    spr.printf("%d", tap + 1);

    // Tap name
    spr.setTextColor(TFT_WHITE);
    spr.setTextSize(2);
    spr.setCursor(74, 10);
    spr.print(cfg.tapName.substring(0, 22));

    // Beer name
    spr.setTextColor(amber);
    spr.setTextSize(2);
    spr.setCursor(74, 35);
    spr.print(cfg.beerName.substring(0, 22));

    // WiFi/status dot top-right
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    spr.fillCircle(468, 16, 6, wifiOk ? spr.color565(0, 200, 0) : spr.color565(200, 0, 0));

    // ── Big oz number ─────────────────────────────────────────
    spr.setTextColor(TFT_WHITE);
    spr.setTextSize(4);
    String ozStr = String((int)st.currentKegLevel);
    int ozW = ozStr.length() * 24;
    spr.setCursor(240 - ozW / 2, 80);
    spr.print(ozStr);

    spr.setTextColor(midGray);
    spr.setTextSize(2);
    spr.setCursor(228, 148);
    spr.print("oz");

    // Percentage
    spr.setTextColor(levelColor(m_display[tap], pct));
    spr.setTextSize(3);
    String pctStr = String((int)(pct * 100)) + "%";
    int pctW = pctStr.length() * 18;
    spr.setCursor(240 - pctW / 2, 185);
    spr.print(pctStr);

    spr.setTextColor(midGray);
    spr.setTextSize(1);
    spr.setCursor(130, 228);
    spr.printf("of %.0f oz total", cfg.kegSize);

    // ── Progress bar ──────────────────────────────────────────
    drawKegArc(spr, 240, 262, 180, pct);

    // ── Last pour panel ───────────────────────────────────────
    spr.fillRoundRect(5, 280, 470, 34, 8, darkGray);
    if (st.hasLastPour) {
        spr.setTextColor(midGray);
        spr.setTextSize(1);
        spr.setCursor(15, 288);
        spr.print("Last:");
        spr.setTextColor(TFT_WHITE);
        spr.setCursor(70, 288);
        spr.printf("%.1f oz", st.lastPour.ounces);
        spr.setTextColor(midGray);
        spr.setCursor(200, 288);
        spr.printf("%.0fs", st.lastPour.duration);
        spr.setCursor(290, 288);
        spr.printf("%.1f oz/s", st.lastPour.peakFlowRate);
    } else {
        spr.setTextColor(midGray);
        spr.setTextSize(1);
        spr.setCursor(175, 291);
        spr.print("No pours yet");
    }

    spr.pushSprite(0, 0);
    spr.deleteSprite();
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

    LGFX_Sprite spr(&m_display[tap]);
    spr.setColorDepth(8);
    if (!spr.createSprite(dispW(tap), dispH(tap))) {
        LOG_W("Tap %d: pour sprite alloc failed", tap + 1);
        TapState st2 = TapManager::getInstance().getState(tap);
        m_display[tap].fillScreen(TFT_BLACK);
        m_display[tap].setTextColor(m_display[tap].color565(255,176,0));
        m_display[tap].setTextSize(2);
        m_display[tap].setCursor(10, 10);
        m_display[tap].printf("TAP %d  POURING", tap+1);
        m_display[tap].setTextColor(TFT_WHITE);
        m_display[tap].setTextSize(4);
        m_display[tap].setCursor(20, 90);
        m_display[tap].printf("%.2f", st2.currentPourOz);
        m_display[tap].setTextSize(2);
        m_display[tap].setCursor(20, 150);
        m_display[tap].print("oz");
        return;
    }
    spr.fillScreen(TFT_BLACK);

    uint16_t amber    = amberColor(m_display[tap]);
    uint16_t darkGray = spr.color565(35, 35, 35);

    // Pulsing border
    uint16_t borderCol = (m_animFrame % 2 == 0) ? amber : spr.color565(200, 120, 0);
    spr.drawRect(0, 0, dispW(tap), dispH(tap), borderCol);
    spr.drawRect(2, 2, dispW(tap) - 4, dispH(tap) - 4, borderCol);

    // ── Header ────────────────────────────────────────────────
    spr.fillRect(3, 3, dispW(tap) - 6, 50, darkGray);
    spr.setTextColor(amber);
    spr.setTextSize(2);
    spr.setCursor(10, 10);
    spr.printf("TAP %d  POURING", tap + 1);
    spr.setTextColor(TFT_WHITE);
    spr.setTextSize(1);
    spr.setCursor(10, 38);
    spr.print(cfg.beerName.substring(0, 40));

    // ── Beer glass ────────────────────────────────────────────
    int bx = 20, by = 62, bw = 100, gh = 195;
    spr.drawLine(bx,        by + gh, bx + bw,      by + gh, TFT_WHITE);
    spr.drawLine(bx,        by + gh, bx - 10,       by,     TFT_WHITE);
    spr.drawLine(bx + bw,   by + gh, bx + bw + 10,  by,     TFT_WHITE);
    spr.drawFastHLine(bx - 10, by, bw + 21,                  TFT_WHITE);

    // Beer fill
    int liquidH = (int)(fillPct * (gh - 10));
    for (int row = 0; row < liquidH; row++) {
        float t  = (float)row / gh;
        int lx   = (int)(bx - 10 * t) + 2;
        int lw   = (int)(bw + 20 * t) - 4;
        int ly   = by + gh - 5 - row;
        uint8_t bright = map(row, 0, liquidH, 120, 200);
        spr.drawFastHLine(lx, ly, lw, spr.color565(bright, (uint8_t)(bright * 0.6f), 0));
    }
    // Foam
    if (liquidH > 5) {
        int foamY = by + gh - 5 - liquidH;
        spr.fillRect(bx - 8, foamY - 6, bw + 18, 10, TFT_WHITE);
    }
    // Bubbles
    if (liquidH > 20) {
        int bubbleY = by + gh - 20 - (m_animFrame * 10 % max(liquidH - 12, 1));
        spr.fillCircle(bx + bw / 2 - 10, bubbleY,       4, spr.color565(255, 220, 100));
        spr.fillCircle(bx + bw / 2 + 16,  bubbleY - 16, 3, spr.color565(255, 220, 100));
    }

    // ── Stats (right of glass) ────────────────────────────────
    spr.setTextColor(TFT_WHITE);
    spr.setTextSize(4);
    spr.setCursor(155, 70);
    spr.printf("%.2f", st.currentPourOz);
    spr.setTextSize(2);
    spr.setCursor(155, 125);
    spr.print("oz");

    spr.setTextColor(spr.color565(0, 220, 220));
    spr.setTextSize(1);
    spr.setCursor(155, 160);
    spr.printf("%.2f oz/s", flowRate);

    spr.setTextColor(spr.color565(200, 200, 200));
    spr.setCursor(155, 178);
    spr.printf("%lu seconds", elapsed);

    spr.setTextColor(amber);
    spr.setTextSize(2);
    spr.setCursor(155, 200);
    for (int d = 0; d < (int)(m_animFrame % 4); d++) spr.print(".");

    // ── Keg remaining bar ─────────────────────────────────────
    TapConfig c2 = TapManager::getInstance().getConfig(tap);
    TapState  s2 = TapManager::getInstance().getState(tap);
    float kegPct = (c2.kegSize > 0) ? constrain(s2.currentKegLevel / c2.kegSize, 0.0f, 1.0f) : 0;

    spr.fillRoundRect(5, 280, 470, 34, 8, spr.color565(35, 35, 35));
    spr.setTextColor(spr.color565(150, 150, 150));
    spr.setTextSize(1);
    spr.setCursor(12, 287);
    spr.printf("Keg: %.0f oz  (%.0f%%)", s2.currentKegLevel, kegPct * 100);
    spr.drawRect(12, 300, 450, 10, TFT_WHITE);
    int fill = (int)(kegPct * 448);
    if (fill > 0)
        spr.fillRect(13, 301, fill, 8, levelColor(m_display[tap], kegPct));

    spr.pushSprite(0, 0);
    spr.deleteSprite();
}

// ============================================================
// Pour complete screen
// ============================================================
void DisplayManager::drawTapPourComplete(int tap) {
    LGFX_Sprite spr(&m_display[tap]);
    spr.setColorDepth(8);
    spr.createSprite(dispW(tap), dispH(tap));
    spr.fillScreen(TFT_BLACK);

    uint16_t green = spr.color565(0, 210, 0);
    uint16_t amber = amberColor(m_display[tap]);

    // Green border
    spr.drawRect(0, 0, dispW(tap), dispH(tap), green);
    spr.drawRect(2, 2, dispW(tap) - 4, dispH(tap) - 4, green);

    // Checkmark circle (centred at x=240 for 480px wide)
    spr.fillCircle(240, 95, 60, spr.color565(0, 60, 0));
    spr.drawCircle(240, 95, 60, green);
    // Check mark scaled to new circle centre (240, 95) radius 60
    spr.drawLine(211, 95,  229, 116, green);
    spr.drawLine(212, 95,  230, 116, green);
    spr.drawLine(229, 116, 269,  74, green);
    spr.drawLine(230, 116, 270,  74, green);

    spr.setTextColor(green);
    spr.setTextSize(3);
    spr.setCursor(150, 170);
    spr.print("POUR DONE!");

    spr.setTextColor(amber);
    spr.setTextSize(1);
    spr.setCursor(215, 210);
    spr.printf("Tap %d", tap + 1);

    // Big oz number
    spr.setTextColor(TFT_WHITE);
    spr.setTextSize(5);
    String ozStr = String(m_tap[tap].lastOz, 1);
    spr.setCursor(240 - (ozStr.length() * 15), 225);
    spr.print(ozStr);
    spr.setTextSize(2);
    spr.setCursor(228, 282);
    spr.print("oz");

    // Duration
    spr.setTextColor(spr.color565(180, 180, 180));
    spr.setTextSize(1);
    spr.setCursor(195, 302);
    spr.printf("in %.1f seconds", m_tap[tap].lastDuration);

    spr.pushSprite(0, 0);
    spr.deleteSprite();
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
