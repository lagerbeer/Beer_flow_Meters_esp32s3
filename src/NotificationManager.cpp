#include "NotificationManager.h"
#include "TapManager.h"
#include <Preferences.h>
#include <WiFi.h>

NotificationManager& NotificationManager::getInstance() {
    static NotificationManager instance;
    return instance;
}

void NotificationManager::begin() {
    loadConfig();
    LOG_I("NotificationManager ready (Slack=%s Discord=%s Generic=%s)",
          m_config.slackEnabled   ? "on" : "off",
          m_config.discordEnabled ? "on" : "off",
          m_config.genericEnabled ? "on" : "off");
}

// ============================================================
// Public event methods
// ============================================================
void NotificationManager::notifyPour(int tapIndex, float ounces, float duration,
                                     float peakFlow, const String& beerName) {
    if (!m_config.notifyOnPour) return;
    if (ounces < m_config.minPourOzToNotify) return;

    String msg = buildPourMessage(tapIndex, ounces, duration, peakFlow, beerName);

    if (m_config.slackEnabled)   postSlack(msg, "#2196F3");
    if (m_config.discordEnabled) postDiscord(msg, "🍺 Pour Complete");

    if (m_config.genericEnabled) {
        DynamicJsonDocument doc(256);
        doc["event"]    = "pour";
        doc["tap"]      = tapIndex + 1;
        doc["beer"]     = beerName;
        doc["ounces"]   = ounces;
        doc["duration"] = duration;
        doc["peakFlow"] = peakFlow;
        postGenericWebhook(doc);
    }
}

bool NotificationManager::logPourToSheets(int tapIndex, float ounces, float duration,
                                           float peakFlow, const String& beerName) {
    if (!m_config.sheetsEnabled) return false;

    TapConfig cfg = TapManager::getInstance().getConfig(tapIndex);
    TapState  st  = TapManager::getInstance().getState(tapIndex);
    float pct = (cfg.kegSize > 0) ? (st.currentKegLevel / cfg.kegSize * 100.0f) : 0;

    DynamicJsonDocument doc(384);
    doc["tap"]         = tapIndex + 1;
    doc["tapName"]     = cfg.tapName;
    doc["beer"]        = beerName;
    doc["abv"]         = cfg.abv;
    doc["ibu"]         = cfg.ibu;
    doc["ounces"]      = ounces;
    doc["duration"]    = duration;
    doc["peakFlow"]    = peakFlow;
    doc["kegLevel"]    = st.currentKegLevel;
    doc["kegLevelGal"] = st.currentKegLevel / 128.0f;
    doc["kegCapacity"] = cfg.kegSize;
    doc["kegPct"]      = pct;
    doc["dateTime"]    = st.lastPour.dateTime;
    doc["timestamp"]   = st.lastPour.timestamp;

    return postSheets(doc);
}

void NotificationManager::notifyLowKeg(int tapIndex, float levelOz, float capacityOz) {
    if (!m_config.notifyOnLowKeg) return;

    TapConfig cfg = TapManager::getInstance().getConfig(tapIndex);
    float pct = (capacityOz > 0) ? (levelOz / capacityOz * 100.0f) : 0;
    String msg = buildLowKegMessage(tapIndex, levelOz, capacityOz);

    if (m_config.slackEnabled)   postSlack(msg, "#FF9800");
    if (m_config.discordEnabled) postDiscord(msg, "⚠️ Low Keg Alert");

    if (m_config.genericEnabled) {
        DynamicJsonDocument doc(256);
        doc["event"]    = "low_keg";
        doc["tap"]      = tapIndex + 1;
        doc["beer"]     = cfg.beerName;
        doc["levelOz"]  = levelOz;
        doc["pct"]      = pct;
        postGenericWebhook(doc);
    }
}

void NotificationManager::notifyKegEmpty(int tapIndex, const String& beerName) {
    if (!m_config.notifyOnEmpty) return;
    TapConfig cfg = TapManager::getInstance().getConfig(tapIndex);
    String msg = "🪣 *Keg Empty!* — Tap " + String(tapIndex + 1) + " (" + cfg.tapName + ") is out of *" + beerName + "*. Time to swap!";

    if (m_config.slackEnabled)   postSlack(msg, "#f44336");
    if (m_config.discordEnabled) postDiscord(msg, "🪣 Keg Empty");

    if (m_config.genericEnabled) {
        DynamicJsonDocument doc(128);
        doc["event"] = "keg_empty";
        doc["tap"]   = tapIndex + 1;
        doc["beer"]  = beerName;
        postGenericWebhook(doc);
    }
}

