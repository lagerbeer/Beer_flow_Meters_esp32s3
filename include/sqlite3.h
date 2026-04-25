#include "sqlite3.h"

// Execute a SQL command
int db_exec(sqlite3 *db, const char *sql) {
    char *zErrMsg = 0;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK) {
        Serial.printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }
    return rc;
}

void initDatabase() {
    if (sqlite3_open("/littlefs/kegerator.db", &db)) {
        Serial.println("Can't open database");
        return;
    }

    // Table for history
    db_exec(db, "CREATE TABLE IF NOT EXISTS pour_history ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "tap_id INTEGER,"
                "ounces REAL,"
                "timestamp DATETIME DEFAULT (datetime('now','localtime')))");

    // Table for tap state (persistent settings)
    db_exec(db, "CREATE TABLE IF NOT EXISTS tap_settings ("
                "tap_id INTEGER PRIMARY KEY,"
                "remaining_oz REAL,"
                "pulses_per_oz REAL)");
}

void recordPour(int tapId, float ounces) {
    char sql[128];
    sprintf(sql, "INSERT INTO pour_history (tap_id, ounces) VALUES (%d, %.2f);", tapId, ounces);
    db_exec(db, sql);
    
    // Update the remaining volume in the settings table
    sprintf(sql, "UPDATE tap_settings SET remaining_oz = remaining_oz - %.2f WHERE tap_id = %d;", ounces, tapId);
    db_exec(db, sql);
}