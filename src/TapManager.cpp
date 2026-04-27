#include "TapManager.h"
#include <Preferences.h>
#include <ArduinoJson.h>

// ============================================================
// Singleton
// ============================================================

TapManager& TapManager::getInstance() {
    static TapManager instance;
    return instance;
}

// ============================================================
// ISR — one registered per tap via isrDispatch + arg pointer
// ============================================================

// Shim struct so we can pass tapIndex through the void* arg
struct IsrArg { TapManager* mgr; int idx; };
static IsrArg s_isrArgs[NUM_TAPS];

void IRAM_ATTR TapManager::isrDispatch(void* arg) {
    auto* a = reinterpret_cast<IsrArg*>(arg);
    a->mgr->onPulse(a->idx);
}

void IRAM_ATTR TapManager::onPulse(int tapIndex) {
    unsigned long now = millis();
    TapState& s = m_state[tapIndex];

    if (now - s.lastPulseTime < PULSE_DEBOUNCE_MS) return;

    s.pulseCount = s.pulseCount + 1;
    s.totalPulsesDetected++;
    s.lastPulseTime = now;

    if (s.calibrating) {
        s.calibPulses = s.calibPulses + 1;
    }

    // Only declare a pour after POUR_MIN_PULSES consecutive pulses (noise filter)
    if (!s.isPouring) {
        s.pendingPulses++;
        if (s.pendingPulses >= POUR_MIN_PULSES && m_activeTap == -1) {
            m_activeTap       = tapIndex;
            s.pourStartTime   = now;
            s.isPouring       = true;
            s.peakFlowRate    = 0.0f;
            s.lastFlowCalc    = now;
        }
    }
}

// ============================================================
// Lifecycle
// ============================================================

void TapManager::begin() {
    LOG_I("TapManager initializing (%d taps)", NUM_TAPS);

    for (int i = 0; i < NUM_TAPS; i++) {
        // Reset runtime state
        m_state[i] = TapState{};
        m_config[i].tapName = "Tap " + String(i + 1);

        pinMode(TAP_PINS[i], INPUT_PULLUP);

        // Attach individual ISR per pin using gpio_isr_handler_add
        s_isrArgs[i] = {this, i};
        gpio_set_intr_type((gpio_num_t)TAP_PINS[i], GPIO_INTR_POSEDGE);
        gpio_isr_handler_add((gpio_num_t)TAP_PINS[i], isrDispatch, &s_isrArgs[i]);
        gpio_intr_enable((gpio_num_t)TAP_PINS[i]);

        LOG_D("  Tap %d -> pin %d (initial=%s)",
              i + 1, TAP_PINS[i],
              digitalRead(TAP_PINS[i]) ? "HIGH" : "LOW");
    }

    loadConfig();
    m_lastUpdateMs = millis();
    LOG_I("TapManager ready. Waiting for pulses...");
}

void TapManager::update() {
    unsigned long now = millis();

    // Software polling fallback (debounced, catches any ISR-missed edges)
    if (now - m_lastPollMs >= SENSOR_POLL_INTERVAL_MS) {
        static int prevState[NUM_TAPS];
        for (int i = 0; i < NUM_TAPS; i++) {
            int cur = digitalRead(TAP_PINS[i]);
            if (prevState[i] == LOW && cur == HIGH) {
                onPulse(i);  // reuse same logic, already debounced inside
            }
            prevState[i] = cur;
        }
        m_lastPollMs = now;
    }

    updateLivePour();
    checkPourTimeout();
    m_lastUpdateMs = now;
}

// ============================================================
// Pour state machine
// ============================================================

void TapManager::updateLivePour() {
    if (m_activeTap < 0) return;
    int i = m_activeTap;
    TapState&  s = m_state[i];
    TapConfig& c = m_config[i];

    unsigned long pulses = s.calibrating ? s.calibPulses : s.pulseCount;
    if (pulses > 0 && c.pulsesPerOz > 0.0f) {
        s.currentPourOz = (float)pulses / c.pulsesPerOz;
    }

    // Update peak flow rate
    float flow = calcInstantFlowRate(i);
    if (flow > s.peakFlowRate) {
        s.peakFlowRate = flow;
    }
}

float TapManager::calcInstantFlowRate(int i) {
    unsigned long now = millis();
    TapState& s = m_state[i];
    unsigned long elapsed = now - s.pourStartTime;
    if (elapsed == 0) return 0.0f;
    return (s.currentPourOz / (float)elapsed) * 1000.0f; // oz/sec
}

