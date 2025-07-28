#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <time.h>
#include <esp_adc_cal.h>
#include <Preferences.h>
#include "pin_config.h"

// ================================
// CONFIGURATION & CONSTANTS
// ================================

// Hardware pins
#define BUTTON_PIN      14
#define PIN_BAT_VOLT    4

// Timing constants
const unsigned long DEBOUNCE_DELAY    = 200;
const unsigned long BATTERY_UPDATE    = 5000;   // 5 seconds
const unsigned long WIFI_TIMEOUT      = 15000;  // 15 seconds

// Display constants
#define STATUS_H        28
#define LEFT_MARGIN     5
#define CORNER_RADIUS   4

// Network constants
#define DNS_PORT        53
const char* AP_SSID     = "ESP32_Setup";

// Battery calibration
const float R_DIV       = 2.0f;

// NTP settings
const char* ntpServer   = "pool.ntp.org";
const long  gmtOffset   = -5 * 3600;  // EST
const int   dstOffset   = 3600;       // DST

// ================================
// DATA STRUCTURES
// ================================

struct Period {
    int hour;
    int minute;
    const char* name;
    
    Period(int h, int m, const char* n = "") : hour(h), minute(m), name(n) {}
    
    unsigned long toSeconds() const {
        return hour * 3600UL + minute * 60UL;
    }
};

struct Date {
    int year;
    int month;
    int day;
    
    Date(int y, int m, int d) : year(y), month(m), day(d) {}
    
    time_t toTimeT() const {
        struct tm tm = {0};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        return mktime(&tm);
    }
};

struct Config {
    String ssid;
    String password;
    String deviceName;
    String bgColor;
    String accentColor;
    String textColor;
    String cardBg1;
    String cardBg2;
    String cardBg3;
    int sleepTimeout;
    int pageInterval;
    int brightness;
    bool autoPageCycle;
    bool show24Hour;
    bool showSeconds;
    String timezone;
    
    Config() : 
        deviceName("ESP32"), 
        bgColor("#222222"), 
        accentColor("#00ccff"),
        textColor("#ffffff"),
        cardBg1("#080815"),
        cardBg2("#051420"),
        cardBg3("#0c140c"),
        sleepTimeout(60),
        pageInterval(10),
        brightness(255),
        autoPageCycle(true),
        show24Hour(false),
        showSeconds(true),
        timezone("EST") {}
};

// ================================
// GLOBAL VARIABLES
// ================================

// Hardware objects
TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
static esp_adc_cal_characteristics_t adc_chars;

// State management
volatile bool buttonPressed = false;
unsigned long lastButtonPress = 0;
unsigned long lastActivity = 0;
unsigned long lastPageSwitch = 0;
unsigned long lastBatteryUpdate = 0;
unsigned long lastClockUpdate = 0;

bool screenOn = true;
uint8_t currentPage = 0;
uint8_t lastPage = 255;
uint32_t batteryVoltage = 0;

// Configuration
Config config;

// School schedule
const Period schedule[] = {
    {7, 50, "Period 1"},
    {8, 54, "Period 2"},
    {9, 50, "Period 3"},
    {10, 44, "Period 4"},
    {10, 44, "Lunch"},
    {12, 12, "Period 5"},
    {13, 6, "Period 6"},
    {14, 0, "Period 7"}
};

const Period lunchTime(10, 44, "Lunch");
const Period endTime(14, 50, "End of Day");
const int periodCount = sizeof(schedule) / sizeof(schedule[0]);

// School year dates
const Date schoolStart(2025, 8, 11);
const Date schoolEnd(2026, 5, 28);
time_t schoolStartTime, schoolEndTime;
unsigned long totalSchoolDays;

// ================================
// UTILITY FUNCTIONS
// ================================

void IRAM_ATTR buttonISR() {
    buttonPressed = true;
}

unsigned long getCurrentSeconds() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_hour * 3600UL + timeinfo.tm_min * 60UL + timeinfo.tm_sec;
}

String formatDuration(long seconds) {
    if (seconds < 0) seconds = 0;
    
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    
    char buffer[16];
    if (hours > 0) {
        snprintf(buffer, sizeof(buffer), "%dh %02dm", hours, minutes);
    } else {
        snprintf(buffer, sizeof(buffer), "%dm", minutes);
    }
    return String(buffer);
}

