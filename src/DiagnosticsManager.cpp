#include "DiagnosticsManager.h"
#include "TapManager.h"
#include <ArduinoJson.h>
#include <cmath>

// ============================================================
// Singleton
// ============================================================
DiagnosticsManager& DiagnosticsManager::getInstance() {
    static DiagnosticsManager instance;
    return instance;
}

DiagnosticsManager::DiagnosticsManager() {
    // Built-in keg profiles (common US keg sizes)
    m_profiles = {
        {"Half Barrel",    1984.0f, "Full US 1/2 bbl — 15.5 gal / 165 12oz pours"},
        {"Quarter Barrel", 992.0f,  "Pony keg — 7.75 gal / 82 12oz pours"},
        {"Sixth Barrel",   661.0f,  "Torpedo keg — 5.16 gal / 55 12oz pours"},
        {"Cornelius 5gal", 640.0f,  "Homebrewer corny keg"},
        {"Cornelius 3gal", 384.0f,  "Small corny keg"},
        {"Mini Keg",       169.0f,  "Heineken-style 5L mini keg"},
    };
}

// ============================================================
// Lifecycle
// ============================================================
void DiagnosticsManager::begin() {
    for (int i = 0; i < NUM_TAPS; i++) {
        // Reset fields individually — do NOT assign TapDiagnostics{} which destroys
        // the already-constructed std::vector and corrupts the heap on ESP32-S3
        m_diag[i].health        = SensorHealth::NO_SIGNAL;
        m_diag[i].pulseQuality  = 0.0f;
        m_diag[i].avgIntervalMs = 0.0f;
        m_diag[i].stdDevMs      = 0.0f;
        m_diag[i].noisePulses   = 0;
        m_diag[i].totalPulses   = 0;
        m_diag[i].lastPulseMs   = 0;
        m_diag[i].instantFlowOz = 0.0f;
        m_diag[i].avgFlowOz     = 0.0f;
        m_diag[i].peakFlowOz    = 0.0f;
        m_diag[i].totalPouringMs = 0;
        m_diag[i].avgPourSizeOz  = 0.0f;
        m_diag[i].poursPerDay    = 0.0f;
        m_diag[i].estimatedDaysLeft = 0.0f;
        m_diag[i].calibrated    = false;
        m_diag[i].intervals.clear();
        m_diag[i].intervals.reserve(DIAG_WINDOW_SIZE);
        m_pourRecords[i].clear();

        TapConfig cfg = TapManager::getInstance().getConfig(i);
        m_diag[i].pulsesPerOz = cfg.pulsesPerOz;
        m_diag[i].calibrated  = (cfg.pulsesPerOz != 450.0f);
    }
    m_lastUpdate = millis();
    LOG_I("DiagnosticsManager ready");
}

void DiagnosticsManager::update() {
    unsigned long now = millis();
    if (now - m_lastUpdate < 1000) return;
    m_lastUpdate = now;

    for (int i = 0; i < NUM_TAPS; i++) {
        updateHealth(i);
        updateProjections(i);
    }
}

// ============================================================
// Event handlers
// ============================================================
void DiagnosticsManager::onPulse(int i, unsigned long intervalMs) {
    if (!validIndex(i)) return;
    TapDiagnostics& d = m_diag[i];

    d.totalPulses++;
    d.lastPulseMs = millis();

    // Rolling interval window
    if ((int)d.intervals.size() >= DIAG_WINDOW_SIZE) {
        d.intervals.erase(d.intervals.begin());
    }
    d.intervals.push_back({(float)intervalMs, millis()});

    // Running average interval
    if (!d.intervals.empty()) {
        float sum = 0;
        for (const auto& iv : d.intervals) sum += iv.intervalMs;
        d.avgIntervalMs = sum / d.intervals.size();
        d.stdDevMs = calcStdDev(d.intervals, d.avgIntervalMs);
    }

    // Instant flow rate: pulses/ms → oz/sec
    TapConfig cfg = TapManager::getInstance().getConfig(i);
    if (cfg.pulsesPerOz > 0 && intervalMs > 0) {
        d.instantFlowOz = (1000.0f / intervalMs) / cfg.pulsesPerOz;
    }

    d.pulsesPerOz = cfg.pulsesPerOz;
}

