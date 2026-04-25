#ifndef NOTIFICATION_MANAGER_H
#define NOTIFICATION_MANAGER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"

// ============================================================
// Notification channels
// ============================================================
enum class NotifyEvent : uint8_t {
    POUR_COMPLETE,
    LOW_KEG,
    KEG_EMPTY,
    KEG_RESET,
    SENSOR_ERROR,
    SYSTEM_BOOT,
};

struct NotificationConfig {
    // Slack incoming webhook
    String slackWebhookUrl;
    bool   slackEnabled = false;

    // Discord webhook
    String discordWebhookUrl;
    bool   discordEnabled = false;

    // Generic webhook (JSON POST)
    String genericWebhookUrl;
    bool   genericEnabled = false;

    // Per-event toggles (bitfield)
    bool notifyOnPour       = false; // noisy — off by default
    bool notifyOnLowKeg     = true;
    bool notifyOnEmpty      = true;
    bool notifyOnSensorErr  = true;
    bool notifyOnBoot       = false;

    // Minimum pour size to notify (oz) — 0 = all pours
    float minPourOzToNotify = 4.0f;
};

// ============================================================
// NotificationManager
// ============================================================
class NotificationManager {
public:
    static NotificationManager& getInstance();

    void begin();

    // Send notifications for events
    void notifyPour(int tapIndex, float ounces, float duration, float peakFlow, const String& beerName);
    void notifyLowKeg(int tapIndex, float levelOz, float capacityOz);
    void notifyKegEmpty(int tapIndex, const String& beerName);
    void notifyKegReset(int tapIndex, const String& beerName);
    void notifySensorError(int tapIndex, const String& message);
    void notifyBoot(const String& ip);

    // Config
    NotificationConfig& getConfig() { return m_config; }
    void saveConfig();
    void loadConfig();
    String getConfigJSON() const;
    bool updateFromJSON(const String& json);

    // Test
    bool testSlack();
    bool testDiscord();

private:
    NotificationManager() = default;

    bool postSlack(const String& message, const String& color = "#ff6b35");
    bool postDiscord(const String& message, const String& title = "Beer Flow Monitor");
    bool postGenericWebhook(const JsonDocument& payload);

    String buildPourMessage(int tapIndex, float ounces, float duration,
                            float peakFlow, const String& beerName);
    String buildLowKegMessage(int tapIndex, float levelOz, float capacityOz);

    NotificationConfig m_config;
};

#endif // NOTIFICATION_MANAGER_H
