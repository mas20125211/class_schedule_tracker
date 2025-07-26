#include <TFT_eSPI.h>
#include <WiFi.h>
#include "time.h"
#include <esp_adc_cal.h>
#include "pin_config.h"

// — Power‑on pin (keeps display alive on battery) —
/* #define PIN_POWER_ON 15  // defined in pin_config.h */

// — Screen control button —
#define BUTTON_PIN 14
volatile bool buttonPressed = false;
bool screenOn = true;
unsigned long lastButtonPress = 0;
const unsigned long DEBOUNCE_DELAY = 200;

// — Wi‑Fi & NTP settings —
const char* ssid      = "SSID";
const char* password  = "WIFI-Password";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset = -5 * 3600;
const int   dstOffset = 3600;

// — Battery sense pin & calibration —
#define PIN_BAT_VOLT  4        // Changed from 35 to 4 (common battery pin)
const float R_DIV       = 2.0f;  // Vbat = Vadc * R_DIV
static esp_adc_cal_characteristics_t adc_chars;
static uint32_t batteryVoltage = 0;  // Cache battery reading

// — Daily schedule —
struct Period { int h, m; };
Period schedule[] = {
  {7,50},{8,54},{9,50},{10,44},
  {10,44},{12,12},{13,6},{14,0}
};
const Period lunchTime = {10,44}, endTime = {14,50};
const int periodCount = sizeof(schedule)/sizeof(schedule[0]);

// — School span dates —
struct Date { int y,m,d; };
const Date schoolStart = {2025,8,11}, schoolEnd = {2026,5,28};
time_t startSec, endSec; 
unsigned long spanDays;

// — Page cycling —
const uint32_t PAGE_INTERVAL = 10000;
uint32_t lastSwitch = 0; 
uint8_t  pageIndex  = 0;
uint8_t  lastPageIndex = 255;  // Track page changes

// — Layout constants —
#define STATUS_H    28      // Compact height
#define LEFT_MARGIN 5       // Tighter margins
#define CORNER_RADIUS 4     // Smaller rounded corners

// — Button interrupt handler —
void IRAM_ATTR buttonISR() {
  buttonPressed = true;
}

// — TFT display —
TFT_eSPI tft = TFT_eSPI();

// — Helpers —
unsigned long toSec(int h,int m){ return h*3600UL + m*60UL; }
unsigned long nowSec(){
  time_t raw = time(nullptr);
  struct tm tm; localtime_r(&raw,&tm);
  return tm.tm_hour*3600UL + tm.tm_min*60UL + tm.tm_sec;
}
time_t mkTime(const Date &d){
  struct tm tm={0};
  tm.tm_year = d.y - 1900;
  tm.tm_mon  = d.m - 1;
  tm.tm_mday = d.d;
  return mktime(&tm);
}
void format12(char* buf,int h,int m,int s){
  bool pm = (h >= 12);
  int hh = h % 12; if(!hh) hh = 12;
  sprintf(buf,"%02d:%02d:%02d %s", hh,m,s, pm?"PM":"AM");
}
String fmtHMS(long s){
  if(s<0) s=0;
  int h=s/3600, m=(s%3600)/60;
  char b[16]; sprintf(b,"%dh %02dm",h,m);
  return String(b);
}
String fmtDays(long d){
  if(d<0) d=0;
  char buf[16]; sprintf(buf,"%ldd",d);
  return String(buf);
}

// — Compact progress bar —
void drawProgressBar(int x, int y, int w, int h, float progress, uint32_t color, const char* label = nullptr) {
  // Background
  tft.fillRoundRect(x, y, w, h, 2, TFT_DARKGREY);
  tft.drawRoundRect(x, y, w, h, 2, TFT_WHITE);
  
  // Progress fill
  int fillWidth = (int)((w - 2) * progress);
  if (fillWidth > 0) {
    tft.fillRoundRect(x + 1, y + 1, fillWidth, h - 2, 1, color);
  }
  
  // Progress percentage (smaller text)
  if (label) {
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    int tw = tft.textWidth(label);
    tft.setCursor(x + (w - tw) / 2, y + (h - 8) / 2);
    tft.print(label);
  }
}

