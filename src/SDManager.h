#pragma once
#include <Arduino.h>

// ============================================================
// SDManager — CSV logging to LittleFS (internal flash)
// No external hardware needed. Files downloadable via web UI.
// Storage: ~1.5MB available with default_8MB.csv partition
// ============================================================

class SDManager {
public:
    static SDManager& getInstance();

    bool begin();

    void logPour(int tapIndex, const String& tapName, const String& beerName,
                 float ounces, float duration, float peakFlow,
                 float kegLevelAfter, float kegCapacity);

    void logKegSnapshot(int tapIndex, const String& beerName,
                        float level, float capacity);

    String readFile(const String& path);
    String listFilesJSON();
    bool   deleteFile(const String& path);

    bool   isAvailable() const { return m_available; }
    String getStatusJSON() const;

private:
    SDManager() = default;

    bool   writeHeader(const String& path, const String& header);
    bool   appendLine(const String& path, const String& line);
    String uptimeString();
    String pourLogPath()  { return "/pours.csv"; }
    String kegLogPath()   { return "/keg_levels.csv"; }

    bool     m_available  = false;
    uint32_t m_totalPours = 0;
};
