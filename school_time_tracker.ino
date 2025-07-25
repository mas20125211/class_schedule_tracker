#include <TFT_eSPI.h>
#include <WiFi.h>
#include "time.h"

// — Wi‑Fi & NTP settings —
const char* ssid      = "Flgurl_2.4G";
const char* password  = "TimpforW1";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset = -5 * 3600;
const int   dstOffset = 3600;

// — Battery sense pin —
#define BAT_PIN 35
const float R_DIV = 2.0;
const float V_MIN = 3.0, V_MAX = 4.2;

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
time_t startSec, endSec; unsigned long spanDays;

// — Page cycling —
const uint32_t PAGE_INTERVAL = 10000;
uint32_t lastSwitch = 0; 
uint8_t  pageIndex  = 0;

// — Layout constants —
#define STATUS_H    32      
#define LEFT_MARGIN 5

// — TFT display —
TFT_eSPI tft = TFT_eSPI();

// — Helpers —
unsigned long toSec(int h,int m){ return h*3600UL + m*60UL; }
unsigned long nowSec(){
  time_t raw=time(nullptr); struct tm tm; localtime_r(&raw,&tm);
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
void drawBar(int x,int y,int w,int h,float p,uint32_t c){
  tft.drawRect(x,y,w,h,c);
  int f=int((w-2)*p);
  if(f>0) tft.fillRect(x+1,y+1,f,h-2,c);
}

// — Status bar at top, smaller icons/text —
void drawStatusBar(){
  tft.fillRect(0, 0, tft.width(), STATUS_H, TFT_DARKGREY);

  // Wi‑Fi bars: shorter and lower
  int32_t rssi = WiFi.RSSI();
  int bars = (rssi > -50 ? 4 : rssi > -60 ? 3 : rssi > -70 ? 2 : rssi > -80 ? 1 : 0);
  for(int i=0;i<4;i++){
    int x = LEFT_MARGIN + i*7;
    int h = 3 + i*3;         // smaller growth
    int y = STATUS_H/2 - h/2 + 4; // centered in status bar, shifted down by 4px
    uint32_t col = (i<bars ? TFT_GREEN : TFT_LIGHTGREY);
    tft.fillRoundRect(x, y, 5, h, 1, col);
    tft.drawRoundRect(x, y, 5, h, 1, TFT_BLACK);
  }

  // Battery %: textSize(1) and moved down
  analogReadResolution(12);
  int raw = analogRead(BAT_PIN);
  float v = raw/4095.0f * 3.3f * R_DIV;
  int pct = constrain(int((v - V_MIN)/(V_MAX - V_MIN)*100), 0, 100);
  char buf[6]; sprintf(buf, "%d%%", pct);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  int tw = tft.textWidth(buf);
  int bx = tft.width() - tw - LEFT_MARGIN;
  int by = STATUS_H/2 - 8;  // centered and down by 4px
  tft.setCursor(bx, by);
  tft.print(buf);
  tft.drawRect(bx-1, 0, tw+2, STATUS_H, TFT_BLACK);
}

// — Draw “SAM” vertically —
void drawName(){
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN);
  const char* nm = "SAM";
  int totalH = 3 * 16;
  int startY = STATUS_H + (tft.height() - STATUS_H - totalH)/2;
  for(int i=0;i<3;i++){
    tft.setCursor(LEFT_MARGIN, startY + i*16);
    tft.print(nm[i]);
  }
}

void setup(){
  Serial.begin(115200);
  tft.init(); tft.setRotation(0); tft.fillScreen(TFT_BLACK);

  // Compute school span
  startSec = mkTime(schoolStart);
  endSec   = mkTime(schoolEnd);
  spanDays = (endSec - startSec) / 86400UL;

  WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED) delay(200);
  configTime(gmtOffset,dstOffset,ntpServer);
  delay(2000);

  lastSwitch = millis();
}

void loop(){
  uint32_t m = millis();
  if(m - lastSwitch >= PAGE_INTERVAL){
    pageIndex = (pageIndex+1)%3;
    lastSwitch = m;
  }

  time_t raw = time(nullptr);
  struct tm tm; localtime_r(&raw,&tm);
  unsigned long curS = nowSec();

  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  drawName();

  const int bx = LEFT_MARGIN + 40;
  const int y0 = STATUS_H + 8;

  if(pageIndex == 0){
    unsigned long n=0; for(int i=0;i<periodCount;i++){
      unsigned long t=toSec(schedule[i].h,schedule[i].m);
      if(t>curS){ n=t; break;} }
    if(!n) n=toSec(schedule[0].h,schedule[0].m)+86400;
    long dN=n-curS;
    tft.setTextSize(2); tft.setTextColor(TFT_YELLOW);
    tft.setCursor(bx,y0+10); tft.print("Next: "); tft.print(fmtHMS(dN));
    drawBar(bx,y0+30,180,12,1.0f-float(dN)/86400,TFT_YELLOW);

    unsigned long l=toSec(lunchTime.h,lunchTime.m);
    if(l<=curS) l+=86400;
    long dL=l-curS;
    tft.setTextColor(TFT_MAGENTA);
    tft.setCursor(bx,y0+60); tft.print("Lunch: "); tft.print(fmtHMS(dL));
    drawBar(bx,y0+80,180,12,1.0f-float(dL)/86400,TFT_MAGENTA);

    unsigned long e=toSec(endTime.h,endTime.m);
    if(e<=curS) e+=86400;
    long dE=e-curS;
    tft.setTextColor(TFT_ORANGE);
    tft.setCursor(bx,y0+110); tft.print("End: "); tft.print(fmtHMS(dE));
    drawBar(bx,y0+130,180,12,1.0f-float(dE)/86400,TFT_ORANGE);
  }
  else if(pageIndex == 1){
    char ts[24]; format12(ts,tm.tm_hour,tm.tm_min,tm.tm_sec);
    tft.setTextSize(3); tft.setTextColor(TFT_WHITE);
    int w = tft.textWidth(ts);
    tft.setCursor((tft.width()-w)/2,y0+20);
    tft.print(ts);

    char ds[16];
    sprintf(ds,"%04d-%02d-%02d",tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday);
    tft.setTextSize(2); tft.setTextColor(TFT_CYAN);
    w = tft.textWidth(ds);
    tft.setCursor((tft.width()-w)/2,y0+80);
    tft.print(ds);
  }
  else {
    long left=(endSec-raw)/86400;
    float p=float(left)/float(spanDays);
    tft.setTextSize(2); tft.setTextColor(TFT_GREEN);
    tft.setCursor(bx,y0+60);
    tft.print("School Left:"); tft.print(fmtDays(left));
    drawBar(bx,y0+80,180,12,p,TFT_GREEN);
  }

  delay(100);
}