uint32_t hexToColor565(String hex) {
    if (hex.startsWith("#")) hex = hex.substring(1);
    if (hex.length() != 6) return TFT_WHITE;
    
    long rgb = strtol(hex.c_str(), NULL, 16);
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    
    return tft.color565(r, g, b);
}

void format12Hour(char* buffer, int hour, int minute, int second) {
    if (config.show24Hour) {
        if (config.showSeconds) {
            snprintf(buffer, 24, "%02d:%02d:%02d", hour, minute, second);
        } else {
            snprintf(buffer, 24, "%02d:%02d", hour, minute);
        }
    } else {
        const char* ampm = (hour >= 12) ? "PM" : "AM";
        if (hour == 0) hour = 12;
        else if (hour > 12) hour -= 12;
        
        if (config.showSeconds) {
            snprintf(buffer, 24, "%d:%02d:%02d %s", hour, minute, second, ampm);
        } else {
            snprintf(buffer, 24, "%d:%02d %s", hour, minute, ampm);
        }
    }
}

bool isSchoolDay() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    // Monday = 1, Sunday = 0
    return (timeinfo.tm_wday >= 1 && timeinfo.tm_wday <= 5);
}

// ================================
// BATTERY MANAGEMENT
// ================================

void updateBatteryReading() {
    if (millis() - lastBatteryUpdate < BATTERY_UPDATE) return;
    
    uint32_t total = 0;
    for (int i = 0; i < 10; i++) {
        total += analogRead(PIN_BAT_VOLT);
        delay(1);
    }
    
    batteryVoltage = esp_adc_cal_raw_to_voltage(total / 10, &adc_chars) * R_DIV;
    lastBatteryUpdate = millis();
}

float getBatteryPercentage() {
    if (batteryVoltage > 4200) return 1.0f;
    else if (batteryVoltage > 4000) return (batteryVoltage - 3700) / 500.0f;
    else if (batteryVoltage > 3700) return (batteryVoltage - 3700) / 300.0f;
    else return 0.0f;
}

uint32_t getBatteryColor() {
    if (batteryVoltage > 4000) return TFT_GREEN;
    else if (batteryVoltage > 3700) return TFT_YELLOW;
    else return TFT_RED;
}

// ================================
// DISPLAY FUNCTIONS
// ================================

void drawProgressBar(int x, int y, int w, int h, float progress, uint32_t color, const char* label = nullptr) {
    // Clamp progress
    progress = constrain(progress, 0.0f, 1.0f);
    
    // Draw background and border
    tft.fillRoundRect(x, y, w, h, 2, TFT_DARKGREY);
    tft.drawRoundRect(x, y, w, h, 2, TFT_WHITE);
    
    // Draw fill
    int fillWidth = (int)((w - 2) * progress);
    if (fillWidth > 0) {
        tft.fillRoundRect(x + 1, y + 1, fillWidth, h - 2, 1, color);
    }
    
    // Draw label
    if (label) {
        tft.setTextSize(1);
        tft.setTextColor(TFT_WHITE);
        int textWidth = tft.textWidth(label);
        tft.setCursor(x + (w - textWidth) / 2, y + (h - 8) / 2);
        tft.print(label);
    }
}

void drawBatteryIcon(int x, int y) {
    updateBatteryReading();
    
    float percentage = getBatteryPercentage();
    uint32_t color = getBatteryColor();
    
    // Draw battery outline
    tft.drawRoundRect(x, y, 18, 8, 1, TFT_WHITE);
    tft.fillRect(x + 18, y + 2, 2, 4, TFT_WHITE);  // Battery tip
    
    // Draw battery fill
    int fillWidth = (int)(16 * percentage);
    if (fillWidth > 0) {
        tft.fillRoundRect(x + 1, y + 1, fillWidth, 6, 1, color);
    }
    
    // Draw voltage text
    char voltageText[12];
    if (batteryVoltage > 4500) {
        strcpy(voltageText, "USB");
    } else {
        snprintf(voltageText, sizeof(voltageText), "%.1fV", batteryVoltage / 1000.0f);
    }
    
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(x + 22, y);
    tft.print(voltageText);
}

