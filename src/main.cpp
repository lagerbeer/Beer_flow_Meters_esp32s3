// ============================================================
// Beer Flow Monitor — main.cpp
// Firmware v2.1.0
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <nvs_flash.h>
#include <ArduinoJson.h>

#include "config.h"
#include "credentials.h"   // <-- add to .gitignore
#include "TapManager.h"
#include "DisplayManager.h"
#include "DiagnosticsManager.h"
#include "NotificationManager.h"
#include "SDManager.h"

// ============================================================
// Network objects
// ============================================================
WiFiServer   g_server(WEB_SERVER_PORT);
WiFiClient   g_espClient;
PubSubClient g_mqtt(g_espClient);
Preferences  g_prefs;

// ============================================================
// Timing state
// ============================================================
static unsigned long s_lastMqttReconnect  = 0;
static unsigned long s_lastMqttPublish    = 0;
static unsigned long s_lastPourPublish    = 0;
static unsigned long s_lastStatusPrint    = 0;
static int           s_prevActiveTap      = -1;

// ============================================================
// Forward declarations
// ============================================================
void setupWiFi();
void setupOTA();
void setupMQTT();
void setupAllManagers();

bool        mqttConnect();
void        mqttCallback(char* topic, byte* payload, unsigned int length);
void        mqttPublishTapData();
void        mqttPublishPourEvent(int tapIndex);
void        mqttPublishAllLastPours();
void        mqttPublishHADiscovery();

void        handleWebClient(WiFiClient& client);
static void sendJsonResponse(WiFiClient& client, const String& body, int code = 200);
static void sendOK(WiFiClient& client);

void        handleSerialCommands();
void        sendWebhookAlert(int tapIndex);
void        onPourComplete_hooks(int tapIndex);
void        onKegReset_hooks(int tapIndex);
static String urlDecode(const String& input);
static String bodyParam(const String& body, const String& key);