void NotificationManager::notifyKegReset(int tapIndex, const String& beerName) {
    TapConfig cfg = TapManager::getInstance().getConfig(tapIndex);
    String msg = "🔄 *Keg Reset* — Tap " + String(tapIndex + 1) + " (" + cfg.tapName + ") refilled with *" + beerName + "*";

    if (m_config.slackEnabled)   postSlack(msg, "#4CAF50");
    if (m_config.discordEnabled) postDiscord(msg, "🔄 Keg Reset");

    if (m_config.genericEnabled) {
        DynamicJsonDocument doc(128);
        doc["event"] = "keg_reset";
        doc["tap"]   = tapIndex + 1;
        doc["beer"]  = beerName;
        postGenericWebhook(doc);
    }
}

void NotificationManager::notifySensorError(int tapIndex, const String& message) {
    if (!m_config.notifyOnSensorErr) return;
    String msg = "🔧 *Sensor Warning* — Tap " + String(tapIndex + 1) + ": " + message;
    if (m_config.slackEnabled)   postSlack(msg, "#9C27B0");
    if (m_config.discordEnabled) postDiscord(msg, "🔧 Sensor Warning");
}

void NotificationManager::notifyBoot(const String& ip) {
    if (!m_config.notifyOnBoot) return;
    String msg = "🍺 *Beer Flow Monitor Online* — http://" + ip + " | v" + FIRMWARE_VERSION;
    if (m_config.slackEnabled)   postSlack(msg, "#4CAF50");
    if (m_config.discordEnabled) postDiscord(msg, "🍺 System Online");
}

// ============================================================
// Message builders
// ============================================================
String NotificationManager::buildPourMessage(int tapIndex, float ounces, float duration,
                                              float peakFlow, const String& beerName) {
    TapConfig cfg = TapManager::getInstance().getConfig(tapIndex);
    TapState  st  = TapManager::getInstance().getState(tapIndex);
    float pct = (cfg.kegSize > 0) ? (st.currentKegLevel / cfg.kegSize * 100.0f) : 0;

    char buf[256];
    snprintf(buf, sizeof(buf),
        "🍺 *Pour Complete* — Tap %d (%s)\n"
        ">Beer: *%s*\n"
        ">Amount: *%.2f oz*  |  Duration: %.1fs  |  Peak: %.1f oz/s\n"
        ">Keg remaining: %.1f oz (%.0f%%)",
        tapIndex + 1, cfg.tapName.c_str(),
        beerName.c_str(),
        ounces, duration, peakFlow,
        st.currentKegLevel, pct);
    return String(buf);
}

String NotificationManager::buildLowKegMessage(int tapIndex, float levelOz, float capacityOz) {
    TapConfig cfg = TapManager::getInstance().getConfig(tapIndex);
    float pct = (capacityOz > 0) ? (levelOz / capacityOz * 100.0f) : 0;
    char buf[200];
    snprintf(buf, sizeof(buf),
        "⚠️ *Low Keg Alert* — Tap %d (%s)\n"
        ">Beer: *%s*\n"
        ">Remaining: *%.1f oz* (%.0f%% of keg)\n"
        ">Time to order more!",
        tapIndex + 1, cfg.tapName.c_str(),
        cfg.beerName.c_str(), levelOz, pct);
    return String(buf);
}

// ============================================================
// HTTP senders
// ============================================================
bool NotificationManager::postSlack(const String& message, const String& color) {
    if (m_config.slackWebhookUrl.isEmpty()) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    DynamicJsonDocument doc(512);
    JsonArray attachments = doc.createNestedArray("attachments");
    JsonObject att = attachments.createNestedObject();
    att["text"]      = message;
    att["color"]     = color;
    att["mrkdwn_in"] = serialized("[\"text\"]");
    att["footer"]    = "Beer Flow Monitor v" + String(FIRMWARE_VERSION);

    String body; serializeJson(doc, body);

    HTTPClient http;
    http.begin(m_config.slackWebhookUrl);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    http.end();

    if (code == 200) { LOG_D("Slack OK"); return true; }
    LOG_W("Slack failed: %d", code);
    return false;
}