void drawWiFiStrength(int x, int y) {
    int32_t rssi = WiFi.RSSI();
    int bars = 0;
    
    if (rssi > -50) bars = 4;
    else if (rssi > -60) bars = 3;
    else if (rssi > -70) bars = 2;
    else if (rssi > -80) bars = 1;
    
    for (int i = 0; i < 4; i++) {
        int barX = x + i * 6;
        int barHeight = 3 + i * 2;
        int barY = y - barHeight;
        uint32_t barColor = (i < bars) ? TFT_CYAN : TFT_DARKGREY;
        
        tft.fillRoundRect(barX, barY, 4, barHeight, 1, barColor);
    }
    
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(x + 26, y - 7);
    tft.print("WiFi");
}

void drawStatusBar() {
    // Draw gradient background
    for (int i = 0; i < STATUS_H; i++) {
        uint32_t shade = tft.color565(15 + i / 2, 15 + i / 2, 30 + i);
        tft.drawFastHLine(0, i, tft.width(), shade);
    }
    
    // Draw WiFi indicator
    drawWiFiStrength(LEFT_MARGIN, STATUS_H - 3);
    
    // Draw battery indicator
    drawBatteryIcon(tft.width() - 58, STATUS_H - 12);
}

void drawDeviceName() {
    int nameLength = config.deviceName.length();
    int charHeight = 24;
    int startY = STATUS_H + 8;
    
    tft.setTextSize(3);
    
    for (int i = 0; i < nameLength && i < 10; i++) {
        char character = config.deviceName.charAt(i);
        int charY = startY + i * charHeight;
        
        // Draw shadow
        tft.setTextColor(TFT_DARKGREY);
        tft.setCursor(LEFT_MARGIN + 1, charY + 1);
        tft.print(character);
        
        // Draw main character
        tft.setTextColor(TFT_GREEN);
        tft.setCursor(LEFT_MARGIN, charY);
        tft.print(character);
    }
}

void drawCard(int x, int y, int w, int h, uint32_t backgroundColor) {
    tft.fillRoundRect(x, y, w, h, CORNER_RADIUS, backgroundColor);
    tft.drawRoundRect(x, y, w, h, CORNER_RADIUS, TFT_DARKGREY);
}

// ================================
// PAGE RENDERING
// ================================

void drawSchedulePage() {
    int cardX = LEFT_MARGIN + 40;
    int cardY = STATUS_H + 12;
    int cardW = tft.width() - cardX - LEFT_MARGIN;
    int cardH = tft.height() - cardY - 5;
    
    drawCard(cardX, cardY, cardW, cardH, hexToColor565(config.cardBg1));
    
    unsigned long currentSeconds = getCurrentSeconds();
    int yPosition = cardY + 10;
    int itemHeight = (cardH - 25) / 3;
    
    // Find next period
    unsigned long nextPeriodTime = 0;
    for (int i = 0; i < periodCount; i++) {
        unsigned long periodTime = schedule[i].toSeconds();
        if (periodTime > currentSeconds) {
            nextPeriodTime = periodTime;
            break;
        }
    }
    
    // If no period found today, use first period of next day
    if (nextPeriodTime == 0) {
        nextPeriodTime = schedule[0].toSeconds() + 86400;
    }
    
    // Next period countdown
    long timeToNext = nextPeriodTime - currentSeconds;
    tft.setTextSize(1);
    tft.setTextColor(TFT_YELLOW);
    tft.setCursor(cardX + 8, yPosition);
    tft.print("Next: ");
    tft.print(formatDuration(timeToNext));
    
    float nextProgress = 1.0f - (float)timeToNext / 86400.0f;
    char nextLabel[12];
    snprintf(nextLabel, sizeof(nextLabel), "%.0f%%", nextProgress * 100);
    drawProgressBar(cardX + 8, yPosition + 14, cardW - 16, 14, nextProgress, hexToColor565(config.accentColor), nextLabel);
    yPosition += itemHeight;
    
    // Lunch countdown
    unsigned long lunchTimeSeconds = lunchTime.toSeconds();
    if (lunchTimeSeconds <= currentSeconds) lunchTimeSeconds += 86400;
    long timeToLunch = lunchTimeSeconds - currentSeconds;
    
    tft.setTextColor(TFT_MAGENTA);
    tft.setCursor(cardX + 8, yPosition);
    tft.print("Lunch: ");
    tft.print(formatDuration(timeToLunch));
    
    float lunchProgress = 1.0f - (float)timeToLunch / 86400.0f;
    char lunchLabel[12];
    snprintf(lunchLabel, sizeof(lunchLabel), "%.0f%%", lunchProgress * 100);
    drawProgressBar(cardX + 8, yPosition + 14, cardW - 16, 14, lunchProgress, TFT_MAGENTA, lunchLabel);
    yPosition += itemHeight;
    
    // End of day countdown
    unsigned long endTimeSeconds = endTime.toSeconds();
    if (endTimeSeconds <= currentSeconds) endTimeSeconds += 86400;
    long timeToEnd = endTimeSeconds - currentSeconds;
    
    tft.setTextColor(TFT_ORANGE);
    tft.setCursor(cardX + 8, yPosition);
    tft.print("End: ");
    tft.print(formatDuration(timeToEnd));
    
    float endProgress = 1.0f - (float)timeToEnd / 86400.0f;
    char endLabel[12];
    snprintf(endLabel, sizeof(endLabel), "%.0f%%", endProgress * 100);
    drawProgressBar(cardX + 8, yPosition + 14, cardW - 16, 14, endProgress, TFT_ORANGE, endLabel);
}