void DiagnosticsManager::onNoisePulse(int i) {
    if (!validIndex(i)) return;
    m_diag[i].noisePulses++;
}

void DiagnosticsManager::onPourComplete(int i, float ounces, float duration, float peakFlow) {
    if (!validIndex(i)) return;
    TapDiagnostics& d = m_diag[i];

    d.avgFlowOz  = (duration > 0) ? ounces / duration : 0;
    if (peakFlow > d.peakFlowOz) d.peakFlowOz = peakFlow;
    d.totalPouringMs += (unsigned long)(duration * 1000);
    d.instantFlowOz = 0.0f;

    // Store for rate projection
    if ((int)m_pourRecords[i].size() >= RATE_WINDOW) {
        m_pourRecords[i].erase(m_pourRecords[i].begin());
    }
    m_pourRecords[i].push_back({ounces, millis()});
}

// ============================================================
// Health assessment
// ============================================================
void DiagnosticsManager::updateHealth(int i) {
    TapDiagnostics& d = m_diag[i];
    TapConfig cfg = TapManager::getInstance().getConfig(i);
    d.pulsesPerOz = cfg.pulsesPerOz;
    d.calibrated  = (cfg.pulsesPerOz != 450.0f);

    if (!d.calibrated) {
        d.health = SensorHealth::CALIBRATION_NEEDED;
        d.pulseQuality = 50.0f;
        return;
    }

    if (d.totalPulses == 0) {
        d.health = SensorHealth::NO_SIGNAL;
        d.pulseQuality = 0.0f;
        return;
    }

    // Stuck: currently "pouring" for very long without stopping
    TapState st = TapManager::getInstance().getState(i);
    if (st.isPouring && (millis() - st.pourStartTime) > STUCK_PULSE_TIMEOUT_MS) {
        d.health = SensorHealth::STUCK;
        d.pulseQuality = 10.0f;
        return;
    }

    // Noise: high stdDev relative to mean interval
    float noiseRatio = (d.avgIntervalMs > 0) ? (d.stdDevMs / d.avgIntervalMs) : 0;
    float noiseScore = d.totalPulses > 0
        ? 100.0f * (1.0f - (float)d.noisePulses / max((uint32_t)1, d.totalPulses))
        : 100.0f;

    if (noiseRatio > 0.8f || noiseScore < 70.0f) {
        d.health = SensorHealth::NOISY;
        d.pulseQuality = noiseScore * 0.5f;
        return;
    }

    d.health = SensorHealth::GOOD;
    // Quality: penalise noise, reward consistent intervals
    d.pulseQuality = constrain(noiseScore - noiseRatio * 20.0f, 0.0f, 100.0f);
}

void DiagnosticsManager::updateProjections(int i) {
    TapDiagnostics& d = m_diag[i];
    auto& records = m_pourRecords[i];
    if (records.size() < 2) return;

    // Average pour size
    float totalOz = 0;
    for (const auto& r : records) totalOz += r.ounces;
    d.avgPourSizeOz = totalOz / records.size();

    // Pours per day (using time span of records)
    unsigned long span = records.back().timestamp - records.front().timestamp;
    if (span > 0) {
        float days = span / (float)(86400000UL);
        if (days > 0.01f) {
            d.poursPerDay = (records.size() - 1) / days;
        }
    }

    // Days left
    TapState st = TapManager::getInstance().getState(i);
    float ozPerDay = d.poursPerDay * d.avgPourSizeOz;
    if (ozPerDay > 0) {
        d.estimatedDaysLeft = st.currentKegLevel / ozPerDay;
    }
}

// ============================================================
// Keg profiles
// ============================================================
void DiagnosticsManager::setKegProfile(int tapIndex, int profileIndex) {
    if (!validIndex(tapIndex)) return;
    if (profileIndex < 0 || profileIndex >= (int)m_profiles.size()) return;
    const KegProfile& p = m_profiles[profileIndex];
    TapManager::getInstance().setKegSize(tapIndex, p.sizeOz);
    LOG_I("Tap %d profile set to: %s (%.0f oz)", tapIndex + 1, p.name.c_str(), p.sizeOz);
}