// ============================================================
// HTML UI (served from PROGMEM)
// ============================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Beer Flow Monitor</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <script src="https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js"></script>
    <style>
        *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

        :root {
            --amber:  #ff6b35;
            --amber2: #e85a25;
            --bg:     #111827;
            --surface:#1f2937;
            --surface2:#374151;
            --text:   #f3f4f6;
            --muted:  #9ca3af;
            --green:  #10b981;
            --yellow: #f59e0b;
            --red:    #ef4444;
            --blue:   #3b82f6;
            --radius: 12px;
        }

        body { font-family: 'Segoe UI', system-ui, sans-serif; background: var(--bg); color: var(--text); padding: 20px; }
        .container { max-width: 1400px; margin: 0 auto; }

        /* Header */
        header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; }
        header h1 { font-size: 1.8rem; color: var(--amber); letter-spacing: -0.5px; }
        .pills { display: flex; gap: 10px; align-items: center; }
        .pill { padding: 6px 14px; border-radius: 20px; font-size: 0.8rem; font-weight: 600; }
        .pill-green  { background: rgba(16,185,129,.2); color: var(--green); border: 1px solid var(--green); }
        .pill-red    { background: rgba(239,68,68,.2);  color: var(--red);   border: 1px solid var(--red); }
        .pill-blue   { background: rgba(59,130,246,.2); color: var(--blue);  border: 1px solid var(--blue); }

        /* Alert banner */
        #alertBanner { display: none; background: var(--red); color: #fff; padding: 14px 20px; border-radius: var(--radius); margin-bottom: 20px; justify-content: space-between; align-items: center; }
        #alertBanner.show { display: flex; }
        #alertBanner button { background: rgba(255,255,255,.2); border: none; color: #fff; padding: 6px 14px; border-radius: 8px; cursor: pointer; font-weight: 600; }

        /* Tabs */
        nav.tabs { display: flex; gap: 4px; background: var(--surface); padding: 6px; border-radius: var(--radius); margin-bottom: 24px; }
        nav.tabs button { flex: 1; padding: 10px; background: none; border: none; color: var(--muted); border-radius: 8px; cursor: pointer; font-size: 0.95rem; font-weight: 600; transition: all .2s; }
        nav.tabs button:hover { color: var(--text); background: var(--surface2); }
        nav.tabs button.active { color: var(--amber); background: var(--surface2); }

        section.tab { display: none; }
        section.tab.active { display: block; }

        /* Tap grid */
        .tap-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(340px, 1fr)); gap: 20px; }

        .tap-card { background: var(--surface); border-radius: var(--radius); padding: 20px; transition: transform .2s, box-shadow .2s; border: 2px solid transparent; }
        .tap-card:hover { transform: translateY(-3px); box-shadow: 0 8px 30px rgba(0,0,0,.4); }
        .tap-card.pouring { border-color: var(--amber); animation: glow 1.5s ease-in-out infinite; }
        .tap-card.low { border-color: var(--red); }
        .tap-card.calibrating { border-color: var(--yellow); }
        @keyframes glow { 0%,100%{ box-shadow: 0 0 0 rgba(255,107,53,0); } 50%{ box-shadow: 0 0 16px rgba(255,107,53,.4); } }

        .card-header { display: flex; justify-content: space-between; align-items: flex-start; margin-bottom: 14px; }
        .tap-name { font-size: 1.2rem; font-weight: 700; color: var(--amber); }
        .tap-badges { display: flex; gap: 6px; flex-wrap: wrap; }
        .badge { padding: 3px 8px; border-radius: 6px; font-size: 0.72rem; font-weight: 700; text-transform: uppercase; }
        .badge-gray  { background: var(--surface2); color: var(--muted); }
        .badge-green { background: rgba(16,185,129,.15); color: var(--green); }
        .badge-amber { background: rgba(255,107,53,.15); color: var(--amber); }
        .badge-yellow{ background: rgba(245,158,11,.15); color: var(--yellow); }
        .badge-red   { background: rgba(239,68,68,.15);  color: var(--red); }

        /* Beer name edit */
        .beer-row { display: flex; gap: 8px; margin-bottom: 16px; }
        .beer-row input { flex: 1; background: var(--surface2); border: 1px solid #4b5563; color: var(--text); border-radius: 8px; padding: 8px 12px; font-size: 0.9rem; }
        .beer-row input:focus { outline: none; border-color: var(--amber); }
        .beer-row button { background: var(--green); color: #fff; border: none; border-radius: 8px; padding: 8px 14px; cursor: pointer; font-weight: 600; font-size: 0.85rem; }

        /* Keg bar */
        .keg-wrap { margin-bottom: 16px; }
        .keg-label { display: flex; justify-content: space-between; font-size: 0.8rem; color: var(--muted); margin-bottom: 6px; }
        .keg-track { height: 14px; background: var(--surface2); border-radius: 7px; overflow: hidden; }
        .keg-fill { height: 100%; border-radius: 7px; transition: width .4s ease; }
        .keg-fill.ok   { background: linear-gradient(90deg, #10b981, #34d399); }
        .keg-fill.warn { background: linear-gradient(90deg, #f59e0b, #fbbf24); }
        .keg-fill.crit { background: linear-gradient(90deg, #ef4444, #f87171); }

        /* Mini stats */
        .stats-row { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 14px; }
        .stat { background: var(--surface2); border-radius: 8px; padding: 10px 12px; text-align: center; }
        .stat-val { font-size: 1.15rem; font-weight: 700; color: var(--amber); }
        .stat-lbl { font-size: 0.72rem; color: var(--muted); margin-top: 2px; }

        /* Pouring indicator */
        .pouring-live { background: rgba(255,107,53,.15); border: 1px solid var(--amber); border-radius: 8px; padding: 10px; text-align: center; font-weight: 700; color: var(--amber); margin-bottom: 12px; font-size: 1.05rem; }

        /* Last pour */
        .last-pour { background: var(--surface2); border-radius: 8px; padding: 12px; margin-bottom: 14px; border-left: 3px solid var(--amber); }
        .last-pour-title { font-size: 0.75rem; text-transform: uppercase; color: var(--amber); font-weight: 700; margin-bottom: 8px; }
        .last-pour-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
        .lp-item span:first-child { display: block; font-size: 0.72rem; color: var(--muted); }
        .lp-item span:last-child  { display: block; font-weight: 700; font-size: 0.95rem; }
        .last-pour-time { font-size: 0.75rem; color: var(--muted); margin-top: 8px; text-align: right; }

        /* Buttons */
        .btn-row { display: flex; gap: 8px; }
        .btn { flex: 1; padding: 9px; border: none; border-radius: 8px; cursor: pointer; font-weight: 700; font-size: 0.85rem; transition: opacity .2s, transform .1s; }
        .btn:hover { opacity: .85; }
        .btn:active { transform: scale(.97); }
        .btn-amber  { background: var(--amber);  color: #fff; }
        .btn-red    { background: var(--red);    color: #fff; }
        .btn-yellow { background: var(--yellow); color: #111; }
        .btn-blue   { background: var(--blue);   color: #fff; }

        /* History table */
        .history-bar { display: flex; gap: 10px; margin-bottom: 20px; }
        .history-bar select { flex: 1; background: var(--surface); border: 1px solid #4b5563; color: var(--text); border-radius: 8px; padding: 10px; }
        .table-wrap { overflow-x: auto; border-radius: var(--radius); }
        table { width: 100%; border-collapse: collapse; }
        thead th { background: var(--amber); color: #fff; padding: 12px 14px; text-align: left; font-size: 0.85rem; }
        tbody td { padding: 10px 14px; border-bottom: 1px solid var(--surface2); font-size: 0.9rem; }
        tbody tr:hover td { background: var(--surface2); }

        /* Charts */
        .chart-box { background: var(--surface); border-radius: var(--radius); padding: 20px; margin-bottom: 20px; }
        .chart-box h3 { color: var(--amber); margin-bottom: 16px; font-size: 1rem; }
        canvas { max-height: 320px; }

        /* Stats page */
        .kpi-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr)); gap: 16px; margin-bottom: 24px; }
        .kpi { background: var(--surface); border-radius: var(--radius); padding: 20px; text-align: center; }
        .kpi-val { font-size: 2rem; font-weight: 800; color: var(--amber); }
        .kpi-lbl { font-size: 0.8rem; color: var(--muted); margin-top: 4px; }

        /* Alerts page */
        .settings-card { background: var(--surface); border-radius: var(--radius); padding: 24px; margin-bottom: 20px; }
        .settings-card h3 { color: var(--amber); margin-bottom: 16px; }
        .form-row { display: flex; gap: 10px; align-items: center; margin-bottom: 14px; }
        .form-row label { color: var(--muted); font-size: 0.9rem; width: 180px; flex-shrink: 0; }
        .form-row input { flex: 1; background: var(--surface2); border: 1px solid #4b5563; color: var(--text); border-radius: 8px; padding: 9px 12px; }
        .form-row input:focus { outline: none; border-color: var(--amber); }

        /* Notification toast */
        #toast { position: fixed; bottom: 24px; right: 24px; background: var(--green); color: #fff; padding: 12px 20px; border-radius: 10px; font-weight: 600; opacity: 0; transition: opacity .3s; pointer-events: none; z-index: 999; }
        #toast.show { opacity: 1; }

        footer { text-align: center; color: var(--muted); font-size: 0.8rem; margin-top: 30px; }
    </style>
</head>
<body>
<div id="alertBanner">
    <span id="alertMsg">⚠️ Low Keg Alert!</span>
    <button onclick="dismissAlert()">Dismiss</button>
</div>

<div class="container">
    <header>
        <h1>🍺 Beer Flow Monitor</h1>
        <div class="pills">
            <span id="mqttPill" class="pill pill-red">MQTT ✕</span>
            <span id="otaPill" class="pill pill-blue">OTA Ready</span>
        </div>
    </header>

    <nav class="tabs">
        <button class="active" onclick="switchTab('taps',        this)">🍺 Taps</button>
        <button               onclick="switchTab('history',     this)">📊 History</button>
        <button               onclick="switchTab('stats',       this)">📈 Stats</button>
        <button               onclick="switchTab('diagnostics', this)">🔬 Diagnostics</button>
        <button               onclick="switchTab('integrations',this)">🔔 Integrations</button>
        <button               onclick="switchTab('alerts',      this)">⚙️ Settings</button>
        <button               onclick="switchTab('logs',        this)">📁 Logs</button>
    </nav>

    <!-- Taps -->
    <section class="tab active" id="tab-taps">
        <div class="tap-grid" id="tapGrid"></div>
    </section>

    <!-- History -->
    <section class="tab" id="tab-history">
        <div class="history-bar">
            <select id="histSel" onchange="loadHistory()">
                <option value="all">All Taps</option>
                <option value="0">Tap 1</option><option value="1">Tap 2</option>
                <option value="2">Tap 3</option><option value="3">Tap 4</option>
                <option value="4">Tap 5</option><option value="5">Tap 6</option>
            </select>
            <button class="btn btn-blue" style="flex:unset;padding:10px 18px" onclick="loadHistory()">↻ Refresh</button>
        </div>
        <div class="chart-box"><h3>Pour History</h3><canvas id="histChart"></canvas></div>
        <div class="table-wrap">
            <table>
                <thead><tr><th>Time</th><th>Tap</th><th>Beer</th><th>oz</th><th>Keg After</th><th>Duration</th><th>Peak oz/s</th></tr></thead>
                <tbody id="histBody"></tbody>
            </table>
        </div>
    </section>

    <!-- Stats -->
    <section class="tab" id="tab-stats">
        <div class="kpi-grid">
            <div class="kpi"><div class="kpi-val" id="kTotalPours">—</div><div class="kpi-lbl">Total Pours</div></div>
            <div class="kpi"><div class="kpi-val" id="kTotalOz">—</div><div class="kpi-lbl">Total Ounces</div></div>
            <div class="kpi"><div class="kpi-val" id="kActiveTap">—</div><div class="kpi-lbl">Active Tap</div></div>
            <div class="kpi"><div class="kpi-val" id="kUptime">—</div><div class="kpi-lbl">Uptime</div></div>
        </div>
        <div class="chart-box"><h3>Keg Levels</h3><canvas id="kegChart"></canvas></div>
    </section>

    <!-- Diagnostics -->
    <section class="tab" id="tab-diagnostics">
        <div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:16px" id="diagGrid">
            <p style="color:var(--muted)">Loading diagnostics...</p>
        </div>
    </section>

    <!-- Integrations -->
    <section class="tab" id="tab-integrations">
        <div class="settings-card">
            <h3>💬 Slack</h3>
            <div class="form-row"><label>Webhook URL</label><input type="text" id="inpSlackUrl" placeholder="https://hooks.slack.com/services/..."></div>
            <div style="display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid var(--surface2)">
                <span style="font-size:.88rem">Enable Slack Notifications</span>
                <input type="checkbox" id="togSlack" style="width:18px;height:18px;accent-color:var(--amber)">
            </div>
            <div style="display:flex;gap:8px;margin-top:14px">
                <button class="btn btn-blue" onclick="testSlack()">Test</button>
                <button class="btn btn-amber" onclick="saveIntegrations()">Save</button>
            </div>
        </div>
        <div class="settings-card">
            <h3>🎮 Discord</h3>
            <div class="form-row"><label>Webhook URL</label><input type="text" id="inpDiscordUrl" placeholder="https://discord.com/api/webhooks/..."></div>
            <div style="display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid var(--surface2)">
                <span style="font-size:.88rem">Enable Discord Notifications</span>
                <input type="checkbox" id="togDiscord" style="width:18px;height:18px;accent-color:var(--amber)">
            </div>
            <div style="display:flex;gap:8px;margin-top:14px">
                <button class="btn btn-blue" onclick="testDiscord()">Test</button>
                <button class="btn btn-amber" onclick="saveIntegrations()">Save</button>
            </div>
        </div>
        <div class="settings-card">
            <h3>🔗 Generic Webhook</h3>
            <div class="form-row"><label>POST URL</label><input type="text" id="inpGenericUrl" placeholder="https://maker.ifttt.com/trigger/..."></div>
            <div style="display:flex;justify-content:space-between;align-items:center;padding:10px 0">
                <span style="font-size:.88rem">Enable Webhook</span>
                <input type="checkbox" id="togGeneric" style="width:18px;height:18px;accent-color:var(--amber)">
            </div>
            <button class="btn btn-amber" style="width:100%;margin-top:8px" onclick="saveIntegrations()">Save</button>
        </div>
        <div class="settings-card">
            <h3>🔔 Notification Events</h3>
            <div style="display:flex;flex-direction:column;gap:0">
                <div style="display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid var(--surface2)"><span style="font-size:.88rem">Pour Complete</span><input type="checkbox" id="togOnPour" style="width:18px;height:18px;accent-color:var(--amber)"></div>
                <div style="display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid var(--surface2)"><span style="font-size:.88rem">Low Keg</span><input type="checkbox" id="togOnLow" checked style="width:18px;height:18px;accent-color:var(--amber)"></div>
                <div style="display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid var(--surface2)"><span style="font-size:.88rem">Keg Empty</span><input type="checkbox" id="togOnEmpty" checked style="width:18px;height:18px;accent-color:var(--amber)"></div>
                <div style="display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid var(--surface2)"><span style="font-size:.88rem">Sensor Errors</span><input type="checkbox" id="togOnSensor" checked style="width:18px;height:18px;accent-color:var(--amber)"></div>
                <div style="display:flex;justify-content:space-between;align-items:center;padding:10px 0"><span style="font-size:.88rem">System Boot</span><input type="checkbox" id="togOnBoot" style="width:18px;height:18px;accent-color:var(--amber)"></div>
            </div>
            <button class="btn btn-amber" style="width:100%;margin-top:14px" onclick="saveNotifyEvents()">Save Events</button>
        </div>
    </section>

    <!-- Alerts / Settings -->
    <section class="tab" id="tab-alerts">
        <div class="settings-card">
            <h3>⚠️ Alert Settings</h3>
            <div class="form-row">
                <label>Low Keg Threshold (oz)</label>
                <input type="number" id="inpThreshold" value="100">
                <button class="btn btn-amber" style="flex:unset;padding:9px 16px" onclick="saveThreshold()">Save</button>
            </div>
            <div class="form-row">
                <label>Legacy Webhook URL</label>
                <input type="text" id="inpWebhook" placeholder="https://maker.ifttt.com/trigger/…">
                <button class="btn btn-amber" style="flex:unset;padding:9px 16px" onclick="saveWebhook()">Save</button>
            </div>
            <p style="color:var(--muted);font-size:.85rem;margin-top:8px">Webhook receives a POST with JSON on low-keg events. Use Integrations tab for Slack/Discord.</p>
        </div>
        <div class="settings-card">
            <h3>🪣 Keg Profiles</h3>
            <div class="form-row">
                <label>Apply to Tap</label>
                <select id="profileTapSel" style="flex:1;background:var(--surface2);border:1px solid #4b5563;color:var(--text);border-radius:8px;padding:9px 12px">
                    <option value="0">Tap 1</option><option value="1">Tap 2</option>
                    <option value="2">Tap 3</option><option value="3">Tap 4</option>
                    <option value="4">Tap 5</option><option value="5">Tap 6</option>
                </select>
            </div>
            <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:4px" id="profileGrid">
                <p style="color:var(--muted);font-size:.85rem">Loading profiles...</p>
            </div>
            <button class="btn btn-amber" style="width:100%;margin-top:12px" onclick="applyProfile()">Apply Selected Profile</button>
        </div>
        <div class="settings-card">
            <h3>Active Alerts</h3>
            <div id="activeAlerts" style="color:var(--muted)">No active alerts.</div>
        </div>
    </section>

    <!-- Logs -->
    <section class="tab" id="tab-logs">
        <div class="settings-card">
            <h3>📁 Log Files (Internal Flash)</h3>
            <div style="display:flex;gap:10px;margin-bottom:16px">
                <button class="btn btn-blue" style="flex:unset;padding:9px 16px" onclick="loadFiles()">↻ Refresh</button>
                <span id="storageInfo" style="color:var(--muted);font-size:.85rem;align-self:center"></span>
            </div>
            <div id="fileList" style="color:var(--muted)">Loading...</div>
        </div>
    </section>

    <footer>Beer Flow Monitor v2.0 &nbsp;|&nbsp; <span id="footerIP">—</span> &nbsp;|&nbsp; <a href="/menu" style="color:var(--muted);text-decoration:none">&#127866; Tap Menu</a></footer>
</div>

<div id="toast"></div>

<script>
// ----------------------------------------------------------------
// State
// ----------------------------------------------------------------
let histChart = null;
let kegChart  = null;
let lastData  = null;
let profiles  = [];
let selectedProfileIdx = -1;

// ----------------------------------------------------------------
// Tab switching
// ----------------------------------------------------------------
function switchTab(id, btn) {
    document.querySelectorAll('section.tab').forEach(s => s.classList.remove('active'));
    document.querySelectorAll('nav.tabs button').forEach(b => b.classList.remove('active'));
    document.getElementById('tab-' + id).classList.add('active');
    btn.classList.add('active');
    if (id === 'history')     loadHistory();
    if (id === 'stats')       renderKegChart(lastData);
    if (id === 'alerts')      renderAlerts(lastData);
    if (id === 'diagnostics') loadDiagnostics();
    if (id === 'integrations') loadIntegrations();
    if (id === 'logs')         loadFiles();
}

// ----------------------------------------------------------------
// Toast notification
// ----------------------------------------------------------------
function toast(msg) {
    const el = document.getElementById('toast');
    el.textContent = msg;
    el.classList.add('show');
    setTimeout(() => el.classList.remove('show'), 2500);
}

// ----------------------------------------------------------------
// Tap card rendering
// ----------------------------------------------------------------
function renderTaps(data) {
    if (!data) return;
    lastData = data;
    const grid = document.getElementById('tapGrid');

    data.taps.forEach(tap => {
        const pct = Math.max(0, Math.min(100, (tap.level / tap.capacity * 100))).toFixed(1);
        const fillCls = pct > 30 ? 'ok' : pct > 15 ? 'warn' : 'crit';
        const cardCls = tap.isPouring ? 'pouring' : tap.lowKegAlert ? 'low' : tap.calibrating ? 'calibrating' : '';

        let badgesHtml = `<span class="badge badge-gray">#${tap.index + 1}</span>`;
        if (tap.isPouring)     badgesHtml += '<span class="badge badge-amber">Pouring</span>';
        if (tap.calibrating)   badgesHtml += '<span class="badge badge-yellow">Calibrating</span>';
        if (tap.lowKegAlert)   badgesHtml += '<span class="badge badge-red">Low Keg</span>';
        if (tap.sensorWarning) badgesHtml += '<span class="badge badge-green">Sensor ⚠</span>';

        const pourHtml = tap.isPouring
            ? `<div class="pouring-live">🍺 Live: ${tap.currentPour.toFixed(2)} oz</div>` : '';

        const qColor = (tap.pulseQuality||100) > 70 ? '#10b981' : (tap.pulseQuality||100) > 40 ? '#f59e0b' : '#ef4444';
        const sensorHtml = `
            <div style="font-size:.78rem;color:var(--muted);display:flex;gap:8px;align-items:center;margin-bottom:10px">
                <span>${tap.sensorHealth||'Good'}</span>
                <div style="flex:1;height:5px;background:var(--surface2);border-radius:3px;overflow:hidden">
                    <div style="width:${tap.pulseQuality||100}%;height:100%;background:${qColor};border-radius:3px"></div>
                </div>
                <span>${(tap.pulseQuality||100).toFixed(0)}%</span>
                ${tap.daysLeft > 0 ? `<span style="margin-left:auto">${tap.daysLeft.toFixed(1)}d left</span>` : ''}
            </div>`;

        grid.innerHTML = grid.innerHTML; // re-render below

        const id = 'card-' + tap.index;
        let card = document.getElementById(id);
        if (!card) {
            card = document.createElement('div');
            card.id = id;
            grid.appendChild(card);
        }

        card.className = 'tap-card ' + cardCls;
        card.innerHTML = `
            <div class="card-header">
                <span class="tap-name">${tap.name}</span>
                <div class="tap-badges">${badgesHtml}</div>
            </div>
            <div class="beer-row">
                <input id="bi_${tap.index}" value="${tap.beer}" placeholder="Beer name" style="flex:1">
            </div>
            <div class="beer-row" style="gap:6px;margin-top:4px">
                <label style="font-size:.75rem;color:var(--muted);align-self:center">ABV%</label>
                <input id="abv_${tap.index}" type="number" step="0.1" min="0" max="25" value="${(tap.abv||0).toFixed(1)}" style="width:60px">
                <label style="font-size:.75rem;color:var(--muted);align-self:center">IBU</label>
                <input id="ibu_${tap.index}" type="number" step="1" min="0" max="200" value="${tap.ibu||0}" style="width:55px">
                <input id="col_${tap.index}" type="color" value="${tap.beerColor||'#c8820a'}" title="Beer color" style="width:36px;height:28px;padding:2px;cursor:pointer;border-radius:4px">
                <button onclick="updateBeer(${tap.index})">Save</button>
            </div>
            <div class="keg-wrap">
                <div class="keg-label"><span>Keg Level</span><span>${tap.level.toFixed(1)} / ${tap.capacity} oz (${pct}%)</span></div>
                <div class="keg-track"><div class="keg-fill ${fillCls}" style="width:${pct}%"></div></div>
            </div>
            <div class="stats-row">
                <div class="stat"><div class="stat-val">${tap.pulsesPerOz.toFixed(1)}</div><div class="stat-lbl">pulses/oz</div></div>
                <div class="stat"><div class="stat-val">${tap.pourCount}</div><div class="stat-lbl">pours logged</div></div>
            </div>
            ${sensorHtml}
            ${pourHtml}
            <div id="lp_${tap.index}"></div>
            <div class="btn-row">
                <button class="btn btn-yellow" onclick="startCal(${tap.index})">Calibrate</button>
                <button class="btn btn-red"    onclick="resetKeg(${tap.index})">Reset Keg</button>
            </div>`;

        fetchLastPour(tap.index);
    });

    // Global KPIs
    document.getElementById('kTotalPours').textContent = data.totalPours;
    document.getElementById('kTotalOz').textContent    = data.totalOunces.toFixed(1);
    document.getElementById('kActiveTap').textContent  = data.activeTap >= 0 ? 'Tap ' + (data.activeTap + 1) : 'None';
    document.getElementById('kUptime').textContent     = fmtUptime(data.uptime);

    // MQTT pill
    const pill = document.getElementById('mqttPill');
    pill.className = 'pill ' + (data.mqtt_connected ? 'pill-green' : 'pill-red');
    pill.textContent = data.mqtt_connected ? 'MQTT ✓' : 'MQTT ✕';
}

function fetchLastPour(idx) {
    fetch('/api/last_pour?index=' + idx)
        .then(r => r.json())
        .then(lp => {
            if (!lp || !lp.ounces) return;
            document.getElementById('lp_' + idx).innerHTML = `
            <div class="last-pour">
                <div class="last-pour-title">Last Pour</div>
                <div class="last-pour-grid">
                    <div class="lp-item"><span>Amount</span><span>${lp.ounces.toFixed(2)} oz</span></div>
                    <div class="lp-item"><span>Duration</span><span>${lp.duration.toFixed(1)} s</span></div>
                    <div class="lp-item"><span>Peak Flow</span><span>${lp.peakFlow.toFixed(1)} oz/s</span></div>
                    <div class="lp-item"><span>Keg After</span><span>${lp.kegAfter.toFixed(1)} oz</span></div>
                </div>
                <div class="last-pour-time">${lp.time}</div>
            </div>`;
        }).catch(() => {});
}

// ----------------------------------------------------------------
// API actions
// ----------------------------------------------------------------
function post(url, body) {
    return fetch(url, {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body});
}

function updateBeer(i) {
    const name  = document.getElementById('bi_'  + i).value;
    const abv   = document.getElementById('abv_' + i).value || '0';
    const ibu   = document.getElementById('ibu_' + i).value || '0';
    const color = document.getElementById('col_' + i).value || '#c8820a';
    post('/api/beer/update', 'index=' + i
        + '&name='  + encodeURIComponent(name)
        + '&abv='   + abv
        + '&ibu='   + ibu
        + '&color=' + encodeURIComponent(color))
        .then(() => toast('Beer updated!'));
}

function resetKeg(i) {
    if (!confirm('Reset Tap ' + (i+1) + ' keg to full?')) return;
    post('/api/reset', 'index=' + i).then(() => { toast('Keg reset!'); pollStatus(); });
}

function startCal(i) {
    post('/api/calibrate/start', 'index=' + i).then(() => {
        const oz = prompt('Pour a measured amount then enter how many ounces:');
        if (oz && !isNaN(oz) && parseFloat(oz) > 0) {
            post('/api/calibrate/end', 'index=' + i + '&ounces=' + oz)
                .then(() => { toast('Calibration saved!'); pollStatus(); });
        } else {
            toast('Calibration cancelled.');
        }
    });
}

// ----------------------------------------------------------------
// History tab
// ----------------------------------------------------------------
function loadHistory() {
    const val = document.getElementById('histSel').value;
    const url = val === 'all' ? '/api/history?index=all' : '/api/history?index=' + val;
    fetch(url).then(r => r.json()).then(renderHistory).catch(console.error);
}

function renderHistory(rows) {
    const tbody = document.getElementById('histBody');
    tbody.innerHTML = rows.map(r => `
        <tr>
            <td>${r.time}</td>
            <td>${r.tap !== undefined ? 'Tap ' + r.tap : '—'}</td>
            <td>${r.beer}</td>
            <td>${r.ounces.toFixed(2)}</td>
            <td>${r.kegAfter.toFixed(1)} oz</td>
            <td>${(r.duration||0).toFixed(1)} s</td>
            <td>${(r.peakFlow||0).toFixed(1)}</td>
        </tr>`).join('');

    // Update chart
    if (histChart) { histChart.destroy(); histChart = null; }
    const ctx = document.getElementById('histChart').getContext('2d');
    histChart = new Chart(ctx, {
        type: 'bar',
        data: {
            labels: rows.map(r => r.time),
            datasets: [{ label: 'oz', data: rows.map(r => r.ounces), backgroundColor: '#ff6b35aa', borderColor: '#ff6b35', borderWidth: 1 }]
        },
        options: { responsive: true, plugins:{ legend:{ labels:{ color:'#f3f4f6' } } }, scales:{ x:{ ticks:{ color:'#9ca3af', maxRotation:45 } }, y:{ ticks:{ color:'#9ca3af' } } } }
    });
}

// ----------------------------------------------------------------
// Stats tab — keg level chart
// ----------------------------------------------------------------
function renderKegChart(data) {
    if (!data) return;
    if (kegChart) { kegChart.destroy(); kegChart = null; }
    const ctx = document.getElementById('kegChart').getContext('2d');
    const colors = ['#ff6b35','#10b981','#3b82f6','#f59e0b','#a855f7','#ec4899'];
    kegChart = new Chart(ctx, {
        type: 'bar',
        data: {
            labels: data.taps.map(t => t.name),
            datasets: [{
                label: 'Keg Level (oz)',
                data: data.taps.map(t => t.level),
                backgroundColor: data.taps.map((_, i) => colors[i % colors.length] + '88'),
                borderColor:     data.taps.map((_, i) => colors[i % colors.length]),
                borderWidth: 2
            }]
        },
        options: { responsive: true, scales:{ y:{ max: data.taps[0]?.capacity || 640, ticks:{ color:'#9ca3af' } }, x:{ ticks:{ color:'#9ca3af' } } }, plugins:{ legend:{ labels:{ color:'#f3f4f6' } } } }
    });
}

// ----------------------------------------------------------------
// Alerts tab
// ----------------------------------------------------------------
function renderAlerts(data) {
    if (!data) return;
    const el = document.getElementById('activeAlerts');
    const low = data.taps.filter(t => t.lowKegAlert);
    if (!low.length) { el.innerHTML = '<span style="color:var(--muted)">No active alerts.</span>'; return; }
    el.innerHTML = low.map(t => `
        <div style="display:flex;justify-content:space-between;align-items:center;padding:12px;background:var(--surface2);border-radius:8px;margin-bottom:10px;border-left:3px solid var(--red)">
            <span>⚠️ <strong>${t.name}</strong> — ${t.level.toFixed(1)} oz remaining</span>
            <button class="btn btn-blue" style="flex:unset;padding:6px 14px" onclick="clearAlert(${t.index})">Dismiss</button>
        </div>`).join('');
}

function clearAlert(i) {
    post('/api/alert/clear', 'index=' + i).then(() => pollStatus());
}

function dismissAlert() {
    document.getElementById('alertBanner').classList.remove('show');
}

function saveThreshold() {
    post('/api/settings/alert', 'threshold=' + document.getElementById('inpThreshold').value)
        .then(() => toast('Alert threshold saved!'));
}

function saveWebhook() {
    const url = encodeURIComponent(document.getElementById('inpWebhook').value);
    post('/api/settings/webhook', 'url=' + url)
        .then(() => toast('Webhook URL saved!'));
}

// ----------------------------------------------------------------
// Diagnostics tab
// ----------------------------------------------------------------
function loadDiagnostics() {
    fetch('/api/diagnostics').then(r => r.json()).then(data => {
        const grid = document.getElementById('diagGrid');
        grid.innerHTML = data.map(d => {
            const hClass = d.health === 'Good' ? 'color:var(--green)' : (d.health === 'Stuck!' || d.health === 'No Signal') ? 'color:var(--red)' : 'color:var(--yellow)';
            const qColor = d.pulseQuality > 70 ? '#10b981' : d.pulseQuality > 40 ? '#f59e0b' : '#ef4444';
            return `
            <div style="background:var(--surface);border-radius:var(--radius);padding:18px;border:1px solid #4b5563">
                <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:12px">
                    <span style="font-weight:700;color:var(--amber)">Tap ${d.tap}</span>
                    <span style="font-size:.75rem;font-weight:700;${hClass}">${d.health}</span>
                </div>
                <div style="margin-bottom:10px">
                    <div style="display:flex;justify-content:space-between;font-size:.72rem;color:var(--muted);margin-bottom:4px"><span>Signal Quality</span><span>${d.pulseQuality.toFixed(0)}%</span></div>
                    <div style="height:7px;background:var(--surface2);border-radius:4px;overflow:hidden"><div style="width:${d.pulseQuality}%;height:100%;background:${qColor};border-radius:4px"></div></div>
                </div>
                <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;font-size:.82rem">
                    <div><div style="font-size:.68rem;color:var(--muted)">Avg Interval</div>${d.avgIntervalMs.toFixed(1)} ms</div>
                    <div><div style="font-size:.68rem;color:var(--muted)">Std Dev</div>${d.stdDevMs.toFixed(1)} ms</div>
                    <div><div style="font-size:.68rem;color:var(--muted)">Noise Pulses</div>${d.noisePulses}</div>
                    <div><div style="font-size:.68rem;color:var(--muted)">Total Pulses</div>${d.totalPulses}</div>
                    <div><div style="font-size:.68rem;color:var(--muted)">Peak Flow</div>${d.peakFlowOz.toFixed(1)} oz/s</div>
                    <div><div style="font-size:.68rem;color:var(--muted)">Avg Pour</div>${d.avgPourSizeOz.toFixed(1)} oz</div>
                    <div><div style="font-size:.68rem;color:var(--muted)">Pours/Day</div>${d.poursPerDay.toFixed(1)}</div>
                    <div><div style="font-size:.68rem;color:var(--muted)">Days Left</div>${d.daysLeft > 0 ? d.daysLeft.toFixed(1) : '—'}</div>
                    <div><div style="font-size:.68rem;color:var(--muted)">Calibrated</div>${d.calibrated ? '✓ Yes' : '✗ No'}</div>
                    <div><div style="font-size:.68rem;color:var(--muted)">Pulses/oz</div>${d.pulsesPerOz.toFixed(0)}</div>
                </div>
                ${d.warning ? `<div style="background:rgba(239,68,68,.1);border:1px solid rgba(239,68,68,.3);border-radius:6px;padding:8px;margin-top:10px;font-size:.78rem;color:#fca5a5">⚠️ ${d.warning}</div>` : ''}
            </div>`;
        }).join('');
    }).catch(() => toast('Failed to load diagnostics'));
}

// ----------------------------------------------------------------
// Integrations tab
// ----------------------------------------------------------------
function loadIntegrations() {
    fetch('/api/notifications/config').then(r => r.json()).then(cfg => {
        document.getElementById('inpSlackUrl').value   = cfg.slackUrl || '';
        document.getElementById('togSlack').checked    = !!cfg.slackEnabled;
        document.getElementById('inpDiscordUrl').value = cfg.discordUrl || '';
        document.getElementById('togDiscord').checked  = !!cfg.discordEnabled;
        document.getElementById('inpGenericUrl').value = cfg.genericUrl || '';
        document.getElementById('togGeneric').checked  = !!cfg.genericEnabled;
        document.getElementById('togOnPour').checked   = !!cfg.onPour;
        document.getElementById('togOnLow').checked    = cfg.onLowKeg !== false;
        document.getElementById('togOnEmpty').checked  = cfg.onEmpty  !== false;
        document.getElementById('togOnSensor').checked = cfg.onSensorErr !== false;
        document.getElementById('togOnBoot').checked   = !!cfg.onBoot;
    }).catch(() => {});
}

function saveIntegrations() {
    fetch('/api/notifications/config', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
            slackUrl:       document.getElementById('inpSlackUrl').value,
            slackEnabled:   document.getElementById('togSlack').checked,
            discordUrl:     document.getElementById('inpDiscordUrl').value,
            discordEnabled: document.getElementById('togDiscord').checked,
            genericUrl:     document.getElementById('inpGenericUrl').value,
            genericEnabled: document.getElementById('togGeneric').checked,
        })
    }).then(() => toast('Integration settings saved'));
}

function saveNotifyEvents() {
    fetch('/api/notifications/config', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
            onPour:      document.getElementById('togOnPour').checked,
            onLowKeg:    document.getElementById('togOnLow').checked,
            onEmpty:     document.getElementById('togOnEmpty').checked,
            onSensorErr: document.getElementById('togOnSensor').checked,
            onBoot:      document.getElementById('togOnBoot').checked,
        })
    }).then(() => toast('Notification events saved'));
}

function testSlack() {
    post('/api/notifications/test/slack', '').then(r => r.json())
        .then(d => toast(d.success ? '✓ Slack test sent!' : 'Slack test failed — check URL'));
}

function testDiscord() {
    post('/api/notifications/test/discord', '').then(r => r.json())
        .then(d => toast(d.success ? '✓ Discord test sent!' : 'Discord test failed — check URL'));
}

// ----------------------------------------------------------------
// Keg profiles (in Settings tab)
// ----------------------------------------------------------------
function loadProfiles() {
    fetch('/api/profiles').then(r => r.json()).then(p => {
        profiles = p;
        document.getElementById('profileGrid').innerHTML = p.map((pr, i) => `
            <button id="pb_${i}" onclick="selectProfile(${i})"
                style="background:var(--surface2);border:1px solid #4b5563;color:var(--text);border-radius:6px;padding:10px;cursor:pointer;text-align:left;transition:border-color .2s;font-family:inherit">
                <div style="font-size:.85rem;font-weight:600">${pr.name}</div>
                <div style="font-size:.72rem;color:var(--muted);margin-top:2px">${pr.oz} oz</div>
            </button>`).join('');
    }).catch(() => {});
}

function selectProfile(i) {
    selectedProfileIdx = i;
    document.querySelectorAll('[id^="pb_"]').forEach(b => b.style.borderColor = '#4b5563');
    const btn = document.getElementById('pb_' + i);
    if (btn) btn.style.borderColor = 'var(--amber)';
}

function applyProfile() {
    if (selectedProfileIdx < 0) { toast('Select a profile first'); return; }
    const tap = document.getElementById('profileTapSel').value;
    post('/api/settings/profile', 'tap=' + tap + '&profile=' + selectedProfileIdx)
        .then(() => { toast('Profile applied to Tap ' + (parseInt(tap) + 1)); pollStatus(); });
}

// ----------------------------------------------------------------
// Polling
// ----------------------------------------------------------------
let _pausePoll = false;

// Pause polling while any input/select/textarea is focused
document.addEventListener('focusin',  e => { if (['INPUT','SELECT','TEXTAREA'].includes(e.target.tagName)) _pausePoll = true;  });
document.addEventListener('focusout', e => { if (['INPUT','SELECT','TEXTAREA'].includes(e.target.tagName)) _pausePoll = false; });

function pollStatus() {
    if (_pausePoll) return;
    fetch('/api/status')
        .then(r => r.json())
        .then(data => {
            if (_pausePoll) return;  // check again after async gap
            renderTaps(data);
            checkAlertBanner(data);
        })
        .catch(console.error);
}

function checkAlertBanner(data) {
    const low = data.taps.find(t => t.lowKegAlert);
    const banner = document.getElementById('alertBanner');
    if (low) {
        document.getElementById('alertMsg').textContent = `⚠️ Low keg: ${low.name} (${low.level.toFixed(1)} oz left)`;
        banner.classList.add('show');
    }
}

function fmtUptime(s) {
    const d = Math.floor(s / 86400), h = Math.floor((s%86400)/3600), m = Math.floor((s%3600)/60), ss = s%60;
    if (d) return `${d}d ${h}h ${m}m`;
    if (h) return `${h}h ${m}m ${ss}s`;
    if (m) return `${m}m ${ss}s`;
    return `${ss}s`;
}

// ----------------------------------------------------------------
// Logs / file download
// ----------------------------------------------------------------
function loadFiles() {
    const el = document.getElementById('fileList');
    el.textContent = 'Loading...';
    fetch('/api/files')
        .then(r => r.json())
        .then(d => {
            if (!d.available) { el.textContent = 'LittleFS not available.'; return; }
            fetch('/api/storage').then(r2 => r2.json()).then(s => {
                if (s.total_kb) document.getElementById('storageInfo').textContent =
                    `${s.used_kb} KB used / ${s.total_kb} KB total`;
            }).catch(() => {});
            if (!d.files || d.files.length === 0) { el.textContent = 'No files found.'; return; }
            el.innerHTML = d.files.map(f =>
                `<div style="display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid var(--surface2)">` +
                `<span style="font-family:monospace">${f.name} <span style="color:var(--muted);font-size:.8rem">(${(f.size/1024).toFixed(1)} KB)</span></span>` +
                `<div style="display:flex;gap:8px">` +
                `<a href="/api/file?path=${encodeURIComponent(f.name)}" download="${f.name}" class="btn btn-blue" style="padding:6px 14px;text-decoration:none;font-size:.85rem;font-weight:700;border-radius:8px">Download</a>` +
                `<button class="btn btn-red" style="padding:6px 14px;font-size:.85rem" onclick="deleteFile('${f.name}')">Delete</button>` +
                `</div></div>`
            ).join('');
        })
        .catch(() => { el.textContent = 'Error loading files.'; });
}

function deleteFile(name) {
    if (!confirm('Delete ' + name + '?')) return;
    fetch('/api/file/delete?path=' + encodeURIComponent(name), {method:'POST'})
        .then(r => r.json())
        .then(d => { toast(d.success ? 'Deleted.' : 'Delete failed.'); loadFiles(); });
}

// ----------------------------------------------------------------
// Boot
// ----------------------------------------------------------------
pollStatus();
setInterval(pollStatus, 2000);
loadProfiles();
</script>
</body>
</html>
)rawliteral";

// ============================================================
// Beer Menu Display (served at /menu) — Raspberrypints style
// ============================================================
const char MENU_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>On Tap Now</title>
<style>
body {
  background-color: #000;
  font: 1.5em Georgia, Arial, Verdana, sans-serif;
  margin: 5px;
  color: #fff;
}
hr {
  border: 0; height: 1px; margin: 2px 0;
  background: linear-gradient(to right, rgba(255,255,255,0), rgba(255,255,255,0.75), rgba(255,255,255,0));
}
h1 { margin:0; color:#fff; }
h2, h3, p { margin:0; color:#a0a0a0; }
.bodywrapper { width:100%; }
/* Header — auto height so SVGs never clip */
.header { width:100%; overflow:visible; margin-bottom:6px; display:flex; align-items:center; }
.HeaderLeft  { width:25%; font:.85em Georgia; color:#fff; font-weight:bold; text-align:left; }
.HeaderCenter{ flex:1; text-align:center; font:1em Georgia; color:#fff; padding:6px 0; }
.HeaderRight { width:25%; font:.85em Georgia; color:#fff; font-weight:bold; text-align:right; }
/* Table */
table { width:100%; border-collapse:collapse; }
table th, table td { border-left:1px white dashed; padding:1px 5px; text-align:center; vertical-align:middle; color:white; }
table th:first-child, table td:first-child { border-left:none; }
table thead { background-color:rgba(153,153,153,.3); }
table thead th { font:.65em Georgia; color:white; font-weight:bold; padding:4px 5px; }
table .tap-num { width:55px; }
table .srm     { width:100px; }
table td.name  { text-align:left; padding:5px; }
table .abvcol  { width:100px; }
table .ibucol  { width:90px; }
table .keg     { width:120px; }
/* ABV pint glass — same shape as SRM, filled proportionally */
.abv-container { height:110px; width:55px; margin:4px auto; position:relative; }
/* IBU bitterness bar */
.ibu-container { height:110px; width:53px; margin:4px auto; text-align:center; }
.ibu-bar-bg { display:inline-block; position:relative; width:26px; height:90px;
  background:rgba(255,255,255,0.06); border:1px solid rgba(255,255,255,0.14);
  border-radius:3px; overflow:hidden; vertical-align:bottom; }
.ibu-bar-fill { position:absolute; bottom:0; left:0; width:100%;
  background:rgba(160,210,70,0.80); border-radius:0 0 2px 2px; }
.ibu-label { font:.7em Georgia; color:#888; margin-top:3px; }
table h1 { color:white; display:block; font:1.5em Georgia; font-weight:bold; }
table h2 { font:1em Georgia; text-align:center; }
table h3 { font:.8em Georgia; text-align:center; }
span.tapcircle {
  background-color:rgba(153,153,153,.3); border-radius:1em; color:#fff;
  display:inline-block; font-weight:bold; font-size:30px;
  line-height:2em; text-align:center; width:2em;
}
.altrow { background-color:rgba(153,153,153,.23); }
/* Pint glass (beer color column) */
.srm-container { height:110px; width:55px; margin:4px auto; position:relative; }
.pint-wrap {
  width:55px; height:100px; overflow:hidden; position:relative;
  clip-path:polygon(9% 0%,91% 0%,81% 100%,19% 100%);
  background:rgba(255,255,255,0.04);
}
.pint-beer { position:absolute; bottom:0; left:0; width:100%; background:#c8820a; }
.pint-foam { position:absolute; left:0; width:100%; height:9px; background:rgba(255,255,255,0.28); }
.pint-outline {
  position:absolute; top:0; left:0; width:100%; height:100%; z-index:2;
  box-shadow:inset 0 0 0 1.5px rgba(255,255,255,0.22);
  clip-path:polygon(9% 0%,91% 0%,81% 100%,19% 100%);
}
.pint-pct { font:.7em Georgia; color:#888; margin-top:2px; }
/* Status */
.s-avail   { color:#80d080; }
.s-low     { color:#e0a060; }
.s-empty   { color:#555; }
.s-pouring { color:#f5a623; animation:blink 1.1s ease-in-out infinite; }
@keyframes blink { 0%,100%{opacity:1} 50%{opacity:.3} }
#footer { text-align:center; padding:8px 0; margin-top:6px; font:.6em Georgia; color:#444; letter-spacing:3px; border-top:1px dashed rgba(255,255,255,0.08); }
</style>
</head>
<body>
<div class="bodywrapper">

<div class="header">
  <div class="HeaderLeft">
    <!-- Green hop cone, top-left corner -->
    <svg viewBox="0 0 70 120" width="75" height="110" xmlns="http://www.w3.org/2000/svg">
      <!-- Stem + tendril -->
      <line x1="35" y1="6" x2="35" y2="20" stroke="#4a7a1c" stroke-width="2.5" stroke-linecap="round"/>
      <path d="M35 11 Q24 7 21 10" stroke="#4a7a1c" stroke-width="1.5" fill="none" stroke-linecap="round"/>
      <!-- Cone body (dark base) -->
      <ellipse cx="35" cy="63" rx="19" ry="38" fill="#2c5a12"/>
      <!-- Bottom tip -->
      <ellipse cx="35" cy="97" rx="9" ry="5" fill="#3a6e18"/>
      <!-- Bracts layer 1 - bottom -->
      <ellipse cx="35" cy="92" rx="13" ry="6" fill="#416c18"/>
      <ellipse cx="21" cy="87" rx="11" ry="5" fill="#437218" transform="rotate(28,21,87)"/>
      <ellipse cx="49" cy="87" rx="11" ry="5" fill="#437218" transform="rotate(-28,49,87)"/>
      <!-- Bracts layer 2 -->
      <ellipse cx="35" cy="76" rx="15" ry="7" fill="#4e8c1e"/>
      <ellipse cx="18" cy="73" rx="12" ry="6" fill="#549420" transform="rotate(18,18,73)"/>
      <ellipse cx="52" cy="73" rx="12" ry="6" fill="#549420" transform="rotate(-18,52,73)"/>
      <!-- Bracts layer 3 -->
      <ellipse cx="35" cy="60" rx="16" ry="7" fill="#5a9e24"/>
      <ellipse cx="16" cy="57" rx="13" ry="6" fill="#60a626" transform="rotate(14,16,57)"/>
      <ellipse cx="54" cy="57" rx="13" ry="6" fill="#60a626" transform="rotate(-14,54,57)"/>
      <!-- Bracts layer 4 -->
      <ellipse cx="35" cy="44" rx="15" ry="7" fill="#68ae2a"/>
      <ellipse cx="18" cy="41" rx="12" ry="6" fill="#6eb62a" transform="rotate(11,18,41)"/>
      <ellipse cx="52" cy="41" rx="12" ry="6" fill="#6eb62a" transform="rotate(-11,52,41)"/>
      <!-- Bracts layer 5 - top -->
      <ellipse cx="35" cy="29" rx="13" ry="6" fill="#76be30"/>
      <ellipse cx="22" cy="27" rx="10" ry="5" fill="#7cc430" transform="rotate(9,22,27)"/>
      <ellipse cx="48" cy="27" rx="10" ry="5" fill="#7cc430" transform="rotate(-9,48,27)"/>
      <!-- Top cap -->
      <ellipse cx="35" cy="20" rx="9" ry="5" fill="#86cc36"/>
      <!-- Shine -->
      <ellipse cx="27" cy="50" rx="3.5" ry="13" fill="rgba(255,255,255,0.09)" transform="rotate(-8,27,50)"/>
    </svg>
  </div>
  <div class="HeaderCenter">
    <h1>The Tap Room</h1>
    <h2>On Tap Today</h2>
    <hr>
  </div>
  <div class="HeaderRight" style="text-align:right">
    <!-- Green hop cone, top-right corner (mirrored) -->
    <svg viewBox="0 0 70 120" width="75" height="110" xmlns="http://www.w3.org/2000/svg" style="transform:scaleX(-1)">
      <line x1="35" y1="6" x2="35" y2="20" stroke="#4a7a1c" stroke-width="2.5" stroke-linecap="round"/>
      <path d="M35 11 Q24 7 21 10" stroke="#4a7a1c" stroke-width="1.5" fill="none" stroke-linecap="round"/>
      <ellipse cx="35" cy="63" rx="19" ry="38" fill="#2c5a12"/>
      <ellipse cx="35" cy="97" rx="9" ry="5" fill="#3a6e18"/>
      <ellipse cx="35" cy="92" rx="13" ry="6" fill="#416c18"/>
      <ellipse cx="21" cy="87" rx="11" ry="5" fill="#437218" transform="rotate(28,21,87)"/>
      <ellipse cx="49" cy="87" rx="11" ry="5" fill="#437218" transform="rotate(-28,49,87)"/>
      <ellipse cx="35" cy="76" rx="15" ry="7" fill="#4e8c1e"/>
      <ellipse cx="18" cy="73" rx="12" ry="6" fill="#549420" transform="rotate(18,18,73)"/>
      <ellipse cx="52" cy="73" rx="12" ry="6" fill="#549420" transform="rotate(-18,52,73)"/>
      <ellipse cx="35" cy="60" rx="16" ry="7" fill="#5a9e24"/>
      <ellipse cx="16" cy="57" rx="13" ry="6" fill="#60a626" transform="rotate(14,16,57)"/>
      <ellipse cx="54" cy="57" rx="13" ry="6" fill="#60a626" transform="rotate(-14,54,57)"/>
      <ellipse cx="35" cy="44" rx="15" ry="7" fill="#68ae2a"/>
      <ellipse cx="18" cy="41" rx="12" ry="6" fill="#6eb62a" transform="rotate(11,18,41)"/>
      <ellipse cx="52" cy="41" rx="12" ry="6" fill="#6eb62a" transform="rotate(-11,52,41)"/>
      <ellipse cx="35" cy="29" rx="13" ry="6" fill="#76be30"/>
      <ellipse cx="22" cy="27" rx="10" ry="5" fill="#7cc430" transform="rotate(9,22,27)"/>
      <ellipse cx="48" cy="27" rx="10" ry="5" fill="#7cc430" transform="rotate(-9,48,27)"/>
      <ellipse cx="35" cy="20" rx="9" ry="5" fill="#86cc36"/>
      <ellipse cx="27" cy="50" rx="3.5" ry="13" fill="rgba(255,255,255,0.09)" transform="rotate(-8,27,50)"/>
    </svg>
  </div>
</div>

<table>
  <thead>
    <tr>
      <th class="tap-num">TAP</th>
      <th class="srm">COLOR</th>
      <th class="name">BEER NAME</th>
      <th class="abvcol">ABV %</th>
      <th class="ibucol">BITTERNESS</th>
      <th class="keg">KEG LEVEL</th>
      <th style="width:110px">STATUS</th>
    </tr>
  </thead>
  <tbody id="tap-body"></tbody>
</table>

<div id="footer">
  DRINK RESPONSIBLY &nbsp;&bull;&nbsp; AUTO-REFRESHES EVERY 30 SECONDS
  &nbsp;&nbsp;&bull;&nbsp;&nbsp;
  <a href="/" style="color:#666;text-decoration:none;letter-spacing:2px">&#8592; DASHBOARD</a>
</div>
</div>

<script>
/* Build an SVG keg graphic showing fill level */
function kegSvg(idx, pct) {
  /* fillable interior: y=28 to y=97, height=69 */
  var fillH = Math.round(69 * pct / 100);
  var fillY = 97 - fillH;
  var col;
  if      (pct >= 45) col = 'rgba(55,195,55,0.78)';
  else if (pct >= 25) col = 'rgba(210,185,40,0.85)';
  else if (pct >= 15) col = 'rgba(210,120,40,0.90)';
  else if (pct >  0)  col = 'rgba(210,45,45,0.90)';
  else                col = 'rgba(35,35,35,0.50)';
  var cid = 'kclip' + idx;
  return '<svg viewBox="0 0 64 120" width="64" height="118" style="display:block;margin:0 auto">'
    /* shadow */
    + '<ellipse cx="32" cy="116" rx="20" ry="3" fill="rgba(0,0,0,0.55)"/>'
    /* body shell */
    + '<rect x="10" y="18" width="44" height="86" rx="10" fill="#1e1e1e" stroke="rgba(255,255,255,0.18)" stroke-width="1.5"/>'
    /* top rim */
    + '<rect x="5"  y="12" width="54" height="16" rx="8" fill="#282828" stroke="rgba(255,255,255,0.28)" stroke-width="1.5"/>'
    /* bottom rim */
    + '<rect x="5"  y="97" width="54" height="16" rx="8" fill="#282828" stroke="rgba(255,255,255,0.28)" stroke-width="1.5"/>'
    /* beer fill clipped to interior */
    + '<clipPath id="' + cid + '"><rect x="11" y="28" width="42" height="69" rx="5"/></clipPath>'
    + '<rect x="11" y="' + fillY + '" width="42" height="' + (fillH + 4) + '" fill="' + col + '" clip-path="url(#' + cid + ')"/>'
    /* interior border */
    + '<rect x="11" y="28" width="42" height="69" rx="5" fill="none" stroke="rgba(255,255,255,0.07)" stroke-width="1"/>'
    /* subtle highlight strip */
    + '<rect x="13" y="30" width="5" height="62" rx="2" fill="rgba(255,255,255,0.04)"/>'
    /* valve / coupling on top */
    + '<rect x="22" y="3"  width="20" height="13" rx="5" fill="#2c2c2c" stroke="rgba(255,255,255,0.30)" stroke-width="1.5"/>'
    + '<circle cx="32" cy="9" r="4" fill="#181818" stroke="rgba(255,255,255,0.22)" stroke-width="1.5"/>'
    + '</svg>';
}

function render(data) {
  var h = '';
  for (var i = 0; i < data.taps.length; i++) {
    var t   = data.taps[i];
    var pct = t.capacity > 0 ? Math.max(0, Math.min(100, (t.level / t.capacity) * 100)) : 0;
    var empty = !t.beer || t.beer === 'Unknown' || t.beer === '' || pct < 1;
    var alt   = i % 2 === 1 ? ' class="altrow"' : '';
    var fh    = empty ? 0 : Math.round(pct);

    /* Pint glass (beer color) — fill color from beerColor field */
    var beerCol = (t.beerColor && t.beerColor.length > 0) ? t.beerColor : '#c8820a';
    var srm = '<td class="srm"><div class="srm-container">'
            + '<div class="pint-wrap">'
            + '<div class="pint-beer" style="height:' + fh + '%;background:' + beerCol + '"></div>'
            + (empty ? '' : '<div class="pint-foam" style="bottom:' + fh + '%"></div>')
            + '</div><div class="pint-outline"></div>'
            + '</div></td>';

    /* Beer name */
    var name = '<td class="name">'
             + (empty ? '<h3>Nothing on tap</h3>' : '<h1>' + t.beer + '</h1>')
             + '</td>';

    /* ABV pint glass */
    var abvVal = t.abv || 0;
    var abvH   = empty ? 0 : Math.min(100, Math.round(abvVal / 12 * 100));
    var abv = '<td class="abvcol"><div class="abv-container">'
            + '<div class="pint-wrap">'
            + '<div class="pint-beer" style="height:' + abvH + '%;background:rgba(210,170,30,0.80)"></div>'
            + (abvH > 0 ? '<div class="pint-foam" style="bottom:' + abvH + '%"></div>' : '')
            + '</div><div class="pint-outline"></div>'
            + '</div>'
            + '<h3>' + (empty || abvVal === 0 ? '&mdash;' : abvVal.toFixed(1) + '%') + '</h3>'
            + '</td>';

    /* IBU bitterness bar */
    var ibuVal = t.ibu || 0;
    var ibuH   = empty ? 0 : Math.min(100, Math.round(ibuVal / 100 * 100));
    var ibu = '<td class="ibucol"><div class="ibu-container">'
            + '<div class="ibu-bar-bg"><div class="ibu-bar-fill" style="height:' + ibuH + '%"></div></div>'
            + '<div class="ibu-label">' + (empty || ibuVal === 0 ? '&mdash;' : ibuVal + ' IBU') + '</div>'
            + '</div></td>';

    /* Keg SVG */
    var keg = '<td class="keg" style="padding:6px 4px">'
            + kegSvg(t.index, pct)
            + '<h3 style="margin-top:3px">' + (empty ? '&mdash;' : fh + '%') + '</h3>'
            + '</td>';

    /* Status */
    var sc, st;
    if      (t.isPouring)   { sc = 's-pouring'; st = '&#9654; Pouring'; }
    else if (empty)         { sc = 's-empty';   st = 'Empty'; }
    else if (t.lowKegAlert) { sc = 's-low';     st = '&#9888; Low'; }
    else                    { sc = 's-avail';   st = 'Available'; }

    h += '<tr' + alt + '>'
       + '<td class="tap-num"><span class="tapcircle">' + (t.index + 1) + '</span></td>'
       + srm + name + abv + ibu + keg
       + '<td style="vertical-align:middle"><h2 class="' + sc + '">' + st + '</h2></td>'
       + '</tr>';
  }
  document.getElementById('tap-body').innerHTML = h;
}
function poll() {
  fetch('/api/status').then(function(r){return r.json();}).then(render).catch(function(){});
}
poll();
setInterval(poll, 30000);
</script>
</body>
</html>
)rawliteral";

// ============================================================
// WiFi
// ============================================================
void setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WIFI_HOSTNAME);

    #if !WIFI_USE_DHCP
    IPAddress ip, gw, sn;
    ip.fromString(STATIC_IP);
    gw.fromString(GATEWAY);
    sn.fromString(SUBNET);
    if (!WiFi.config(ip, gw, sn)) {
        LOG_E("Static IP config failed");
    }
    #endif

    LOG_I("Connecting to WiFi SSID: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries++ < 40) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        LOG_I("WiFi connected — IP: %s", WiFi.localIP().toString().c_str());
    } else {
        LOG_E("WiFi connection failed");
    }
}

// ============================================================
// OTA
// ============================================================
void setupOTA() {
    ArduinoOTA.setHostname(WIFI_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setMdnsEnabled(true);

    ArduinoOTA.onStart([]() {
        LOG_I("OTA update starting: %s",
              ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem");
    });
    ArduinoOTA.onEnd([]()   { LOG_I("OTA complete"); });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        Serial.printf("[OTA] %u%%\r", p / (t / 100));
    });
    ArduinoOTA.onError([](ota_error_t e) {
        const char* msg;
        switch (e) {
            case OTA_AUTH_ERROR:    msg = "Auth Failed";    break;
            case OTA_BEGIN_ERROR:   msg = "Begin Failed";   break;
            case OTA_CONNECT_ERROR: msg = "Connect Failed"; break;
            case OTA_RECEIVE_ERROR: msg = "Receive Failed"; break;
            case OTA_END_ERROR:     msg = "End Failed";     break;
            default:                msg = "Unknown";        break;
        }
        LOG_E("OTA error: %s", msg);
    });

    ArduinoOTA.begin();
    LOG_I("OTA ready — %s.local:%d", WIFI_HOSTNAME, OTA_PORT);
}

// ============================================================
// MQTT
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String t(topic);
    String body;
    body.reserve(length);
    for (unsigned int i = 0; i < length; i++) body += (char)payload[i];
    LOG_D("MQTT rx [%s] %s", topic, body.c_str());

    if (t.endsWith("/command")) {
        if (body == "RESET_ALL") {
            for (int i = 0; i < NUM_TAPS; i++) TapManager::getInstance().resetKeg(i);
            LOG_I("All kegs reset via MQTT command");
        }
    }
}

void setupMQTT() {
    g_mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    g_mqtt.setCallback(mqttCallback);
    g_mqtt.setBufferSize(2048);
    LOG_I("MQTT configured: %s:%d (client=%s)", MQTT_SERVER, MQTT_PORT, MQTT_CLIENT_ID);
}

bool mqttConnect() {
    String willTopic(MQTT_TOPIC_AVAILABILITY);
    if (g_mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD,
                       willTopic.c_str(), 0, true, "offline")) {
        g_mqtt.publish(MQTT_TOPIC_AVAILABILITY, "online", true);
        g_mqtt.subscribe(MQTT_TOPIC_PREFIX "/#");
        mqttPublishHADiscovery();
        LOG_I("MQTT connected");
        return true;
    }
    LOG_W("MQTT connect failed — state %d", g_mqtt.state());
    return false;
}

void mqttPublishTapData() {
    if (!g_mqtt.connected()) return;
    TapManager& tm = TapManager::getInstance();

    for (int i = 0; i < NUM_TAPS; i++) {
        TapConfig cfg = tm.getConfig(i);
        TapState  st  = tm.getState(i);

        // Keg level
        DynamicJsonDocument kegDoc(128);
        kegDoc["level"] = st.currentKegLevel;
        kegDoc["beer"]  = cfg.beerName;
        String kegJson; serializeJson(kegDoc, kegJson);
        g_mqtt.publish(("beermonitor/tap/" + String(i+1) + "/keg").c_str(), kegJson.c_str());

        // Status
        g_mqtt.publish(("beermonitor/tap/" + String(i+1) + "/status").c_str(),
                       st.isPouring ? "pouring" : "idle");

        // Last pour
        if (st.hasLastPour) {
            g_mqtt.publish(("beermonitor/tap/" + String(i+1) + "/last_pour").c_str(),
                           tm.getLastPourJSON(i).c_str(), true);
        }
    }

    // Aggregate stats
    DynamicJsonDocument statsDoc(64);
    statsDoc["total_pours"]  = tm.getTotalPours();
    statsDoc["total_ounces"] = tm.getTotalOunces();
    String statsJson; serializeJson(statsDoc, statsJson);
    g_mqtt.publish(MQTT_TOPIC_STATS, statsJson.c_str());
}

void mqttPublishPourEvent(int i) {
    if (!g_mqtt.connected()) return;
    TapManager& tm = TapManager::getInstance();
    TapConfig cfg = tm.getConfig(i);
    TapState  st  = tm.getState(i);

    DynamicJsonDocument doc(256);
    doc["ounces"]   = st.lastPour.ounces;
    doc["beer"]     = cfg.beerName;
    doc["timestamp"]= st.lastPour.timestamp;
    doc["kegLevel"] = st.currentKegLevel;
    doc["duration"] = st.lastPour.duration;
    doc["peakFlow"] = st.lastPour.peakFlowRate;
    String json; serializeJson(doc, json);

    String topic = "beermonitor/tap/" + String(i+1) + "/pour";
    g_mqtt.publish(topic.c_str(), json.c_str());
    LOG_I("MQTT pour event published — Tap %d %.2f oz", i + 1, st.lastPour.ounces);
}

void mqttPublishAllLastPours() {
    if (!g_mqtt.connected()) return;
    TapManager& tm = TapManager::getInstance();
    for (int i = 0; i < NUM_TAPS; i++) {
        TapState st = tm.getState(i);
        if (st.hasLastPour) {
            g_mqtt.publish(("beermonitor/tap/" + String(i+1) + "/last_pour").c_str(),
                           tm.getLastPourJSON(i).c_str(), true);
        }
    }
}

void mqttPublishHADiscovery() {
    // Helper lambda to publish a single sensor discovery payload
    auto pub = [&](const String& topic, const String& payload) {
        g_mqtt.publish(topic.c_str(), payload.c_str(), true);
    };

    for (int i = 0; i < NUM_TAPS; i++) {
        TapConfig cfg = TapManager::getInstance().getConfig(i);
        String base   = "homeassistant/sensor/beermonitor_tap" + String(i+1);
        String avail  = MQTT_TOPIC_AVAILABILITY;
        String tapTopic = "beermonitor/tap/" + String(i+1);

        // Keg Level
        DynamicJsonDocument d(512);
        d["name"]               = cfg.tapName + " Keg Level";
        d["state_topic"]        = tapTopic + "/keg";
        d["unit_of_measurement"]= "oz";
        d["icon"]               = "mdi:keg";
        d["value_template"]     = "{{ value_json.level }}";
        d["availability_topic"] = avail;
        d["unique_id"]          = "beermonitor_tap" + String(i+1) + "_keg";
        String json; serializeJson(d, json);
        pub(base + "_keg/config", json);

        // Last Pour Amount
        d.clear();
        d["name"]               = cfg.tapName + " Last Pour";
        d["state_topic"]        = tapTopic + "/last_pour";
        d["unit_of_measurement"]= "oz";
        d["icon"]               = "mdi:beer";
        d["value_template"]     = "{{ value_json.ounces }}";
        d["availability_topic"] = avail;
        d["unique_id"]          = "beermonitor_tap" + String(i+1) + "_last_pour";
        serializeJson(d, json);
        pub(base + "_last_pour/config", json);
    }

    // Totals
    DynamicJsonDocument d(256);
    d["name"]               = "Total Pours";
    d["state_topic"]        = MQTT_TOPIC_STATS;
    d["icon"]               = "mdi:counter";
    d["value_template"]     = "{{ value_json.total_pours }}";
    d["availability_topic"] = (String)MQTT_TOPIC_AVAILABILITY;
    d["unique_id"]          = "beermonitor_total_pours";
    String json; serializeJson(d, json);
    g_mqtt.publish("homeassistant/sensor/beermonitor_total_pours/config", json.c_str(), true);

    LOG_I("Home Assistant discovery payloads published");
}

// ============================================================
// Webhook
// ============================================================
void sendWebhookAlert(int tapIndex) {
    g_prefs.begin(NVS_NS_SETTINGS, true);
    String url = g_prefs.getString(NVS_KEY_WEBHOOK, "");
    g_prefs.end();
    if (url.length() == 0) return;

    TapManager& tm = TapManager::getInstance();
    TapConfig cfg  = tm.getConfig(tapIndex);
    TapState  st   = tm.getState(tapIndex);

    DynamicJsonDocument doc(256);
    doc["tap"]      = tapIndex + 1;
    doc["tapName"]  = cfg.tapName;
    doc["level"]    = st.currentKegLevel;
    doc["beer"]     = cfg.beerName;
    String json; serializeJson(doc, json);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(json);
    if (code > 0) { LOG_I("Webhook OK: %d", code); }
    else          { LOG_W("Webhook failed: %s", http.errorToString(code).c_str()); }
    http.end();
}

// ============================================================
// Web server
// ============================================================
static void sendJsonResponse(WiFiClient& client, const String& body, int code) {
    client.printf("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
                  code, code == 200 ? "OK" : "Bad Request");
    client.print(body);
}

static void sendOK(WiFiClient& client) {
    sendJsonResponse(client, "{\"success\":true}");
}

static String urlDecode(const String& input) {
    String out;
    out.reserve(input.length());
    char hex[3] = {0};
    for (size_t i = 0; i < input.length(); i++) {
        if (input[i] == '%' && i + 2 < input.length()) {
            hex[0] = input[i+1]; hex[1] = input[i+2];
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (input[i] == '+') {
            out += ' ';
        } else {
            out += input[i];
        }
    }
    return out;
}

// Parse a simple URL-encoded body into key=value pairs
static String bodyParam(const String& body, const String& key) {
    int start = body.indexOf(key + "=");
    if (start < 0) return "";
    start += key.length() + 1;
    int end = body.indexOf('&', start);
    return urlDecode(end < 0 ? body.substring(start) : body.substring(start, end));
}

void handleWebClient(WiFiClient& client) {
    String request;
    request.reserve(512);
    unsigned long deadline = millis() + 500;

    while (client.connected() && millis() < deadline) {
        if (client.available()) {
            char c = client.read();
            request += c;
            if (request.endsWith("\r\n\r\n")) break;
        }
    }

    // Read POST body if present
    String body;
    int bodyStart = request.indexOf("\r\n\r\n");
    if (bodyStart >= 0) {
        bodyStart += 4;
        // Read any remaining bytes
        deadline = millis() + 100;
        while (client.connected() && millis() < deadline && client.available()) {
            body += (char)client.read();
        }
    }

    TapManager& tm = TapManager::getInstance();

    // ---- GET /api/status ----
    if (request.startsWith("GET /api/status")) {
        DynamicJsonDocument doc(4096);
        deserializeJson(doc, tm.getTapStatusJSON()); // reuse existing builder
        doc["mqtt_connected"] = g_mqtt.connected();
        doc["mqtt_server"]    = MQTT_SERVER;
        // Enrich each tap with live diagnostics
        JsonArray taps = doc["taps"].as<JsonArray>();
        for (int i = 0; i < NUM_TAPS && i < (int)taps.size(); i++) {
            const TapDiagnostics& d = DiagnosticsManager::getInstance().getDiag(i);
            taps[i]["sensorHealth"]  = d.healthString();
            taps[i]["pulseQuality"]  = d.pulseQuality;
            taps[i]["daysLeft"]      = d.estimatedDaysLeft;
            taps[i]["sensorWarning"] = DiagnosticsManager::getInstance().hasSensorWarning(i);
        }
        String out; serializeJson(doc, out);
        sendJsonResponse(client, out);
    }
    // ---- GET /api/last_pour ----
    else if (request.startsWith("GET /api/last_pour")) {
        int idx = bodyParam(request.substring(request.indexOf('?') + 1), "index").toInt();
        sendJsonResponse(client, tm.getLastPourJSON(idx));
    }
    // ---- GET /api/history ----
    else if (request.startsWith("GET /api/history")) {
        String qs = request.substring(request.indexOf('?') + 1, request.indexOf(' ', request.indexOf('?')));
        String idxStr = bodyParam(qs, "index");
        if (idxStr == "all") sendJsonResponse(client, tm.getAllHistoryJSON());
        else                 sendJsonResponse(client, tm.getPourHistoryJSON(idxStr.toInt()));
    }
    // ---- POST /api/beer/update ----
    else if (request.startsWith("POST /api/beer/update")) {
        int    idx   = bodyParam(body, "index").toInt();
        String name  = bodyParam(body, "name");
        float  abv   = bodyParam(body, "abv").toFloat();
        int    ibu   = bodyParam(body, "ibu").toInt();
        String color = bodyParam(body, "color");
        tm.setTapInfo(idx, name, abv, ibu, color);
        sendOK(client);
    }
    // ---- POST /api/reset ----
    else if (request.startsWith("POST /api/reset")) {
        int idx = bodyParam(body, "index").toInt();
        tm.resetKeg(idx);
        onKegReset_hooks(idx);
        if (g_mqtt.connected()) {
            g_mqtt.publish(("beermonitor/tap/" + String(idx+1) + "/command").c_str(), "RESET");
        }
        sendOK(client);
    }
    // ---- POST /api/calibrate/start ----
    else if (request.startsWith("POST /api/calibrate/start")) {
        int idx = bodyParam(body, "index").toInt();
        tm.startCalibration(idx);
        sendOK(client);
    }
    // ---- POST /api/calibrate/end ----
    else if (request.startsWith("POST /api/calibrate/end")) {
        int idx = bodyParam(body, "index").toInt();
        float oz = bodyParam(body, "ounces").toFloat();
        bool ok  = tm.endCalibration(idx, oz);
        sendJsonResponse(client, ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"No pulses detected\"}", ok ? 200 : 400);
    }
    // ---- POST /api/alert/clear ----
    else if (request.startsWith("POST /api/alert/clear")) {
        int idx = bodyParam(body, "index").toInt();
        tm.clearAlert(idx);
        sendOK(client);
    }
    // ---- POST /api/settings/alert ----
    else if (request.startsWith("POST /api/settings/alert")) {
        int thresh = bodyParam(body, "threshold").toInt();
        g_prefs.begin(NVS_NS_SETTINGS, false);
        g_prefs.putInt(NVS_KEY_ALERT_THRESH, thresh);
        g_prefs.end();
        sendOK(client);
    }
    // ---- POST /api/settings/webhook ----
    else if (request.startsWith("POST /api/settings/webhook")) {
        String url = bodyParam(body, "url");
        g_prefs.begin(NVS_NS_SETTINGS, false);
        g_prefs.putString(NVS_KEY_WEBHOOK, url);
        g_prefs.end();
        sendOK(client);
    }
    // ---- GET /api/diagnostics/report ----
    else if (request.startsWith("GET /api/diagnostics/report")) {
        sendJsonResponse(client, DiagnosticsManager::getInstance().getSensorReportJSON());
    }
    // ---- GET /api/diagnostics ----
    else if (request.startsWith("GET /api/diagnostics")) {
        sendJsonResponse(client, DiagnosticsManager::getInstance().getAllDiagJSON());
    }
    // ---- GET /api/profiles ----
    else if (request.startsWith("GET /api/profiles")) {
        sendJsonResponse(client, DiagnosticsManager::getInstance().getProfilesJSON());
    }
    // ---- GET /api/notifications/config ----
    else if (request.startsWith("GET /api/notifications/config")) {
        sendJsonResponse(client, NotificationManager::getInstance().getConfigJSON());
    }
    // ---- POST /api/settings/profile ----
    else if (request.startsWith("POST /api/settings/profile")) {
        int tap     = bodyParam(body, "tap").toInt();
        int profile = bodyParam(body, "profile").toInt();
        DiagnosticsManager::getInstance().setKegProfile(tap, profile);
        sendOK(client);
    }
    // ---- POST /api/notifications/config ----
    else if (request.startsWith("POST /api/notifications/config")) {
        bool ok = NotificationManager::getInstance().updateFromJSON(body);
        sendJsonResponse(client, ok ? "{\"success\":true}" : "{\"success\":false}", ok ? 200 : 400);
    }
    // ---- POST /api/notifications/test/slack ----
    else if (request.startsWith("POST /api/notifications/test/slack")) {
        bool ok = NotificationManager::getInstance().testSlack();
        sendJsonResponse(client, ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"Check Slack URL\"}");
    }
    // ---- POST /api/notifications/test/discord ----
    else if (request.startsWith("POST /api/notifications/test/discord")) {
        bool ok = NotificationManager::getInstance().testDiscord();
        sendJsonResponse(client, ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"Check Discord URL\"}");
    }
    // ---- GET /api/files ----
    else if (request.startsWith("GET /api/files")) {
        sendJsonResponse(client, SDManager::getInstance().listFilesJSON());
    }
    // ---- GET /api/storage ----
    else if (request.startsWith("GET /api/storage")) {
        sendJsonResponse(client, SDManager::getInstance().getStatusJSON());
    }
    // ---- GET /api/file?path=... ----
    else if (request.startsWith("GET /api/file?")) {
        String qs = request.substring(request.indexOf('?') + 1, request.indexOf(' ', request.indexOf('?')));
        String path = urlDecode(bodyParam(qs, "path"));
        if (!path.startsWith("/")) path = "/" + path;
        String content = SDManager::getInstance().readFile(path);
        String fname = path.substring(path.lastIndexOf('/') + 1);
        client.print("HTTP/1.1 200 OK\r\nContent-Type: text/csv\r\nConnection: close\r\n"
                     "Content-Disposition: attachment; filename=\"" + fname + "\"\r\n\r\n");
        client.print(content);
    }
    // ---- POST /api/file/delete?path=... ----
    else if (request.startsWith("POST /api/file/delete")) {
        String qs = request.substring(request.indexOf('?') + 1, request.indexOf(' ', request.indexOf('?')));
        String path = urlDecode(bodyParam(qs, "path"));
        if (!path.startsWith("/")) path = "/" + path;
        bool ok = SDManager::getInstance().deleteFile(path);
        sendJsonResponse(client, ok ? "{\"success\":true}" : "{\"success\":false}");
    }
    // ---- GET /menu: chalk beer menu display ----
    else if (request.startsWith("GET /menu")) {
        client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
        client.print(FPSTR(MENU_HTML));
    }
    // ---- Default: serve HTML ----
    else {
        client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
        client.print(FPSTR(INDEX_HTML));
    }

    delay(5);
    client.stop();
}

// ============================================================
// Serial debug REPL
// ============================================================
void handleSerialCommands() {
    if (!Serial.available()) return;
    char cmd = Serial.read();
    TapManager& tm = TapManager::getInstance();

    switch (tolower(cmd)) {
        case 's': tm.printPinStatus(); break;
        case 't':
            for (int i = 0; i < NUM_TAPS; i++) {
                TapState st   = tm.getState(i);
                TapConfig cfg = tm.getConfig(i);
                LOG_I("Tap %d | %s | %s | level=%.1f oz | last=%.2f oz",
                      i+1, st.isPouring ? "POURING" : "idle",
                      cfg.beerName.c_str(), st.currentKegLevel,
                      st.hasLastPour ? st.lastPour.ounces : 0.0f);
            }
            break;
        case 'r':
            for (int i = 0; i < NUM_TAPS; i++) tm.getState(i); // print
            LOG_I("Counters listed (use S for pin status)");
            break;
        case 'm':
            LOG_I("MQTT connected: %s | state: %d",
                  g_mqtt.connected() ? "YES" : "NO", g_mqtt.state());
            break;
        case 'd':
            for (int i = 0; i < NUM_TAPS; i++) {
                const TapDiagnostics& d = DiagnosticsManager::getInstance().getDiag(i);
                LOG_I("Tap %d: %s quality=%.0f%% noise=%lu",
                      i+1, d.healthString(), d.pulseQuality, (unsigned long)d.noisePulses);
            }
            break;
        case 'h':
            Serial.println("Commands: s=pin status  t=tap status  d=diagnostics  m=mqtt  h=help");
            break;
    }
}

// ============================================================
// New manager event hooks
// ============================================================
void onPourComplete_hooks(int tapIndex) {
    TapManager& tm = TapManager::getInstance();
    TapState  st   = tm.getState(tapIndex);
    TapConfig cfg  = tm.getConfig(tapIndex);

    SDManager::getInstance().logPour(
        tapIndex, cfg.tapName, cfg.beerName,
        st.lastPour.ounces, st.lastPour.duration, st.lastPour.peakFlowRate,
        st.currentKegLevel, cfg.kegSize);

    DisplayManager::getInstance().onPourEnd(tapIndex, st.lastPour.ounces, st.lastPour.duration);

    DiagnosticsManager::getInstance().onPourComplete(
        tapIndex, st.lastPour.ounces, st.lastPour.duration, st.lastPour.peakFlowRate);

    NotificationManager::getInstance().notifyPour(
        tapIndex, st.lastPour.ounces, st.lastPour.duration,
        st.lastPour.peakFlowRate, cfg.beerName);

    if (st.currentKegLevel <= 0.0f) {
        NotificationManager::getInstance().notifyKegEmpty(tapIndex, cfg.beerName);
    }

    if (tm.checkLowKegAlert(tapIndex)) {
        NotificationManager::getInstance().notifyLowKeg(
            tapIndex, st.currentKegLevel, cfg.kegSize);
        DisplayManager::getInstance().showLowKegAlert(tapIndex, st.currentKegLevel);
    }

    DiagnosticsManager& dm = DiagnosticsManager::getInstance();
    if (dm.hasSensorWarning(tapIndex)) {
        NotificationManager::getInstance().notifySensorError(
            tapIndex, dm.getSensorWarningMessage(tapIndex));
    }
}

void onKegReset_hooks(int tapIndex) {
    TapConfig cfg = TapManager::getInstance().getConfig(tapIndex);
    NotificationManager::getInstance().notifyKegReset(tapIndex, cfg.beerName);
}

// ============================================================
// setupAllManagers
// ============================================================
void setupAllManagers() {
    SDManager::getInstance().begin();
    DisplayManager::getInstance().begin();
    DiagnosticsManager::getInstance().begin();
    NotificationManager::getInstance().begin();
    if (WiFi.status() == WL_CONNECTED) {
        NotificationManager::getInstance().notifyBoot(WiFi.localIP().toString());
    }
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    LOG_I("=== Beer Flow Monitor v%s booting ===", FIRMWARE_VERSION);

    setupWiFi();

    // GPIO ISR service must be installed before TapManager attaches per-pin ISRs
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        LOG_W("gpio_install_isr_service returned %d", err);
    }

    TapManager::getInstance().begin();
    setupAllManagers();

    if (WiFi.status() == WL_CONNECTED) {
        setupMQTT();
        mqttConnect();
        setupOTA();
        g_server.begin();
        LOG_I("Web server started — http://%s", WiFi.localIP().toString().c_str());
    } else {
        LOG_E("No network — running in offline mode (web + MQTT unavailable)");
    }
}

void loop() {
    TapManager::getInstance().update();
    DisplayManager::getInstance().update();
    DiagnosticsManager::getInstance().update();
    ArduinoOTA.handle();

    unsigned long now = millis();

    // ---- MQTT maintenance ----
    if (!g_mqtt.connected()) {
        if (now - s_lastMqttReconnect > MQTT_RECONNECT_INTERVAL) {
            s_lastMqttReconnect = now;
            mqttConnect();
        }
    } else {
        g_mqtt.loop();

        if (now - s_lastMqttPublish > MQTT_PUBLISH_INTERVAL_MS) {
            mqttPublishTapData();
            s_lastMqttPublish = now;
        }

        if (now - s_lastPourPublish > LAST_POUR_PUBLISH_MS) {
            mqttPublishAllLastPours();
            s_lastPourPublish = now;
        }
    }

    // ---- Detect pour start / completion ----
    int cur = TapManager::getInstance().getActiveTap();
    if (cur >= 0 && s_prevActiveTap == -1) {
        // Pour just started
        DisplayManager::getInstance().onPourStart(cur);
    }
    if (s_prevActiveTap >= 0 && cur == -1) {
        // Pour just finished
        TapState st = TapManager::getInstance().getState(s_prevActiveTap);
        if (st.hasLastPour && st.lastPour.ounces > 0.0f) {
            if (g_mqtt.connected()) mqttPublishPourEvent(s_prevActiveTap);
            onPourComplete_hooks(s_prevActiveTap);
            sendWebhookAlert(s_prevActiveTap);
        }
    }
    s_prevActiveTap = cur;

    // ---- Web requests ----
    WiFiClient client = g_server.accept();
    if (client) {
        handleWebClient(client);
    }

    // ---- Periodic debug print ----
    if (now - s_lastStatusPrint > STATUS_PRINT_INTERVAL) {
        TapManager::getInstance().printPinStatus();
        s_lastStatusPrint = now;
    }

    handleSerialCommands();

    delay(5);
}