// — Compact battery icon —
void drawBatteryIcon(int x, int y, uint32_t voltage) {
  float batteryPercent;
  uint32_t color;
  
  // Better battery calculation
  if (voltage > 4200) {
    batteryPercent = 1.0f;
    color = TFT_GREEN;
  } else if (voltage > 4000) {
    batteryPercent = (voltage - 3700) / 500.0f;
    color = TFT_GREEN;
  } else if (voltage > 3700) {
    batteryPercent = (voltage - 3700) / 300.0f;
    color = TFT_YELLOW;
  } else {
    batteryPercent = 0.0f;
    color = TFT_RED;
  }
  
  // Smaller battery outline
  tft.drawRoundRect(x, y, 18, 8, 1, TFT_WHITE);
  tft.fillRect(x + 18, y + 2, 2, 4, TFT_WHITE);
  
  // Battery fill
  int fillWidth = (int)(16 * batteryPercent);
  if (fillWidth > 0) {
    tft.fillRoundRect(x + 1, y + 1, fillWidth, 6, 1, color);
  }
  
  // Compact voltage text
  char buf[12];
  if (voltage > 4500) {
    strcpy(buf, "USB");
  } else {
    sprintf(buf, "%.1fV", voltage / 1000.0f);
  }
  
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(x + 22, y);
  tft.print(buf);
}

// — Compact status bar —  
void drawStatusBar(){
  // Simpler gradient background
  for (int i = 0; i < STATUS_H; i++) {
    uint32_t shade = tft.color565(15 + i/2, 15 + i/2, 30 + i);
    tft.drawFastHLine(0, i, tft.width(), shade);
  }

  // Compact Wi‑Fi signal strength
  int32_t rssi = WiFi.RSSI();
  int bars = (rssi > -50 ? 4 : rssi > -60 ? 3 : rssi > -70 ? 2 : rssi > -80 ? 1 : 0);
  
  for(int i = 0; i < 4; i++){
    int x = LEFT_MARGIN + i * 6;
    int h = 3 + i * 2;
    int y = STATUS_H - h - 3;
    uint32_t col = (i < bars ? TFT_CYAN : TFT_DARKGREY);
    tft.fillRoundRect(x, y, 4, h, 1, col);
  }

  // WiFi label
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(LEFT_MARGIN + 26, STATUS_H - 10);
  tft.print("WiFi");

  // Battery reading with proper timing
  static unsigned long nextBatteryRead = 0;
  if (millis() > nextBatteryRead) {
    // Multiple readings for accuracy
    uint32_t total = 0;
    for (int i = 0; i < 10; i++) {
      total += analogRead(PIN_BAT_VOLT);
      delay(1);
    }
    uint32_t raw = total / 10;
    
    // Convert to voltage with proper calibration
    batteryVoltage = esp_adc_cal_raw_to_voltage(raw, &adc_chars) * R_DIV;
    nextBatteryRead = millis() + 5000; // Update every 5 seconds
  }

  // Draw compact battery indicator
  drawBatteryIcon(tft.width() - 58, STATUS_H - 12, batteryVoltage);
}

// — Vertical name display —
void drawName(){
  tft.setTextSize(3);  // Bigger text
  tft.setTextColor(TFT_CYAN);
  
  // Shadow effect
  const char* nm = "SAM";
  int charHeight = 24;  // Bigger height
  int totalH = 3 * charHeight;
  int startY = STATUS_H + 8;
  
  for(int i = 0; i < 3; i++){
    // Shadow
    tft.setTextColor(TFT_DARKGREY);
    tft.setCursor(LEFT_MARGIN + 1, startY + i * charHeight + 1);
    tft.print(nm[i]);
    
    // Main text
    tft.setTextColor(TFT_CYAN);
    tft.setCursor(LEFT_MARGIN, startY + i * charHeight);
    tft.print(nm[i]);
  }
}