bool NotificationManager::postDiscord(const String& message, const String& title) {
    if (m_config.discordWebhookUrl.isEmpty()) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    DynamicJsonDocument doc(512);
    JsonArray embeds = doc.createNestedArray("embeds");
    JsonObject embed = embeds.createNestedObject();
    embed["title"]       = title;
    embed["description"] = message;
    embed["color"]       = 16737843; // orange #FF6B35 as decimal

    String body; serializeJson(doc, body);

    HTTPClient http;
    http.begin(m_config.discordWebhookUrl);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    http.end();

    if (code == 204 || code == 200) { LOG_D("Discord OK"); return true; }
    LOG_W("Discord failed: %d", code);
    return false;
}

bool NotificationManager::postGenericWebhook(const JsonDocument& payload) {
    if (m_config.genericWebhookUrl.isEmpty()) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    String body; serializeJson(payload, body);

    HTTPClient http;
    http.begin(m_config.genericWebhookUrl);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    http.end();

    if (code > 0 && code < 300) { LOG_D("Webhook OK: %d", code); return true; }
    LOG_W("Webhook failed: %d", code);
    return false;
}

bool NotificationManager::postSheets(const JsonDocument& payload) {
    if (m_config.sheetsWebAppUrl.isEmpty()) {
        m_lastSheetsStatus = "No Web App URL configured";
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        m_lastSheetsStatus = "WiFi not connected";
        return false;
    }

    String body; serializeJson(payload, body);

    HTTPClient http;
    http.begin(m_config.sheetsWebAppUrl);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    String resp = (code > 0) ? http.getString() : http.errorToString(code);
    http.end();

    // Apps Script Web Apps run doPost() to completion *before* replying with
    // a 302 redirect to the actual response body — the row is already
    // appended even if we don't follow that redirect, so treat 3xx as success too.
    if (code > 0 && code < 400) {
        m_lastSheetsStatus = "HTTP " + String(code);
        LOG_D("Sheets log OK: %d", code);
        return true;
    }
    m_lastSheetsStatus = "HTTP " + String(code) + ": " + resp;
    LOG_W("Sheets log failed: %d (%s)", code, resp.c_str());
    return false;
}

// ============================================================
// Test helpers
// ============================================================
bool NotificationManager::testSlack() {
    return postSlack("🧪 *Test message* from Beer Flow Monitor. Slack is connected!", "#2196F3");
}

bool NotificationManager::testDiscord() {
    return postDiscord("🧪 Test message from Beer Flow Monitor. Discord is connected!", "🧪 Test");
}

bool NotificationManager::testSheets() {
    DynamicJsonDocument doc(256);
    doc["tap"]         = 0;
    doc["tapName"]     = "Test";
    doc["beer"]        = "Test Pour";
    doc["abv"]         = 0;
    doc["ibu"]         = 0;
    doc["ounces"]      = 0;
    doc["duration"]    = 0;
    doc["peakFlow"]    = 0;
    doc["kegLevel"]    = 0;
    doc["kegLevelGal"] = 0;
    doc["kegCapacity"] = 0;
    doc["kegPct"]      = 0;
    doc["dateTime"]    = "TEST";
    doc["timestamp"]   = 0;
    doc["test"]        = true;
    return postSheets(doc);
}

// ============================================================
// Config persistence
// ============================================================
void NotificationManager::saveConfig() {
    Preferences prefs;
    prefs.begin("notify", false);
    prefs.putString("slack_url",    m_config.slackWebhookUrl);
    prefs.putBool("slack_en",       m_config.slackEnabled);
    prefs.putString("discord_url",  m_config.discordWebhookUrl);
    prefs.putBool("discord_en",     m_config.discordEnabled);
    prefs.putString("generic_url",  m_config.genericWebhookUrl);
    prefs.putBool("generic_en",     m_config.genericEnabled);
    prefs.putString("sheets_url",   m_config.sheetsWebAppUrl);
    prefs.putBool("sheets_en",      m_config.sheetsEnabled);
    prefs.putBool("on_pour",        m_config.notifyOnPour);
    prefs.putBool("on_low",         m_config.notifyOnLowKeg);
    prefs.putBool("on_empty",       m_config.notifyOnEmpty);
    prefs.putBool("on_sensor",      m_config.notifyOnSensorErr);
    prefs.putBool("on_boot",        m_config.notifyOnBoot);
    prefs.putFloat("min_pour",      m_config.minPourOzToNotify);
    prefs.end();
}

