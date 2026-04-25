// ============================================================
// TapManager additions — add these to TapManager.h and .cpp
// ============================================================

// --- In TapManager.h, add to public section: ---
//   void setKegSize(int tapIndex, float sizeOz);

// --- In TapManager.cpp, add: ---
//
// void TapManager::setKegSize(int i, float sizeOz) {
//     if (!validIndex(i)) return;
//     m_config[i].kegSize = sizeOz;
//     // Optionally reset level to new full size:
//     // m_state[i].currentKegLevel = sizeOz;
//     saveConfig();
//     LOG_I("Tap %d keg size set to %.0f oz", i + 1, sizeOz);
// }

// --- In saveConfig() / loadConfig(), add to the NVS block: ---
//
//   snprintf(key, sizeof(key), "keg_size_%d", i);
//   prefs.putFloat(key, m_config[i].kegSize);
//   // and in load:
//   m_config[i].kegSize = prefs.getFloat(key, KEG_SIZE_OZ);

// ============================================================
// Updated TapStatus JSON — add these fields per tap in getTapStatusJSON():
// ============================================================
//
//   t["lowKegAlert"]   = checkLowKegAlertReadOnly(i);   // non-mutating check
//   t["sensorHealth"]  = DiagnosticsManager::getInstance().getDiag(i).healthString();
//   t["daysLeft"]      = DiagnosticsManager::getInstance().getDiag(i).estimatedDaysLeft;
//   t["sensorWarning"] = DiagnosticsManager::getInstance().hasSensorWarning(i);
