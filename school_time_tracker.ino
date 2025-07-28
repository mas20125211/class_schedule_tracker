#include <TFT_eSPI.h>
#include <WiFi.h>
#include "time.h"
#include <esp_adc_cal.h>
#include "pin_config.h"

// — Power‑on pin (keeps display alive on battery) —
// PIN_POWER_ON defined in pin_config.h

// — Screen control button —
#define BUTTON_PIN      14
volatile bool buttonPressed   = false;
unsigned long lastButtonPress = 0;
const unsigned long DEBOUNCE_DELAY = 200;

// — Auto‑sleep timeout —
const unsigned long SLEEP_TIMEOUT = 60000;  // 1 minute
unsigned long lastActivity        = 0;
bool screenOn                     = true;

// — Wi‑Fi & NTP settings —
const char* ssid      = "";
const char* password  = "";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset = -5 * 3600;
const int   dstOffset = 3600;

// — Battery sense pin & calibration —
#define PIN_BAT_VOLT  4
const float R_DIV       = 2.0f;   // Vbat = Vadc * R_DIV
static esp_adc_cal_characteristics_t adc_chars;
static uint32_t batteryVoltage = 0;

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
uint32_t lastSwitch   = 0;
uint8_t  pageIndex    = 0;
uint8_t  lastPageIndex = 255;

// — Layout constants —
#define STATUS_H      28
#define LEFT_MARGIN   5
#define CORNER_RADIUS 4

// — TFT display —
TFT_eSPI tft = TFT_eSPI();

// — Button ISR —
void IRAM_ATTR buttonISR() {
  buttonPressed = true;
}

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
  bool pm = (h>=12);
  int hh = h%12; if(!hh) hh=12;
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

// — Draw a compact progress bar —
void drawProgressBar(int x,int y,int w,int h,float p,uint32_t c,const char* label=nullptr){
  tft.fillRoundRect(x,y,w,h,2,TFT_DARKGREY);
  tft.drawRoundRect(x,y,w,h,2,TFT_WHITE);
  int fillW = int((w-2)*p);
  if(fillW>0) tft.fillRoundRect(x+1,y+1,fillW,h-2,1,c);
  if(label){
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    int tw = tft.textWidth(label);
    tft.setCursor(x + (w-tw)/2, y + (h-8)/2);
    tft.print(label);
  }
}

// — Draw a compact battery icon —
void drawBatteryIcon(int x,int y,uint32_t voltage){
  float pct; uint32_t col;
  if(voltage>4200){ pct=1.0f; col=TFT_GREEN; }
  else if(voltage>4000){ pct=(voltage-3700)/500.0f; col=TFT_GREEN; }
  else if(voltage>3700){ pct=(voltage-3700)/300.0f; col=TFT_YELLOW; }
  else { pct=0; col=TFT_RED; }
  tft.drawRoundRect(x,y,18,8,1,TFT_WHITE);
  tft.fillRect(x+18,y+2,2,4,TFT_WHITE);
  int fw = int(16*pct);
  if(fw>0) tft.fillRoundRect(x+1,y+1,fw,6,1,col);
  char buf[12];
  if(voltage>4500) strcpy(buf,"USB");
  else sprintf(buf,"%.1fV",voltage/1000.0f);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(x+22,y);
  tft.print(buf);
}

