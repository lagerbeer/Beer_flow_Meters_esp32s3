#ifndef TAP_MANAGER_H
#define TAP_MANAGER_H

#include <Arduino.h>
#include <vector>
#include "config.h"

// ============================================================
// Data structures
// ============================================================

struct PourEvent {
    unsigned long timestamp;    // millis() at time of pour
    float         ounces;
    float         duration;     // seconds
    float         peakFlowRate; // oz/sec
    float         kegLevelAfter;
    String        beerName;
    String        dateTime;     // formatted uptime string
};

struct TapConfig {
    float  pulsesPerOz  = 450.0f;
    float  kegSize      = KEG_SIZE_OZ;
    String tapName      = "Tap";
    String beerName     = "Unknown";
    float  abv          = 0.0f;       // Alcohol by Volume (%)
    int    ibu          = 0;          // International Bitterness Units
    String beerColor    = "#c8820a";  // Beer color hex for display
};

struct TapState {
    // Keg accounting
    float currentKegLevel = KEG_SIZE_OZ;

    // Live pour tracking
    volatile unsigned long pulseCount    = 0;
    unsigned long          lastPulseTime = 0;
    uint8_t                pendingPulses = 0;  // noise gate: pulses before pour declared
    bool                   isPouring     = false;
    float                  currentPourOz = 0.0f;
    unsigned long          pourStartTime = 0;
    float                  peakFlowRate  = 0.0f;
    unsigned long          lastFlowCalc  = 0;

    // Calibration
    bool                   calibrating      = false;
    volatile unsigned long calibPulses      = 0;

    // History
    PourEvent              lastPour;
    bool                   hasLastPour = false;
    std::vector<PourEvent> history;

    // Alert state
    bool  lowKegAlertSent = false;
    float lastAlertLevel  = 0.0f;

    // Debug
    unsigned long totalPulsesDetected = 0;
};



// ============================================================
// TapManager — owns all tap state and business logic
// ============================================================
class TapManager {
public:
    // Singleton accessor
    static TapManager& getInstance();

    // Lifecycle
    void begin();
    void update();

    // Tap operations
    void resetKeg(int tapIndex);
    void setBeerName(int tapIndex, const String& name);
    void setTapInfo(int tapIndex, const String& name, float abv, int ibu, const String& color);
    void setTapName(int tapIndex, const String& name);
    void setKegSize(int tapIndex, float sizeOz);
    bool startCalibration(int tapIndex);
    bool endCalibration(int tapIndex, float knownOunces);

    // Data access (read-only copies to avoid race conditions)
    TapConfig   getConfig(int tapIndex) const;
    TapState    getState(int tapIndex) const;
    int         getActiveTap() const { return m_activeTap; }
    float       getCurrentPour(int tapIndex) const;
    PourEvent   getLastPour(int tapIndex) const;

    // JSON helpers — used by the web server
    String getTapStatusJSON() const;
    String getLastPourJSON(int tapIndex) const;
    String getPourHistoryJSON(int tapIndex) const;
    String getAllHistoryJSON() const;

    // Aggregate statistics
    unsigned long getTotalPours()  const { return m_totalPours; }
    float         getTotalOunces() const { return m_totalOunces; }

    // Alert helpers
    bool checkLowKegAlert(int tapIndex);
    void clearAlert(int tapIndex);

    // Config persistence
    void saveConfig();
    void loadConfig();

    // Debug
    void printPinStatus() const;

    // ISR entry point — must be public so it can be registered with attachInterrupt
    static void IRAM_ATTR isrDispatch(void* arg);

private:
    TapManager() = default;
    TapManager(const TapManager&) = delete;
    TapManager& operator=(const TapManager&) = delete;

    // Per-tap ISR (called from isrDispatch)
    void onPulse(int tapIndex);

    // Update helpers
    void checkPourTimeout();
    void updateLivePour();
    float calcInstantFlowRate(int tapIndex);
    void  finalisePour(int tapIndex);

    // Utilities
    static bool validIndex(int i) { return i >= 0 && i < NUM_TAPS; }
    static String formatUptime();
    static String buildJSON(const PourEvent& p, int tapIndex = -1);

    // State
    TapConfig m_config[NUM_TAPS];
    TapState  m_state[NUM_TAPS];
    int       m_activeTap   = -1;

    // Aggregate counters
    unsigned long m_totalPours   = 0;
    float         m_totalOunces  = 0.0f;

    // Timing
    unsigned long m_lastUpdateMs = 0;
    unsigned long m_lastPollMs   = 0;
};

#endif // TAP_MANAGER_H