void drawClockPage() {
    int cardX = LEFT_MARGIN + 40;
    int cardY = STATUS_H + 12;
    int cardW = tft.width() - cardX - LEFT_MARGIN;
    int cardH = tft.height() - cardY - 5;
    
    drawCard(cardX, cardY, cardW, cardH, hexToColor565(config.cardBg2));
    
    // Get current time
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    // Draw time
    char timeString[24];
    format12Hour(timeString, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    
    tft.setTextSize(4);
    int timeWidth = tft.textWidth(timeString);
    int timeX = cardX + (cardW - timeWidth) / 2;
    int timeY = cardY + 25;
    
    // Draw time shadow
    tft.setTextColor(hexToColor565(config.accentColor));
    tft.setCursor(timeX + 1, timeY + 1);
    tft.print(timeString);
    
    // Draw main time
    tft.setTextColor(hexToColor565(config.textColor));
    tft.setCursor(timeX, timeY);
    tft.print(timeString);
    
    // Draw date
    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char dateString[24];
    snprintf(dateString, sizeof(dateString), "%s %d, %d", 
             months[timeinfo.tm_mon], timeinfo.tm_mday, timeinfo.tm_year + 1900);
    
    tft.setTextSize(2);
    tft.setTextColor(hexToColor565(config.accentColor));
    int dateWidth = tft.textWidth(dateString);
    tft.setCursor(cardX + (cardW - dateWidth) / 2, timeY + 58);
    tft.print(dateString);
}

void drawSchoolProgressPage() {
    int cardX = LEFT_MARGIN + 40;
    int cardY = STATUS_H + 12;
    int cardW = tft.width() - cardX - LEFT_MARGIN;
    int cardH = tft.height() - cardY - 5;
    
    drawCard(cardX, cardY, cardW, cardH, hexToColor565(config.cardBg3));
    
    // Calculate days remaining
    time_t now = time(nullptr);
    long daysLeft = (schoolEndTime - now) / 86400;
    if (daysLeft < 0) daysLeft = 0;
    
    // Calculate progress
    float progress = 1.0f - (float)daysLeft / (float)totalSchoolDays;
    progress = constrain(progress, 0.0f, 1.0f);
    
    // Draw title
    tft.setTextSize(2);
    tft.setTextColor(TFT_GREEN);
    tft.setCursor(cardX + 8, cardY + 15);
    tft.print("Days Left");
    
    // Draw days count
    char daysString[16];
    snprintf(daysString, sizeof(daysString), "%ld", daysLeft);
    
    tft.setTextSize(4);
    tft.setTextColor(hexToColor565(config.textColor));
    int daysWidth = tft.textWidth(daysString);
    tft.setCursor(cardX + (cardW - daysWidth) / 2, cardY + 40);
    tft.print(daysString);
    
    // Draw progress bar
    char progressLabel[16];
    snprintf(progressLabel, sizeof(progressLabel), "%.1f%%", progress * 100);
    drawProgressBar(cardX + 8, cardY + 75, cardW - 16, 16, progress, TFT_GREEN, progressLabel);
}

// ================================
// CONFIGURATION & NETWORKING
// ================================

void loadConfiguration() {
    prefs.begin("cfg", true);
    config.ssid = prefs.getString("ssid", "");
    config.password = prefs.getString("pass", "");
    config.deviceName = prefs.getString("name", config.deviceName);
    config.bgColor = prefs.getString("bg", config.bgColor);
    config.accentColor = prefs.getString("ac", config.accentColor);
    config.textColor = prefs.getString("tc", config.textColor);
    config.cardBg1 = prefs.getString("cb1", config.cardBg1);
    config.cardBg2 = prefs.getString("cb2", config.cardBg2);
    config.cardBg3 = prefs.getString("cb3", config.cardBg3);
    config.sleepTimeout = prefs.getInt("sleep", config.sleepTimeout);
    config.pageInterval = prefs.getInt("page", config.pageInterval);
    config.brightness = prefs.getInt("bright", config.brightness);
    config.autoPageCycle = prefs.getBool("auto", config.autoPageCycle);
    config.show24Hour = prefs.getBool("24h", config.show24Hour);
    config.showSeconds = prefs.getBool("sec", config.showSeconds);
    config.timezone = prefs.getString("tz", config.timezone);
    prefs.end();
}

void saveConfiguration() {
    prefs.begin("cfg", false);
    prefs.putString("ssid", config.ssid);
    prefs.putString("pass", config.password);
    prefs.putString("name", config.deviceName);
    prefs.putString("bg", config.bgColor);
    prefs.putString("ac", config.accentColor);
    prefs.putString("tc", config.textColor);
    prefs.putString("cb1", config.cardBg1);
    prefs.putString("cb2", config.cardBg2);
    prefs.putString("cb3", config.cardBg3);
    prefs.putInt("sleep", config.sleepTimeout);
    prefs.putInt("page", config.pageInterval);
    prefs.putInt("bright", config.brightness);
    prefs.putBool("auto", config.autoPageCycle);
    prefs.putBool("24h", config.show24Hour);
    prefs.putBool("sec", config.showSeconds);
    prefs.putString("tz", config.timezone);
    prefs.end();
}

// Setup web server
void setupWebServer() {
    server.on("/", HTTP_GET, []() {
        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>ESP32 Setup</title>
    <style>
        * { box-sizing: border-box; }
        body { 
            background: __BG__; 
            color: #fff; 
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; 
            padding: 1em; 
            margin: 0;
            min-height: 100vh;
        }
        .container { 
            max-width: 500px; 
            margin: 0 auto;
            background: rgba(255,255,255,0.1);
            padding: 2em;
            border-radius: 12px;
            backdrop-filter: blur(10px);
        }
        h1 { 
            color: __AC__; 
            text-align: center; 
            margin-bottom: 1.5em;
        }
        .section {
            margin-bottom: 2em;
            padding: 1em;
            background: rgba(255,255,255,0.05);
            border-radius: 8px;
        }
        .section h3 {
            color: __AC__;
            margin-top: 0;
            margin-bottom: 1em;
        }
        .form-group { 
            margin-bottom: 1em; 
        }
        .form-row {
            display: flex;
            gap: 1em;
        }
        .form-row .form-group {
            flex: 1;
        }
        label { 
            display: block; 
            margin-bottom: 0.5em; 
            font-weight: 500;
            font-size: 0.9em;
        }
        input, select { 
            width: 100%; 
            padding: 0.75em; 
            border: 1px solid rgba(255,255,255,0.3); 
            border-radius: 4px; 
            background: rgba(255,255,255,0.1);
            color: #fff;
            font-size: 16px;
        }
        input:focus, select:focus {
            outline: none;
            border-color: __AC__;
            box-shadow: 0 0 0 2px rgba(0,204,255,0.2);
        }
        .color-input {
            height: 50px;
        }
        .checkbox-group {
            display: flex;
            align-items: center;
            gap: 0.5em;
        }
        .checkbox-group input[type="checkbox"] {
            width: auto;
        }
        button { 
            width: 100%; 
            padding: 1em; 
            background: __AC__; 
            border: none; 
            color: #fff; 
            font-weight: bold; 
            border-radius: 8px;
            font-size: 16px;
            cursor: pointer;
            transition: all 0.2s;
            margin-top: 1em;
        }
        button:hover {
            opacity: 0.9;
            transform: translateY(-2px);
        }
        .range-input {
            display: flex;
            align-items: center;
            gap: 1em;
        }
        .range-input input[type="range"] {
            flex: 1;
        }
        .range-value {
            min-width: 3em;
            text-align: center;
            background: rgba(255,255,255,0.1);
            padding: 0.5em;
            border-radius: 4px;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>__NAME__ Configuration</h1>
        <form action="/save" method="post">
            
            <div class="section">
                <h3>📶 Network Settings</h3>
                <div class="form-group">
                    <label>WiFi Network:</label>
                    <input name="ssid" type="text" required placeholder="Enter WiFi name">
                </div>
                <div class="form-group">
                    <label>WiFi Password:</label>
                    <input name="pass" type="password" placeholder="Enter WiFi password">
                </div>
            </div>

            <div class="section">
                <h3>🎨 Appearance</h3>
                <div class="form-group">
                    <label>Device Name:</label>
                    <input name="devname" type="text" value="__NAME__" placeholder="Enter device name">
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label>Background:</label>
                        <input name="bg" type="color" value="__BG__" class="color-input">
                    </div>
                    <div class="form-group">
                        <label>Accent Color:</label>
                        <input name="ac" type="color" value="__AC__" class="color-input">
                    </div>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label>Text Color:</label>
                        <input name="tc" type="color" value="__TC__" class="color-input">
                    </div>
                    <div class="form-group">
                        <label>Brightness:</label>
                        <div class="range-input">
                            <input name="bright" type="range" min="50" max="255" value="__BRIGHT__">
                            <span class="range-value" id="brightValue">__BRIGHT__</span>
                        </div>
                    </div>
                </div>
            </div>

            <div class="section">
                <h3>🃏 Card Backgrounds</h3>
                <div class="form-row">
                    <div class="form-group">
                        <label>Schedule Card:</label>
                        <input name="cb1" type="color" value="__CB1__" class="color-input">
                    </div>
                    <div class="form-group">
                        <label>Clock Card:</label>
                        <input name="cb2" type="color" value="__CB2__" class="color-input">
                    </div>
                </div>
                <div class="form-group">
                    <label>Progress Card:</label>
                    <input name="cb3" type="color" value="__CB3__" class="color-input">
                </div>
            </div>

            <div class="section">
                <h3>⚙️ Behavior Settings</h3>
                <div class="form-row">
                    <div class="form-group">
                        <label>Sleep Timeout (minutes):</label>
                        <div class="range-input">
                            <input name="sleep" type="range" min="1" max="60" value="__SLEEP__">
                            <span class="range-value" id="sleepValue">__SLEEP__</span>
                        </div>
                    </div>
                    <div class="form-group">
                        <label>Page Interval (seconds):</label>
                        <div class="range-input">
                            <input name="page" type="range" min="5" max="30" value="__PAGE__">
                            <span class="range-value" id="pageValue">__PAGE__</span>
                        </div>
                    </div>
                </div>
                <div class="checkbox-group">
                    <input name="auto" type="checkbox" id="auto" __AUTO_CHECKED__>
                    <label for="auto">Enable automatic page cycling</label>
                </div>
            </div>

            <div class="section">
                <h3>🕐 Time Display</h3>
                <div class="form-row">
                    <div class="checkbox-group">
                        <input name="24h" type="checkbox" id="24h" __24H_CHECKED__>
                        <label for="24h">24-hour format</label>
                    </div>
                    <div class="checkbox-group">
                        <input name="sec" type="checkbox" id="sec" __SEC_CHECKED__>
                        <label for="sec">Show seconds</label>
                    </div>
                </div>
                <div class="form-group">
                    <label>Timezone:</label>
                    <select name="tz">
                        <option value="EST" __TZ_EST__>Eastern Time (EST/EDT)</option>
                        <option value="CST" __TZ_CST__>Central Time (CST/CDT)</option>
                        <option value="MST" __TZ_MST__>Mountain Time (MST/MDT)</option>
                        <option value="PST" __TZ_PST__>Pacific Time (PST/PDT)</option>
                    </select>
                </div>
            </div>

            <button type="submit">💾 Save Configuration</button>
        </form>
    </div>

    <script>
        // Update range value displays
        document.querySelector('input[name="bright"]').oninput = function() {
            document.getElementById('brightValue').textContent = this.value;
        }
        document.querySelector('input[name="sleep"]').oninput = function() {
            document.getElementById('sleepValue').textContent = this.value;
        }
        document.querySelector('input[name="page"]').oninput = function() {
            document.getElementById('pageValue').textContent = this.value;
        }
    </script>
</body>
</html>
        )rawliteral";
        
        html.replace("__NAME__", config.deviceName);
        html.replace("__BG__", config.bgColor);
        html.replace("__AC__", config.accentColor);
        html.replace("__TC__", config.textColor);
        html.replace("__CB1__", config.cardBg1);
        html.replace("__CB2__", config.cardBg2);
        html.replace("__CB3__", config.cardBg3);
        html.replace("__BRIGHT__", String(config.brightness));
        html.replace("__SLEEP__", String(config.sleepTimeout));
        html.replace("__PAGE__", String(config.pageInterval));
        html.replace("__AUTO_CHECKED__", config.autoPageCycle ? "checked" : "");
        html.replace("__24H_CHECKED__", config.show24Hour ? "checked" : "");
        html.replace("__SEC_CHECKED__", config.showSeconds ? "checked" : "");
        html.replace("__TZ_EST__", config.timezone == "EST" ? "selected" : "");
        html.replace("__TZ_CST__", config.timezone == "CST" ? "selected" : "");
        html.replace("__TZ_MST__", config.timezone == "MST" ? "selected" : "");
        html.replace("__TZ_PST__", config.timezone == "PST" ? "selected" : "");
        
        server.send(200, "text/html", html);
    });
    
    server.on("/save", HTTP_POST, []() {
        config.ssid = server.arg("ssid");
        config.password = server.arg("pass");
        config.deviceName = server.arg("devname");
        config.bgColor = server.arg("bg");
        config.accentColor = server.arg("ac");
        config.textColor = server.arg("tc");
        config.cardBg1 = server.arg("cb1");
        config.cardBg2 = server.arg("cb2");
        config.cardBg3 = server.arg("cb3");
        config.sleepTimeout = server.arg("sleep").toInt();
        config.pageInterval = server.arg("page").toInt();
        config.brightness = server.arg("bright").toInt();
        config.autoPageCycle = server.arg("auto") == "on";
        config.show24Hour = server.arg("24h") == "on";
        config.showSeconds = server.arg("sec") == "on";
        config.timezone = server.arg("tz");
        
        saveConfiguration();
        
        server.send(200, "text/html", 
            "<html><body style='background:" + config.bgColor + ";color:#fff;font-family:sans-serif;text-align:center;padding:2em;'>"
            "<h1 style='color:" + config.accentColor + ";'>Configuration Saved!</h1>"
            "<p>Device will restart in 3 seconds...</p>"
            "<div style='width:200px;height:4px;background:rgba(255,255,255,0.2);margin:1em auto;border-radius:2px;overflow:hidden;'>"
            "<div style='width:0;height:100%;background:" + config.accentColor + ";animation:load 3s linear forwards;'></div></div>"
            "<style>@keyframes load{to{width:100%}}</style>"
            "</body></html>");
        
        delay(3000);
        ESP.restart();
    });
}

void startConfigurationPortal() {
    WiFi.softAP(AP_SSID);
    IPAddress apIP = WiFi.softAPIP();
    dnsServer.start(DNS_PORT, "*", apIP);
    
    // Show setup screen
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(10, 40);
    tft.print("Setup Mode");
    tft.setCursor(10, 70);
    tft.print("Connect to WiFi:");
    tft.setTextSize(3);
    tft.setTextColor(TFT_CYAN);
    tft.setCursor(10, 100);
    tft.print(AP_SSID);
    tft.setCursor(10, 130);
    tft.print(apIP.toString());
    
    setupWebServer();
    server.begin();
    
    // Handle portal requests
    while (true) {
        dnsServer.processNextRequest();
        server.handleClient();
        delay(1);
    }
}

bool connectToWiFi() {
    if (config.ssid.length() == 0) {
        return false;
    }
    
    WiFi.begin(config.ssid.c_str(), config.password.c_str());
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_TIMEOUT) {
        delay(500);
        Serial.print(".");
    }
    
    return WiFi.status() == WL_CONNECTED;
}