String DiagnosticsManager::getProfilesJSON() const {
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < (int)m_profiles.size(); i++) {
        JsonObject o = arr.createNestedObject();
        o["id"]   = i;
        o["name"] = m_profiles[i].name;
        o["oz"]   = m_profiles[i].sizeOz;
        o["desc"] = m_profiles[i].description;
    }
    String out; serializeJson(arr, out);
    return out;
}

// ============================================================
// JSON accessors
// ============================================================
const TapDiagnostics& DiagnosticsManager::getDiag(int i) const {
    static TapDiagnostics empty;
    return validIndex(i) ? m_diag[i] : empty;
}

String DiagnosticsManager::getDiagJSON(int i) const {
    if (!validIndex(i)) return "{}";
    const TapDiagnostics& d = m_diag[i];
    DynamicJsonDocument doc(512);
    doc["health"]         = d.healthString();
    doc["pulseQuality"]   = d.pulseQuality;
    doc["avgIntervalMs"]  = d.avgIntervalMs;
    doc["stdDevMs"]       = d.stdDevMs;
    doc["noisePulses"]    = d.noisePulses;
    doc["totalPulses"]    = d.totalPulses;
    doc["instantFlowOz"]  = d.instantFlowOz;
    doc["avgFlowOz"]      = d.avgFlowOz;
    doc["peakFlowOz"]     = d.peakFlowOz;
    doc["avgPourSizeOz"]  = d.avgPourSizeOz;
    doc["poursPerDay"]    = d.poursPerDay;
    doc["daysLeft"]       = d.estimatedDaysLeft;
    doc["calibrated"]     = d.calibrated;
    doc["pulsesPerOz"]    = d.pulsesPerOz;
    String out; serializeJson(doc, out);
    return out;
}

String DiagnosticsManager::getAllDiagJSON() const {
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < NUM_TAPS; i++) {
        JsonObject o = arr.createNestedObject();
        DynamicJsonDocument d(512);
        deserializeJson(d, getDiagJSON(i));
        o["tap"] = i + 1;
        for (JsonPair kv : d.as<JsonObject>()) o[kv.key()] = kv.value();
    }
    String out; serializeJson(arr, out);
    return out;
}

String DiagnosticsManager::getSensorReportJSON() const {
    DynamicJsonDocument doc(2048);
    doc["timestamp"]  = millis() / 1000;
    doc["firmwareVersion"] = FIRMWARE_VERSION;
    JsonArray taps = doc.createNestedArray("taps");
    for (int i = 0; i < NUM_TAPS; i++) {
        JsonObject o = taps.createNestedObject();
        TapConfig cfg = TapManager::getInstance().getConfig(i);
        const TapDiagnostics& d = m_diag[i];
        o["index"]      = i;
        o["name"]       = cfg.tapName;
        o["health"]     = d.healthString();
        o["quality"]    = d.pulseQuality;
        o["calibrated"] = d.calibrated;
        o["warning"]    = hasSensorWarning(i) ? getSensorWarningMessage(i) : "";
    }
    String out; serializeJson(doc, out);
    return out;
}

bool DiagnosticsManager::hasSensorWarning(int i) const {
    if (!validIndex(i)) return false;
    return m_diag[i].health != SensorHealth::GOOD;
}

String DiagnosticsManager::getSensorWarningMessage(int i) const {
    if (!validIndex(i)) return "";
    switch (m_diag[i].health) {
        case SensorHealth::NOISY:              return "High noise on flow sensor. Check wiring or add pull-down resistor.";
        case SensorHealth::STUCK:             return "Sensor may be jammed or line blocked. Inspect tap.";
        case SensorHealth::NO_SIGNAL:         return "No pulses detected. Check sensor wiring and pin config.";
        case SensorHealth::CALIBRATION_NEEDED: return "Tap has never been calibrated. Pour accuracy may be poor.";
        case SensorHealth::GOOD:              return "";
    }
    return "";
}

// ============================================================
// Utility
// ============================================================
float DiagnosticsManager::calcStdDev(const std::vector<PulseInterval>& iv, float mean) const {
    if (iv.size() < 2) return 0;
    float sum = 0;
    for (const auto& x : iv) {
        float d = x.intervalMs - mean;
        sum += d * d;
    }
    return sqrtf(sum / iv.size());
}
