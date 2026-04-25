#include "SDManager.h"
#include "config.h"
#include <LittleFS.h>

// ============================================================
// Singleton
// ============================================================
SDManager& SDManager::getInstance() {
    static SDManager instance;
    return instance;
}

// ============================================================
// Begin — mount LittleFS
// ============================================================
bool SDManager::begin() {
    LOG_I("SDManager: mounting LittleFS...");

    if (!LittleFS.begin(true)) {  // true = format on first boot
        LOG_E("SDManager: LittleFS mount failed");
        m_available = false;
        return false;
    }

    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    LOG_I("SDManager: LittleFS mounted — %u KB total, %u KB used",
          total / 1024, used / 1024);

    if (!LittleFS.exists(pourLogPath())) {
        writeHeader(pourLogPath(),
            "uptime,tap,tap_name,beer_name,"
            "ounces,duration_s,peak_flow_oz_s,"
            "keg_after_oz,keg_capacity_oz,pct_remaining");
        LOG_I("SDManager: created pours.csv");
    }

    if (!LittleFS.exists(kegLogPath())) {
        writeHeader(kegLogPath(),
            "uptime,tap,beer_name,level_oz,capacity_oz,pct_remaining");
        LOG_I("SDManager: created keg_levels.csv");
    }

    m_available = true;
    LOG_I("SDManager: ready");
    return true;
}

// ============================================================
// Log a pour
// ============================================================
void SDManager::logPour(int tapIndex, const String& tapName, const String& beerName,
                         float ounces, float duration, float peakFlow,
                         float kegLevelAfter, float kegCapacity) {
    if (!m_available) return;

    float pct = kegCapacity > 0 ? (kegLevelAfter / kegCapacity * 100.0f) : 0;

    String line = uptimeString()            + "," +
                  String(tapIndex + 1)      + "," +
                  tapName                   + "," +
                  beerName                  + "," +
                  String(ounces,    2)      + "," +
                  String(duration,  2)      + "," +
                  String(peakFlow,  3)      + "," +
                  String(kegLevelAfter, 1)  + "," +
                  String(kegCapacity,   1)  + "," +
                  String(pct,           1);

    if (appendLine(pourLogPath(), line)) {
        m_totalPours++;
        LOG_I("SDManager: logged pour tap %d %.2f oz", tapIndex + 1, ounces);
    } else {
        LOG_E("SDManager: write failed — flash full?");
    }
}

// ============================================================
// Log keg snapshot
// ============================================================
void SDManager::logKegSnapshot(int tapIndex, const String& beerName,
                                float level, float capacity) {
    if (!m_available) return;

    float pct = capacity > 0 ? (level / capacity * 100.0f) : 0;

    String line = uptimeString()       + "," +
                  String(tapIndex + 1) + "," +
                  beerName             + "," +
                  String(level,    1)  + "," +
                  String(capacity, 1)  + "," +
                  String(pct,      1);

    appendLine(kegLogPath(), line);
}

// ============================================================
// Read file contents
// ============================================================
String SDManager::readFile(const String& path) {
    if (!m_available) return "";

    File f = LittleFS.open(path, "r");
    if (!f) {
        LOG_W("SDManager: cannot open %s", path.c_str());
        return "";
    }

    String out;
    out.reserve(f.size());
    while (f.available()) out += (char)f.read();
    f.close();
    return out;
}

// ============================================================
// Delete a file
// ============================================================
bool SDManager::deleteFile(const String& path) {
    if (!m_available) return false;
    return LittleFS.remove(path);
}

// ============================================================
// List files as JSON
// ============================================================
String SDManager::listFilesJSON() {
    if (!m_available) return "{\"available\":false}";

    String json = "{\"available\":true,\"files\":[";
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    bool first = true;
    while (f) {
        if (!f.isDirectory()) {
            if (!first) json += ",";
            json += "{\"name\":\"" + String(f.name()) + "\","
                    "\"size\":"    + String(f.size())  + "}";
            first = false;
        }
        f = root.openNextFile();
    }
    json += "]}";
    return json;
}

// ============================================================
// Status JSON
// ============================================================
String SDManager::getStatusJSON() const {
    if (!m_available)
        return "{\"available\":false}";

    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    return "{\"available\":true,"
           "\"total_kb\":"     + String(total / 1024)  + ","
           "\"used_kb\":"      + String(used  / 1024)  + ","
           "\"pours_logged\":" + String(m_totalPours)  + "}";
}

// ============================================================
// Helpers
// ============================================================
bool SDManager::writeHeader(const String& path, const String& header) {
    File f = LittleFS.open(path, "w");
    if (!f) return false;
    f.println(header);
    f.close();
    return true;
}

bool SDManager::appendLine(const String& path, const String& line) {
    File f = LittleFS.open(path, "a");
    if (!f) return false;
    f.println(line);
    f.close();
    return true;
}

String SDManager::uptimeString() {
    unsigned long s = millis() / 1000;
    char buf[24];
    snprintf(buf, sizeof(buf), "%lud%02lu:%02lu:%02lu",
             s / 86400, (s % 86400) / 3600, (s % 3600) / 60, s % 60);
    return String(buf);
}