void TapManager::checkPourTimeout() {
    if (m_activeTap < 0) return;
    int i = m_activeTap;
    TapState& s = m_state[i];

    if (millis() - s.lastPulseTime > POUR_TIMEOUT_MS) {
        if (s.isPouring && s.currentPourOz > 0.01f) {
            finalisePour(i);
        }
        // Reset for next pour (preserve calibPulses so endCalibration() can read them)
        s.isPouring     = false;
        s.pendingPulses = 0;
        s.currentPourOz = 0.0f;
        s.pulseCount    = 0;
        if (!s.calibrating) s.calibPulses = 0;
        s.lastFlowCalc  = 0;
        m_activeTap     = -1;
    }
}

void TapManager::finalisePour(int i) {
    TapState&  s = m_state[i];
    TapConfig& c = m_config[i];
    unsigned long now = millis();

    float amount   = s.currentPourOz;
    float duration = (float)(now - s.pourStartTime) / 1000.0f;

    if (amount < POUR_MIN_OZ) {
        LOG_D("Ghost pour discarded - Tap %d: %.2f oz (below %.1f oz threshold)", i + 1, amount, POUR_MIN_OZ);
        s.currentPourOz = 0.0f;
        return;
    }

    float newLevel = max(0.0f, s.currentKegLevel - amount);

    PourEvent e;
    e.timestamp    = now;
    e.ounces       = amount;
    e.duration     = duration;
    e.peakFlowRate = s.peakFlowRate;
    e.kegLevelAfter= newLevel;
    e.beerName     = c.beerName;
    e.dateTime     = formatUptime();

    s.lastPour     = e;
    s.hasLastPour  = true;

    // Maintain circular history buffer
    if (s.history.size() >= HISTORY_SIZE) {
        s.history.erase(s.history.begin());
    }
    s.history.push_back(e);

    s.currentKegLevel = newLevel;
    s.peakFlowRate    = 0.0f;

    m_totalPours++;
    m_totalOunces += amount;

    if (checkLowKegAlert(i)) {
        LOG_W("LOW KEG: Tap %d at %.1f oz", i + 1, newLevel);
    }

    LOG_I("Pour ended - Tap %d: %.2f oz in %.1fs (peak %.1f oz/s, keg %.1f oz)",
          i + 1, amount, duration, e.peakFlowRate, newLevel);

    saveConfig();
}

// ============================================================
// Tap operations
// ============================================================

void TapManager::resetKeg(int i) {
    if (!validIndex(i)) return;
    m_state[i].currentKegLevel  = m_config[i].kegSize;
    m_state[i].history.clear();
    m_state[i].hasLastPour      = false;
    m_state[i].totalPulsesDetected = 0;
    m_state[i].pulseCount       = 0;
    m_state[i].lowKegAlertSent  = false;
    m_state[i].lastAlertLevel   = 0.0f;
    saveConfig();
    LOG_I("Tap %d keg reset to full (%.0f oz)", i + 1, m_config[i].kegSize);
}

void TapManager::setBeerName(int i, const String& name) {
    if (!validIndex(i)) return;
    m_config[i].beerName = name;
    saveConfig();
}

void TapManager::setTapInfo(int i, const String& name, float abv, int ibu, const String& color) {
    if (!validIndex(i)) return;
    m_config[i].beerName  = name;
    m_config[i].abv       = abv;
    m_config[i].ibu       = ibu;
    m_config[i].beerColor = color.length() > 0 ? color : "#c8820a";
    saveConfig();
    LOG_I("Tap %d: %s  ABV=%.1f%%  IBU=%d  color=%s", i+1, name.c_str(), abv, ibu, color.c_str());
}

void TapManager::setTapName(int i, const String& name) {
    if (!validIndex(i)) return;
    m_config[i].tapName = name;
    saveConfig();
}

void TapManager::setKegSize(int i, float sizeOz) {
    if (!validIndex(i)) return;
    m_config[i].kegSize = sizeOz;
    saveConfig();
    LOG_I("Tap %d keg size set to %.0f oz", i + 1, sizeOz);
}

bool TapManager::startCalibration(int i) {
    if (!validIndex(i)) return false;
    m_state[i].calibrating  = true;
    m_state[i].calibPulses  = 0;
    LOG_I("Calibration started on Tap %d", i + 1);
    return true;
}