void NotificationManager::loadConfig() {
    Preferences prefs;
    if (!prefs.begin("notify", true)) return;
    m_config.slackWebhookUrl    = prefs.getString("slack_url",   "");
    m_config.slackEnabled       = prefs.getBool("slack_en",      false);
    m_config.discordWebhookUrl  = prefs.getString("discord_url", "");
    m_config.discordEnabled     = prefs.getBool("discord_en",    false);
    m_config.genericWebhookUrl  = prefs.getString("generic_url", "");
    m_config.genericEnabled     = prefs.getBool("generic_en",    false);
    m_config.sheetsWebAppUrl    = prefs.getString("sheets_url",  "");
    m_config.sheetsEnabled      = prefs.getBool("sheets_en",     false);
    m_config.notifyOnPour       = prefs.getBool("on_pour",       false);
    m_config.notifyOnLowKeg     = prefs.getBool("on_low",        true);
    m_config.notifyOnEmpty      = prefs.getBool("on_empty",      true);
    m_config.notifyOnSensorErr  = prefs.getBool("on_sensor",     true);
    m_config.notifyOnBoot       = prefs.getBool("on_boot",       false);
    m_config.minPourOzToNotify  = prefs.getFloat("min_pour",     4.0f);
    prefs.end();
}

String NotificationManager::getConfigJSON() const {
    DynamicJsonDocument doc(768);
    doc["slackUrl"]         = m_config.slackWebhookUrl;
    doc["slackEnabled"]     = m_config.slackEnabled;
    doc["discordUrl"]       = m_config.discordWebhookUrl;
    doc["discordEnabled"]   = m_config.discordEnabled;
    doc["genericUrl"]       = m_config.genericWebhookUrl;
    doc["genericEnabled"]   = m_config.genericEnabled;
    doc["sheetsUrl"]        = m_config.sheetsWebAppUrl;
    doc["sheetsEnabled"]    = m_config.sheetsEnabled;
    doc["onPour"]           = m_config.notifyOnPour;
    doc["onLowKeg"]         = m_config.notifyOnLowKeg;
    doc["onEmpty"]          = m_config.notifyOnEmpty;
    doc["onSensorErr"]      = m_config.notifyOnSensorErr;
    doc["onBoot"]           = m_config.notifyOnBoot;
    doc["minPourOz"]        = m_config.minPourOzToNotify;
    String out; serializeJson(doc, out);
    return out;
}

bool NotificationManager::updateFromJSON(const String& json) {
    DynamicJsonDocument doc(768);
    if (deserializeJson(doc, json)) return false;
    if (doc.containsKey("slackUrl"))       m_config.slackWebhookUrl   = doc["slackUrl"].as<String>();
    if (doc.containsKey("slackEnabled"))   m_config.slackEnabled      = doc["slackEnabled"];
    if (doc.containsKey("discordUrl"))     m_config.discordWebhookUrl = doc["discordUrl"].as<String>();
    if (doc.containsKey("discordEnabled")) m_config.discordEnabled    = doc["discordEnabled"];
    if (doc.containsKey("genericUrl"))     m_config.genericWebhookUrl = doc["genericUrl"].as<String>();
    if (doc.containsKey("genericEnabled")) m_config.genericEnabled    = doc["genericEnabled"];
    if (doc.containsKey("sheetsUrl"))      m_config.sheetsWebAppUrl   = doc["sheetsUrl"].as<String>();
    if (doc.containsKey("sheetsEnabled"))  m_config.sheetsEnabled     = doc["sheetsEnabled"];
    if (doc.containsKey("onPour"))         m_config.notifyOnPour      = doc["onPour"];
    if (doc.containsKey("onLowKeg"))       m_config.notifyOnLowKeg    = doc["onLowKeg"];
    if (doc.containsKey("onEmpty"))        m_config.notifyOnEmpty     = doc["onEmpty"];
    if (doc.containsKey("onSensorErr"))    m_config.notifyOnSensorErr = doc["onSensorErr"];
    if (doc.containsKey("onBoot"))         m_config.notifyOnBoot      = doc["onBoot"];
    if (doc.containsKey("minPourOz"))      m_config.minPourOzToNotify = doc["minPourOz"];
    saveConfig();
    return true;
}