// — Draw the status bar —
void drawStatusBar(){
  // gradient
  for(int i=0;i<STATUS_H;i++){
    uint32_t sh = tft.color565(15+i/2,15+i/2,30+i);
    tft.drawFastHLine(0,i,tft.width(),sh);
  }
  // Wi-Fi bars
  int32_t rssi = WiFi.RSSI();
  int bars = (rssi>-50?4:rssi>-60?3:rssi>-70?2:rssi>-80?1:0);
  for(int i=0;i<4;i++){
    int x=LEFT_MARGIN+i*6, h=3+i*2, y=STATUS_H-h-3;
    tft.fillRoundRect(x,y,4,h,1, i<bars?TFT_CYAN:TFT_DARKGREY);
  }
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(LEFT_MARGIN+26, STATUS_H-10);
  tft.print("WiFi");
  // battery read every 5s
  static unsigned long nxt=0;
  if(millis()>nxt){
    uint32_t tot=0;
    for(int i=0;i<10;i++){ tot+=analogRead(PIN_BAT_VOLT); delay(1); }
    batteryVoltage = esp_adc_cal_raw_to_voltage(tot/10,&adc_chars)*R_DIV;
    nxt=millis()+5000;
  }
  drawBatteryIcon(tft.width()-58, STATUS_H-12, batteryVoltage);
}

// — Draw vertical “SAM” —
void drawName(){
  const char *nm="SAM";
  int ch=24, sy=STATUS_H+8;
  tft.setTextSize(3);
  for(int i=0;i<3;i++){
    tft.setTextColor(TFT_DARKGREY);
    tft.setCursor(LEFT_MARGIN+1, sy+i*ch+1);
    tft.print(nm[i]);
    tft.setTextColor(TFT_GREEN);
    tft.setCursor(LEFT_MARGIN, sy+i*ch);
    tft.print(nm[i]);
  }
}

// — Draw a card —
void drawCard(int x,int y,int w,int h,uint32_t bg=TFT_BLACK){
  tft.fillRoundRect(x,y,w,h,CORNER_RADIUS,bg);
  tft.drawRoundRect(x,y,w,h,CORNER_RADIUS,TFT_DARKGREY);
}

void setup(){
  pinMode(PIN_POWER_ON, OUTPUT); digitalWrite(PIN_POWER_ON,HIGH);
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
  tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT_VOLT, ADC_11db);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
  startSec = mkTime(schoolStart);
  endSec   = mkTime(schoolEnd);
  spanDays = (endSec - startSec)/86400UL;
  WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED) delay(200);
  configTime(gmtOffset,dstOffset,ntpServer);
  delay(2000);
  lastSwitch   = millis();
  lastActivity = millis();
}

