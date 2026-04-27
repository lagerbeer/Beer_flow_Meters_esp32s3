#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================
// System Configuration
// ============================================================
#define FIRMWARE_VERSION    "2.0.0"
#define NUM_TAPS            6
#define KEG_SIZE_OZ         640.0f
#define HISTORY_SIZE        50
#define LOW_KEG_ALERT_OZ    100.0f

// ============================================================
// Timing (milliseconds)
// ============================================================
#define PULSE_DEBOUNCE_MS        50
#define POUR_TIMEOUT_MS          5000   // ms of silence before pour ends
#define POUR_MIN_PULSES          3      // pulses required before pour is declared (filters noise)
#define POUR_MIN_OZ              5.0f   // pours below this are discarded as ghost pours (pressure noise)
#define MQTT_PUBLISH_INTERVAL_MS 30000
#define LAST_POUR_PUBLISH_MS     60000
#define MQTT_RECONNECT_INTERVAL  5000
#define STATUS_PRINT_INTERVAL    30000
#define SENSOR_POLL_INTERVAL_MS  10

// ============================================================
// Network
// ============================================================
#define WEB_SERVER_PORT     80
#define OTA_PORT            3232

// ============================================================
// Pin Configuration — update these to match your wiring
// ============================================================
constexpr uint8_t TAP_PINS[NUM_TAPS] = {5, 1, 2, 15, 16, 17};

// ============================================================
// NVS Namespace Keys
// ============================================================
#define NVS_NS_TAPS         "taps"
#define NVS_NS_SETTINGS     "settings"
#define NVS_KEY_PULSES      "pulses_oz_%d"
#define NVS_KEY_KEG_LEVEL   "keg_level_%d"
#define NVS_KEY_TAP_NAME    "tap_name_%d"
#define NVS_KEY_BEER_NAME   "beer_name_%d"
#define NVS_KEY_WEBHOOK     "webhook"
#define NVS_KEY_ALERT_THRESH "alert_thresh"

// ============================================================
// MQTT Topics
// ============================================================
#define MQTT_TOPIC_PREFIX       "beermonitor"
#define MQTT_TOPIC_AVAILABILITY "beermonitor/availability"
#define MQTT_TOPIC_STATS        "beermonitor/stats"

// ============================================================
// Logging
// ============================================================
enum class LogLevel : uint8_t { DEBUG = 0, INFO, WARN, ERROR };

#define LOG_LEVEL LogLevel::DEBUG   // Set to INFO or higher for production

class Logger {
public:
    static void log(LogLevel level, const char* fmt, ...) {
        if (level < LOG_LEVEL) return;
        const char* prefix;
        switch (level) {
            case LogLevel::DEBUG: prefix = "[DBG] "; break;
            case LogLevel::INFO:  prefix = "[INF] "; break;
            case LogLevel::WARN:  prefix = "[WRN] "; break;
            case LogLevel::ERROR: prefix = "[ERR] "; break;
            default:              prefix = "      "; break;
        }
        Serial.print(prefix);
        char buf[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Serial.println(buf);
    }
};

#define LOG_D(...) Logger::log(LogLevel::DEBUG, __VA_ARGS__)
#define LOG_I(...) Logger::log(LogLevel::INFO,  __VA_ARGS__)
#define LOG_W(...) Logger::log(LogLevel::WARN,  __VA_ARGS__)
#define LOG_E(...) Logger::log(LogLevel::ERROR, __VA_ARGS__)

#endif // CONFIG_H