// ================================
// MAIN FUNCTIONS
// ================================

void setup() {
    // Initialize power management
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
    
    // Initialize serial communication
    Serial.begin(115200);
    Serial.println("ESP32 School Display Starting...");
    
    // Initialize button interrupt
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
    
    // Initialize display
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    
    // Initialize ADC for battery monitoring
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_BAT_VOLT, ADC_11db);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
    
    // Calculate school year parameters
    schoolStartTime = schoolStart.toTimeT();
    schoolEndTime = schoolEnd.toTimeT();
    totalSchoolDays = (schoolEndTime - schoolStartTime) / 86400UL;
    
    // Load configuration
    loadConfiguration();
    
    // Connect to WiFi or start configuration portal
    if (!connectToWiFi()) {
        Serial.println("Starting configuration portal...");
        startConfigurationPortal();
    }
    
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    // Synchronize time
    configTime(gmtOffset, dstOffset, ntpServer);
    Serial.println("Waiting for time synchronization...");
    
    // Wait for time sync with timeout
    unsigned long timeStart = millis();
    while (time(nullptr) < 100000 && millis() - timeStart < 10000) {
        delay(100);
    }
    
    if (time(nullptr) > 100000) {
        Serial.println("Time synchronized successfully");
    } else {
        Serial.println("Warning: Time synchronization failed");
    }
    
    // Initialize timing variables
    lastActivity = millis();
    lastPageSwitch = millis();
    
    Serial.println("Setup complete!");
}