void loop(){
  unsigned long now = millis();

  // Wake on button
  if(buttonPressed && now - lastButtonPress > DEBOUNCE_DELAY){
    buttonPressed   = false;
    lastButtonPress = now;
    screenOn        = true;
    digitalWrite(PIN_POWER_ON, HIGH);
    tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);
    lastPageIndex   = 255;
    lastActivity    = now;
    return;
  }

  // Auto‑sleep
  if(screenOn && now - lastActivity >= SLEEP_TIMEOUT){
    tft.fillScreen(TFT_BLACK);
    digitalWrite(PIN_POWER_ON, LOW);
    screenOn = false;
  }
  // If asleep, wait
  if(!screenOn){
    delay(50);
    return;
  }

  // We’re awake
  lastActivity = now;

  // Page cycle
  bool pageChanged = false;
  if(now - lastSwitch >= PAGE_INTERVAL){
    pageIndex  = (pageIndex + 1) % 3;
    lastSwitch = now;
    pageChanged= true;
  }

  time_t raw=time(nullptr);
  struct tm tm; localtime_r(&raw,&tm);
  unsigned long curS=nowSec();
  static unsigned long lastClock=0;
  bool doUpd = pageChanged || (pageIndex==1 && now - lastClock >1000);

  if(doUpd || lastPageIndex!=pageIndex){
    if(pageChanged||lastPageIndex!=pageIndex){
      tft.fillScreen(TFT_BLACK);
      lastPageIndex = pageIndex;
    }
    drawStatusBar();
    drawName();

    int cx=LEFT_MARGIN+40, cy=STATUS_H+12;
    int cw=tft.width()-cx-LEFT_MARGIN, ch=tft.height()-cy-5;

    if(pageIndex==0){
      drawCard(cx,cy,cw,ch, tft.color565(8,8,15));
      int yPos=cy+10, ih=(ch-25)/3;
      // Next
      unsigned long n=0;
      for(int i=0;i<periodCount;i++){
        unsigned long t=toSec(schedule[i].h,schedule[i].m);
        if(t>curS){ n=t; break; }
      }
      if(!n) n=toSec(schedule[0].h,schedule[0].m)+86400;
      long dN=n-curS;
      tft.setTextSize(1); tft.setTextColor(TFT_YELLOW);
      tft.setCursor(cx+8,yPos);
      tft.print("Next: "); tft.print(fmtHMS(dN));
      char lbl[12]; sprintf(lbl,"%.0f%%",(1.0f-float(dN)/86400)*100);
      drawProgressBar(cx+8,yPos+14,cw-16,14,1.0f-float(dN)/86400,TFT_YELLOW,lbl);
      yPos+=ih;
      // Lunch
      unsigned long l=toSec(lunchTime.h,lunchTime.m);
      if(l<=curS) l+=86400;
      long dL=l-curS;
      tft.setTextColor(TFT_MAGENTA);
      tft.setCursor(cx+8,yPos);
      tft.print("Lunch: "); tft.print(fmtHMS(dL));
      sprintf(lbl,"%.0f%%",(1.0f-float(dL)/86400)*100);
      drawProgressBar(cx+8,yPos+14,cw-16,14,1.0f-float(dL)/86400,TFT_MAGENTA,lbl);
      yPos+=ih;
      // End
      unsigned long e=toSec(endTime.h,endTime.m);
      if(e<=curS) e+=86400;
      long dE=e-curS;
      tft.setTextColor(TFT_ORANGE);
      tft.setCursor(cx+8,yPos);
      tft.print("End: "); tft.print(fmtHMS(dE));
      sprintf(lbl,"%.0f%%",(1.0f-float(dE)/86400)*100);
      drawProgressBar(cx+8,yPos+14,cw-16,14,1.0f-float(dE)/86400,TFT_ORANGE,lbl);
    }
    else if(pageIndex==1){
      drawCard(cx,cy,cw,ch, tft.color565(5,12,20));
      char ts[24]; format12(ts,tm.tm_hour,tm.tm_min,tm.tm_sec);
      tft.setTextSize(4); tft.setTextColor(TFT_WHITE);
      int tw=tft.textWidth(ts), tx=cx+(cw-tw)/2, ty=cy+25;
      tft.setTextColor(TFT_CYAN); tft.setCursor(tx+1,ty+1); tft.print(ts);
      tft.setTextColor(TFT_WHITE); tft.setCursor(tx,ty); tft.print(ts);
      char ds[24];
      const char* mth[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
      sprintf(ds,"%s %d, %d",mth[tm.tm_mon],tm.tm_mday,tm.tm_year+1900);
      tft.setTextSize(2); tft.setTextColor(TFT_CYAN);
      tw=tft.textWidth(ds);
      tft.setCursor(cx+(cw-tw)/2,ty+58);
      tft.print(ds);
      lastClock = now;
    }
    else {
      drawCard(cx,cy,cw,ch, tft.color565(12,20,12));
      long daysLeft=(endSec-raw)/86400;
      if(daysLeft<0) daysLeft=0;
      float prog=1.0f-(float(daysLeft)/float(spanDays));
      prog=prog<0?0:prog>1?1:prog;
      tft.setTextSize(2); tft.setTextColor(TFT_GREEN);
      tft.setCursor(cx+8,cy+15); tft.print("Days Left");
      char ds[16]; sprintf(ds,"%ld",daysLeft);
      tft.setTextSize(4); tft.setTextColor(TFT_WHITE);
      int tw=tft.textWidth(ds);
      tft.setCursor(cx+(cw-tw)/2,cy+40);
      tft.print(ds);
      char lbl[16]; sprintf(lbl,"%.1f%%",prog*100);
      drawProgressBar(cx+8,cy+75,cw-16,16,prog,TFT_GREEN,lbl);
    }
  }

  delay(50);
}