// — Card-style content area —
void drawCard(int x, int y, int w, int h, uint32_t bgColor = TFT_BLACK) {
  tft.fillRoundRect(x, y, w, h, CORNER_RADIUS, bgColor);
  tft.drawRoundRect(x, y, w, h, CORNER_RADIUS, TFT_DARKGREY);
}

void setup(){
  // keep panel on battery
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);

  Serial.begin(115200);

  // Button setup with interrupt
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

  // TFT init
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  
  // Show loading screen
  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN);
  tft.setCursor(50, 100);
  tft.print("Initializing...");

  // ADC calibration - better setup
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT_VOLT, ADC_11db);
  esp_adc_cal_characterize(
    ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars
  );

  // School span
  startSec = mkTime(schoolStart);
  endSec   = mkTime(schoolEnd);
  spanDays = (endSec - startSec) / 86400UL;

  // Wi‑Fi + NTP
  WiFi.begin(ssid, password);
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 50) {
    delay(200);
    attempts++;
    if (attempts % 5 == 0) {
      tft.print(".");
    }
  }
  
  configTime(gmtOffset, dstOffset, ntpServer);
  delay(3000); // Wait for time sync

  lastSwitch = millis();
  tft.fillScreen(TFT_BLACK); // Clear loading screen
}

void loop(){
  // Handle button press for screen on/off
  if (buttonPressed && (millis() - lastButtonPress > DEBOUNCE_DELAY)) {
    screenOn = !screenOn;
    lastButtonPress = millis();
    buttonPressed = false;
    
    if (screenOn) {
      // Turn screen on
      digitalWrite(PIN_POWER_ON, HIGH);
      delay(100);
      tft.init();
      tft.setRotation(1);
      tft.fillScreen(TFT_BLACK);
      lastPageIndex = 255; // Force redraw
    } else {
      // Turn screen off
      tft.fillScreen(TFT_BLACK);
      digitalWrite(PIN_POWER_ON, LOW);
      return; // Skip rest of loop when screen is off
    }
  }
  
  // Skip display updates when screen is off
  if (!screenOn) {
    delay(100);
    return;
  }

  uint32_t now = millis();
  bool pageChanged = false;
  
  if(now - lastSwitch >= PAGE_INTERVAL){
    pageIndex = (pageIndex + 1) % 3;
    lastSwitch = now;
    pageChanged = true;
  }

  time_t raw = time(nullptr);
  struct tm tm; localtime_r(&raw, &tm);
  unsigned long curS = nowSec();

  // Only redraw when page changes or every second for clock updates
  static unsigned long lastClockUpdate = 0;
  bool shouldUpdate = pageChanged || (pageIndex == 1 && (now - lastClockUpdate > 1000));
  
  if (shouldUpdate || lastPageIndex != pageIndex) {
    if (pageChanged || lastPageIndex != pageIndex) {
      tft.fillScreen(TFT_BLACK); // Full clear only on page change
      lastPageIndex = pageIndex;
    }
    
    drawStatusBar();
    drawName();

    const int contentX = LEFT_MARGIN + 40;  // More space for bigger name
    const int contentY = STATUS_H + 12;
    const int contentW = tft.width() - contentX - LEFT_MARGIN;
    const int contentH = tft.height() - contentY - 5;

    if(pageIndex == 0){
      // School schedule page
      drawCard(contentX, contentY, contentW, contentH, tft.color565(8, 8, 15));
      
      int yPos = contentY + 10;
      int itemHeight = (contentH - 25) / 3;  // Adjust for bigger text
      
      // Next period
      unsigned long n=0;
      for(int i=0;i<periodCount;i++){
        unsigned long t=toSec(schedule[i].h, schedule[i].m);
        if(t>curS){ n=t; break; }
      }
      if(!n) n=toSec(schedule[0].h, schedule[0].m)+86400;
      long dN = n - curS;
      
      tft.setTextSize(1); 
      tft.setTextColor(TFT_YELLOW);
      tft.setCursor(contentX + 8, yPos);
      tft.print("Next Period: "); tft.print(fmtHMS(dN));
      
      char progLabel[12];
      sprintf(progLabel, "%.0f%%", (1.0f - float(dN)/86400) * 100);
      drawProgressBar(contentX + 8, yPos + 14, contentW - 16, 14, 
                     1.0f - float(dN)/86400, TFT_YELLOW, progLabel);

      yPos += itemHeight;

      // Lunch
      unsigned long l=toSec(lunchTime.h, lunchTime.m);
      if(l<=curS) l+=86400;
      long dL = l - curS;
      
      tft.setTextSize(1);
      tft.setTextColor(TFT_MAGENTA);
      tft.setCursor(contentX + 8, yPos);
      tft.print("Lunch: "); tft.print(fmtHMS(dL));
      
      sprintf(progLabel, "%.0f%%", (1.0f - float(dL)/86400) * 100);
      drawProgressBar(contentX + 8, yPos + 14, contentW - 16, 14,
                     1.0f - float(dL)/86400, TFT_MAGENTA, progLabel);

      yPos += itemHeight;

      // End of day
      unsigned long e=toSec(endTime.h, endTime.m);
      if(e<=curS) e+=86400;
      long dE = e - curS;
      
      tft.setTextSize(1);
      tft.setTextColor(TFT_ORANGE);
      tft.setCursor(contentX + 8, yPos);
      tft.print("School End: "); tft.print(fmtHMS(dE));
      
      sprintf(progLabel, "%.0f%%", (1.0f - float(dE)/86400) * 100);
      drawProgressBar(contentX + 8, yPos + 14, contentW - 16, 14,
                     1.0f - float(dE)/86400, TFT_ORANGE, progLabel);
    }
    else if(pageIndex == 1){
      // Clock page
      drawCard(contentX, contentY, contentW, contentH, tft.color565(5, 12, 20));
      
      char ts[24]; 
      format12(ts, tm.tm_hour, tm.tm_min, tm.tm_sec);
      
      // Bigger time display
      tft.setTextSize(4);
      tft.setTextColor(TFT_WHITE);
      int tw = tft.textWidth(ts);
      int tx = contentX + (contentW - tw) / 2;
      int ty = contentY + 25;
      
      // Glow effect
      tft.setTextColor(TFT_CYAN);
      tft.setCursor(tx + 1, ty + 1);
      tft.print(ts);
      tft.setTextColor(TFT_WHITE);
      tft.setCursor(tx, ty);
      tft.print(ts);
      
      // Bigger date
      char ds[24];
      const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      sprintf(ds, "%s %d, %d", months[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
      
      tft.setTextSize(2);
      tft.setTextColor(TFT_CYAN);
      tw = tft.textWidth(ds);
      tft.setCursor(contentX + (contentW - tw) / 2, ty + 58);
      tft.print(ds);
      
      lastClockUpdate = now;
    }
    else {
      // School days countdown
      drawCard(contentX, contentY, contentW, contentH, tft.color565(12, 20, 12));
      
      long daysLeft = (endSec - raw)/86400;
      if (daysLeft < 0) daysLeft = 0;  // Handle past end date
      float progress = 1.0f - (float(daysLeft)/float(spanDays));
      if (progress < 0) progress = 0;  // Handle before start date
      if (progress > 1) progress = 1;  // Handle past end date
      
      tft.setTextSize(2);
      tft.setTextColor(TFT_GREEN);
      tft.setCursor(contentX + 8, contentY + 15);
      tft.print("School Days Left");
      
      tft.setTextSize(4);
      tft.setTextColor(TFT_WHITE);
      char dayStr[16];
      sprintf(dayStr, "%ld", daysLeft);
      int tw = tft.textWidth(dayStr);
      tft.setCursor(contentX + (contentW - tw) / 2, contentY + 40);
      tft.print(dayStr);
      
      char progLabel[16];
      sprintf(progLabel, "%.1f%% Done", progress * 100);
      drawProgressBar(contentX + 8, contentY + 75, contentW - 16, 16,
                     progress, TFT_GREEN, progLabel);
    }
  }

  delay(50); // Reduced delay for smoother updates
}
