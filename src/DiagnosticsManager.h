#ifndef DIAGNOSTICS_MANAGER_H
#define DIAGNOSTICS_MANAGER_H

#include <Arduino.h>
#include <vector>
#include "config.h"

// ============================================================
// Constants
// ============================================================
#define DIAG_WINDOW_SIZE        32    // rolling pulse interval window
#define RATE_WINDOW             20    // recent pours for rate projection
#define STUCK_PULSE_TIMEOUT_MS  30000 // 30s continuous pour = stuck

// ============================================================
// Sensor health states
// ============================================================
enum class SensorHealth : uint8_t {
    GOOD,
    NOISY,
    STUCK,
    NO_SIGNAL,
    CALIBRATION_NEEDED,
};

// ============================================================
// Rolling pulse interval record
// ============================================================
struct PulseInterval {
    float         intervalMs;
    unsigned long timestamp;
};

// ============================================================
// Per-pour record for rate projection
// ============================================================
struct PourRecord {
    float         ounces;
    unsigned long timestamp;
};

// ============================================================
// Per-tap diagnostic snapshot
// ============================================================
struct TapDiagnostics {
    SensorHealth  health        = SensorHealth::NO_SIGNAL;
    float         pulseQuality  = 0.0f;   // 0-100%

    // Pulse stats
    float         avgIntervalMs  = 0.0f;
    float         stdDevMs       = 0.0f;
    uint32_t      noisePulses    = 0;
    uint32_t      totalPulses    = 0;
    unsigned long lastPulseMs    = 0;

    // Flow
    float         instantFlowOz  = 0.0f;
    float         avgFlowOz      = 0.0f;
    float         peakFlowOz     = 0.0f;
    unsigned long totalPouringMs = 0;

    // Projections
    float         avgPourSizeOz     = 0.0f;
    float         poursPerDay       = 0.0f;
    float         estimatedDaysLeft = 0.0f;

    // Calibration
    bool          calibrated  = false;
    float         pulsesPerOz = 450.0f;

    // Rolling window
    std::vector<PulseInterval> intervals;

    const char* healthString() const {
        switch (health) {
            case SensorHealth::GOOD:               return "Good";
            case SensorHealth::NOISY:              return "Noisy";
            case SensorHealth::STUCK:              return "Stuck!";
            case SensorHealth::NO_SIGNAL:          return "No Signal";
            case SensorHealth::CALIBRATION_NEEDED: return "Needs Cal";
        }
        return "Unknown";
    }
};

// ============================================================
// Keg size presets
// ============================================================
struct KegProfile {
    String name;
    float  sizeOz;
    String description;
};

// ============================================================
// DiagnosticsManager
// ============================================================
class DiagnosticsManager {
public:
    static DiagnosticsManager& getInstance();

    void begin();
    void update();

    // Event hooks
    void onPulse(int tapIndex, unsigned long intervalMs);
    void onNoisePulse(int tapIndex);
    void onPourComplete(int tapIndex, float ounces, float duration, float peakFlow);

    // Keg profiles
    void   setKegProfile(int tapIndex, int profileIndex);
    String getProfilesJSON() const;

    // Accessors
    const TapDiagnostics& getDiag(int tapIndex) const;
    String getDiagJSON(int tapIndex) const;
    String getAllDiagJSON() const;
    String getSensorReportJSON() const;

    bool   hasSensorWarning(int tapIndex) const;
    String getSensorWarningMessage(int tapIndex) const;

private:
    DiagnosticsManager();

    void  updateHealth(int tapIndex);
    void  updateProjections(int tapIndex);
    float calcStdDev(const std::vector<PulseInterval>& intervals, float mean) const;
    static bool validIndex(int i) { return i >= 0 && i < NUM_TAPS; }

    TapDiagnostics          m_diag[NUM_TAPS];
    std::vector<PourRecord> m_pourRecords[NUM_TAPS];
    std::vector<KegProfile> m_profiles;
    unsigned long           m_lastUpdate = 0;
};

#endif // DIAGNOSTICS_MANAGER_H