bool TapManager::endCalibration(int i, float knownOunces) {
    if (!validIndex(i)) return false;
    float pulses = (float)m_state[i].calibPulses;
    if (knownOunces <= 0.0f || pulses <= 0.0f) {
        LOG_E("Calibration failed on Tap %d - no pulses or zero ounces", i + 1);
        m_state[i].calibrating = false;
        return false;
    }
    m_config[i].pulsesPerOz = pulses / knownOunces;
    m_state[i].calibrating  = false;
    saveConfig();
    LOG_I("Calibration complete - Tap %d: %.2f pulses/oz (%lu pulses / %.1f oz)",
          i + 1, m_config[i].pulsesPerOz, (unsigned long)pulses, knownOunces);
    return true;
}

// ============================================================
// Data accessors
// ============================================================

TapConfig TapManager::getConfig(int i) const {
    if (!validIndex(i)) return TapConfig{};
    return m_config[i];
}

TapState TapManager::getState(int i) const {
    if (!validIndex(i)) return TapState{};
    return m_state[i];
}

float TapManager::getCurrentPour(int i) const {
    if (!validIndex(i) || i != m_activeTap) return 0.0f;
    return m_state[i].currentPourOz;
}

PourEvent TapManager::getLastPour(int i) const {
    if (!validIndex(i)) return PourEvent{};
    return m_state[i].lastPour;
}

// ============================================================
// Alert helpers
// ============================================================

bool TapManager::checkLowKegAlert(int i) {
    if (!validIndex(i)) return false;
    TapState& s = m_state[i];
    if (s.currentKegLevel < LOW_KEG_ALERT_OZ && !s.lowKegAlertSent) {
        s.lowKegAlertSent = true;
        s.lastAlertLevel  = s.currentKegLevel;
        return true;
    }
    // Auto-reset alert after keg is refilled
    if (s.currentKegLevel > LOW_KEG_ALERT_OZ + 20.0f) {
        s.lowKegAlertSent = false;
    }
    return false;
}

void TapManager::clearAlert(int i) {
    if (!validIndex(i)) return;
    m_state[i].lowKegAlertSent = false;
}

// ============================================================
// JSON helpers
// ============================================================

String TapManager::getTapStatusJSON() const {
    DynamicJsonDocument doc(4096);
    doc["uptime"]      = millis() / 1000;
    doc["activeTap"]   = m_activeTap;
    doc["totalPours"]  = m_totalPours;
    doc["totalOunces"] = m_totalOunces;

    JsonArray taps = doc.createNestedArray("taps");
    for (int i = 0; i < NUM_TAPS; i++) {
        const TapConfig& c = m_config[i];
        const TapState&  s = m_state[i];
        JsonObject t = taps.createNestedObject();
        t["index"]       = i;
        t["name"]        = c.tapName;
        t["beer"]        = c.beerName;
        t["level"]       = s.currentKegLevel;
        t["levelGal"]    = s.currentKegLevel / 128.0f;
        t["capacity"]    = c.kegSize;
        t["pulsesPerOz"] = c.pulsesPerOz;
        t["isPouring"]   = s.isPouring;
        t["calibrating"] = s.calibrating;
        t["currentPour"] = (i == m_activeTap) ? s.currentPourOz : 0.0f;
        t["pourCount"]   = s.history.size();
        t["lowKegAlert"] = s.lowKegAlertSent;
        t["abv"]         = c.abv;
        t["ibu"]         = c.ibu;
        t["beerColor"]   = c.beerColor;
    }

    String out;
    serializeJson(doc, out);
    return out;
}

String TapManager::getLastPourJSON(int i) const {
    if (!validIndex(i) || !m_state[i].hasLastPour) return "{}";
    DynamicJsonDocument doc(512);
    const PourEvent& p = m_state[i].lastPour;
    doc["ounces"]      = p.ounces;
    doc["beer"]        = p.beerName;
    doc["time"]        = p.dateTime;
    doc["timestamp"]   = p.timestamp;
    doc["kegAfter"]    = p.kegLevelAfter;
    doc["duration"]    = p.duration;
    doc["peakFlow"]    = p.peakFlowRate;
    String out;
    serializeJson(doc, out);
    return out;
}

String TapManager::getPourHistoryJSON(int i) const {
    if (!validIndex(i)) return "[]";
    DynamicJsonDocument doc(8192);
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& p : m_state[i].history) {
        JsonObject o = arr.createNestedObject();
        o["timestamp"]  = p.timestamp;
        o["time"]       = p.dateTime;
        o["ounces"]     = p.ounces;
        o["beer"]       = p.beerName;
        o["kegAfter"]   = p.kegLevelAfter;
        o["duration"]   = p.duration;
        o["peakFlow"]   = p.peakFlowRate;
    }
    String out;
    serializeJson(arr, out);
    return out;
}