void loop() {
    unsigned long currentTime = millis();
    
    // Handle button press for wake-up ONLY
    if (buttonPressed && currentTime - lastButtonPress > DEBOUNCE_DELAY) {
        buttonPressed = false;
        lastButtonPress = currentTime;
        
        if (!screenOn) {
            // Wake up the screen
            screenOn = true;
            digitalWrite(PIN_POWER_ON, HIGH);
            tft.init();
            tft.setRotation(1);
            tft.fillScreen(hexToColor565(config.bgColor));
            lastPage = 255;  // Force full redraw
            lastActivity = currentTime;
        } else {
            // Just reset the activity timer when screen is on
            lastActivity = currentTime;
        }
    }
    
    // Handle auto-sleep with configurable timeout
    unsigned long sleepTimeoutMs = config.sleepTimeout * 60000UL;  // Convert minutes to milliseconds
    if (screenOn && currentTime - lastActivity >= sleepTimeoutMs) {
        tft.fillScreen(TFT_BLACK);
        digitalWrite(PIN_POWER_ON, LOW);
        screenOn = false;
    }
    
    if (!screenOn) {
        delay(100);
        return;
    }
    
    // Handle automatic page cycling (only if enabled)
    bool pageChanged = false;
    if (config.autoPageCycle && currentTime - lastPageSwitch >= (config.pageInterval * 1000UL)) {
        currentPage = (currentPage + 1) % 3;
        lastPageSwitch = currentTime;
        pageChanged = true;
    }
    
    // Determine if we need to update the display
    bool needsUpdate = false;
    
    if (pageChanged || lastPage != currentPage) {
        // Full page change - clear screen
        if (pageChanged || lastPage != currentPage) {
            tft.fillScreen(hexToColor565(config.bgColor));
            lastPage = currentPage;
        }
        needsUpdate = true;
    } else if (currentPage == 1 && currentTime - lastClockUpdate > 1000) {
        // Clock page needs regular updates
        needsUpdate = true;
        lastClockUpdate = currentTime;
    }
    
    // Update display if needed
    if (needsUpdate) {
        // Set display brightness
        analogWrite(TFT_BL, config.brightness);
        
        // Always draw status bar and device name
        drawStatusBar();
        drawDeviceName();
        
        // Draw the current page content
        switch (currentPage) {
            case 0:
                drawSchedulePage();
                break;
            case 1:
                drawClockPage();
                break;
            case 2:
                drawSchoolProgressPage();
                break;
        }
    }
    
    // Small delay to prevent excessive CPU usage
    delay(50);
}