String TapManager::getAllHistoryJSON() const {
    DynamicJsonDocument doc(16384);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < NUM_TAPS; i++) {
        for (const auto& p : m_state[i].history) {
            JsonObject o = arr.createNestedObject();
            o["tap"]       = i + 1;
            o["tapName"]   = m_config[i].tapName;
            o["timestamp"] = p.timestamp;
            o["time"]      = p.dateTime;
            o["ounces"]    = p.ounces;
            o["beer"]      = p.beerName;
            o["kegAfter"]  = p.kegLevelAfter;
            o["duration"]  = p.duration;
            o["peakFlow"]  = p.peakFlowRate;
        }
    }
    String out;
    serializeJson(arr, out);
    return out;
}

// ============================================================
// Persistence
// ============================================================

void TapManager::saveConfig() {
    Preferences prefs;
    prefs.begin(NVS_NS_TAPS, false);
    char key[24];
    for (int i = 0; i < NUM_TAPS; i++) {
        snprintf(key, sizeof(key), NVS_KEY_PULSES,    i); prefs.putFloat(key,  m_config[i].pulsesPerOz);
        snprintf(key, sizeof(key), NVS_KEY_KEG_LEVEL, i); prefs.putFloat(key,  m_state[i].currentKegLevel);
        snprintf(key, sizeof(key), NVS_KEY_TAP_NAME,  i); prefs.putString(key, m_config[i].tapName);
        snprintf(key, sizeof(key), NVS_KEY_BEER_NAME,  i); prefs.putString(key, m_config[i].beerName);
        snprintf(key, sizeof(key), "keg_sz_%d",        i); prefs.putFloat(key,  m_config[i].kegSize);
        snprintf(key, sizeof(key), "abv_%d",           i); prefs.putFloat(key,  m_config[i].abv);
        snprintf(key, sizeof(key), "ibu_%d",           i); prefs.putInt(key,    m_config[i].ibu);
        snprintf(key, sizeof(key), "bcolor_%d",        i); prefs.putString(key, m_config[i].beerColor);
    }
    prefs.end();
}

void TapManager::loadConfig() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS_TAPS, true)) {
        LOG_W("No saved tap config found — using defaults");
        return;
    }
    char key[24];
    for (int i = 0; i < NUM_TAPS; i++) {
        snprintf(key, sizeof(key), NVS_KEY_PULSES,    i); m_config[i].pulsesPerOz     = prefs.getFloat(key,  450.0f);
        snprintf(key, sizeof(key), NVS_KEY_KEG_LEVEL, i); m_state[i].currentKegLevel  = prefs.getFloat(key,  KEG_SIZE_OZ);
        snprintf(key, sizeof(key), NVS_KEY_TAP_NAME,  i); m_config[i].tapName         = prefs.getString(key, "Tap " + String(i + 1));
        snprintf(key, sizeof(key), NVS_KEY_BEER_NAME,  i); m_config[i].beerName        = prefs.getString(key, "Unknown");
        snprintf(key, sizeof(key), "keg_sz_%d",        i); m_config[i].kegSize         = prefs.getFloat(key,  KEG_SIZE_OZ);
        snprintf(key, sizeof(key), "abv_%d",           i); m_config[i].abv             = prefs.getFloat(key,  0.0f);
        snprintf(key, sizeof(key), "ibu_%d",           i); m_config[i].ibu             = prefs.getInt(key,    0);
        snprintf(key, sizeof(key), "bcolor_%d",        i); m_config[i].beerColor       = prefs.getString(key, "#c8820a");
    }
    prefs.end();
    LOG_I("Tap config loaded from NVS");
}

// ============================================================
// Debug
// ============================================================

void TapManager::printPinStatus() const {
    LOG_I("--- Pin Status ---");
    for (int i = 0; i < NUM_TAPS; i++) {
        LOG_I("  Tap %d pin %-2d | %s | pulses=%lu beer=%s level=%.1f lastPour=%.2f oz",
              i + 1, TAP_PINS[i],
              digitalRead(TAP_PINS[i]) ? "HIGH" : "LOW",
              m_state[i].totalPulsesDetected,
              m_config[i].beerName.c_str(),
              m_state[i].currentKegLevel,
              m_state[i].hasLastPour ? m_state[i].lastPour.ounces : 0.0f);
    }
}

// ============================================================
// Utilities
// ============================================================

String TapManager::formatUptime() {
    unsigned long s = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu",
             s / 86400, (s % 86400) / 3600, (s % 3600) / 60, s % 60);
    return String(buf);
}