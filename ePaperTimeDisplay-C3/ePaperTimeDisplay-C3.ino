/*====================================================================
  EPaperClock  —  ESP32-C3 WiFi e-paper clock
  ------------------------------------------------------------------
  Hardware:
    • WeAct 2.13" e-paper, 250x122, B/W, SSD1680 (partial refresh)
    • ESP32-C3 (QFN32, 4 MB flash), native USB-CDC serial
    • Wires: CS=10 DC=4 RST=3 BUSY=5 SCK=6 MOSI=7  (VCC=3V3, GND)
    • Optional wake button on GPIO1 (RTC pin) -> GND
    • Optional RGB LED (WS2812) - set RGB_PIN to your board's pin

  What it does:
    • Landscape clock: configurable header, big HH:MM, date line
    • Three tight partial windows so ONLY what changed is refreshed:
        header  (0,0,248,32)   - only when 'header' command runs
        time    (0,40,248,48)  - every minute
        date    (0,88,248,32)  - at midnight only
    • Full refresh on boot, on deep-sleep wake, and every 60 partials
      (ghost cleanup)
    • NTP once at boot -> internal RTC; resync hourly; no WiFi between
    • Stores up to 8 SSID/PW pairs in NVS; survives reboot
    • Serial command interface (115200, Newline) - see 'help'

  Commands:
    wifi | add wifi <ssid> <pw> | list | scan | connect <n|ssid>
    forget <n|ssid> | sync | time | zone [<+/-N[.N]>]
    header [<text>] | frame | rot [0-3] | power normal|light|deep | help

  Display tools (for learning / debugging the panel):
    frame - calibration frame for 10s: border, guides at y=40/y=80,
            corner labels "0,0" and "250,122". Confirms the coordinate
            system. Then restores the clock.
    rot   - bare: cycles all 4 rotations (2.5s each, labeled "ROT n"
            with its width x height), then restores the clock.
    rot N - sets rotation N (0-3), saves to NVS, redraws.
            NOTE: only 1 and 3 give landscape 250x122. 0 and 2 are
            portrait (122x250) and will clip the layout.

  Power modes:
    normal  - CPU awake, WiFi up. Best for a live clock.      [default]
    light   - light sleep. WiFi/SNTP unreliable; avoid.
    deep    - deep sleep to next minute; RTC keeps time, no WiFi
              per wake. Full redraw each wake (panel RAM is lost on
              reset, so a partial-only repaint would lose the header).

  Escape from deep sleep: press RESET (cold-reset -> normal mode).

  RGB LED:
    If your board has a WS2812, set RGB_PIN and uncomment the NeoPixel
    block. While awake in deep-sleep mode, the LED blinks green
    (250ms on/off). Harmless if no LED is present (guarded).

  Design notes:
    • setCursor() is TOP-LEFT for built-in fonts, BASELINE for
      FreeFonts. Layout uses FreeFonts.
    • Partial windows must have x%8==0 and w%8==0.
    • Refreshes are GUARDED: if getLocalTime() fails, the screen is
      left untouched so the last good time persists (bistable).
    • POSIX TZ signs are inverted; buildTzString() handles it.
      Fixed offset (no DST rules). Floats OK: +5.5, +5.75.
    • Header text is stored in NVS; survives reboot.
    • Deep sleep resets the chip; panel controller RAM is lost, so
      deep wakes do a full redraw.

  Version: 26-10-18
  Status:  working - time, wifi, display, power modes, header, rot setter
====================================================================*/

#include <GxEPD2_BW.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include "esp_sleep.h"

#include "fonts/FreeMonoBold24pt7b.h"
#include "fonts/FreeSans9pt7b.h"

// ---- Optional RGB LED: uncomment these 3 lines to enable the blink ----
#include <Adafruit_NeoPixel.h>
#define RGB_PIN 8
Adafruit_NeoPixel rgb(1, RGB_PIN, NEO_GRB + NEO_KHZ800);

// ---- FreeFonts (copy these .h files into your sketch folder) ----
//#include "FreeMonoBold24pt7b.h"
//#include "FreeSans9pt7b.h"

// ---------------- Panel pins (ESP32-C3) ----------------
#define EPD_CS   10
#define EPD_DC    4
#define EPD_RST   3
#define EPD_BUSY  5
#define EPD_SCK   6
#define EPD_MOSI  7

// ---------------- Optional wake button (RTC pin: GPIO0..GPIO5) ----------------
#define WAKE_BTN  1

// ---------------- Panel class ----------------
GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(
  GxEPD2_213_B74(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ---------------- NTP servers ----------------
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.nist.gov";

// ---------------- WiFi store ----------------
Preferences prefs;
#define MAX_NETS 8
struct Net { String ssid; String pw; };
Net nets[MAX_NETS];
int netCount = 0;

int partialCount = 0;
int displayRotation = 1;

// ---------------- Power mode ----------------
int powerMode = 0;   // 0=normal, 1=light, 2=deep
#define NVS_POWER_KEY "pmode"
#define NVS_ROT_KEY   "rot"
#define NVS_TZ_KEY    "tzoff"
#define NVS_HDR_KEY   "hdr"

// ---------------- Timezone + header ----------------
float tzOffset = 1.0;
char  tzString[24];
String headerText = "Tid:";

// ---------------- Resync interval ----------------
#define RESYNC_INTERVAL_MS (3600000UL)   // 1 hour

// =======================================================
//  Optional RGB blink
// =======================================================
void blinkAwake(int times) {
#ifdef RGB_PIN
  for (int i = 0; i < times; i++) {
    rgb.setPixelColor(0, rgb.Color(0, 60, 0));   // green
    rgb.show();
    delay(250);
    rgb.setPixelColor(0, 0);
    rgb.show();
    delay(250);
  }
#endif
}

// =======================================================
//  Timezone
// =======================================================
void buildTzString() {
  int totalMin = (int)round(tzOffset * 60);
  int invMin = -totalMin;
  int hh = invMin / 60;
  int mm = abs(invMin % 60);
  if (mm == 0)
    snprintf(tzString, sizeof(tzString), "UTC%+d", hh);
  else
    snprintf(tzString, sizeof(tzString), "UTC%+d:%02d", hh, mm);
  Serial.printf("[tz] offset %+.2f -> TZ='%s'\n", tzOffset, tzString);
}

void applyTz() {
  buildTzString();
  setenv("TZ", tzString, 1);
  tzset();
}

void setTzOffset(float off) {
  tzOffset = off;
  prefs.begin("wifi", false);
  prefs.putFloat(NVS_TZ_KEY, off);
  prefs.end();
  applyTz();
  Serial.printf("[tz] set to %+.2f\n", off);
}

// =======================================================
//  Header text
// =======================================================
void setHeaderText(const String& h) {
  headerText = h;
  prefs.begin("wifi", false);
  prefs.putString(NVS_HDR_KEY, h);
  prefs.end();
  Serial.printf("[hdr] header set to '%s'\n", h.c_str());
}

// =======================================================
//  WiFi storage
// =======================================================
void loadNets() {
  netCount = 0;
  prefs.begin("wifi", true);
  int n = prefs.getInt("count", 0);
  for (int i = 0; i < n && i < MAX_NETS; i++) {
    String key = "n" + String(i);
    String blob = prefs.getString(key.c_str(), "");
    int sep = blob.indexOf('|');
    if (sep > 0) {
      nets[netCount].ssid = blob.substring(0, sep);
      nets[netCount].pw   = blob.substring(sep + 1);
      netCount++;
    }
  }
  prefs.end();
  Serial.printf("[wifi] loaded %d saved network(s)\n", netCount);
}

void saveNets() {
  prefs.begin("wifi", false);
  prefs.putInt("count", netCount);
  for (int i = 0; i < netCount; i++) {
    String key = "n" + String(i);
    prefs.putString(key.c_str(), nets[i].ssid + "|" + nets[i].pw);
  }
  for (int i = netCount; i < MAX_NETS; i++) {
    String key = "n" + String(i);
    prefs.remove(key.c_str());
  }
  prefs.end();
}

bool addNet(const String& ssid, const String& pw) {
  for (int i = 0; i < netCount; i++) {
    if (nets[i].ssid == ssid) {
      nets[i].pw = pw; saveNets();
      Serial.printf("[wifi] updated '%s'\n", ssid.c_str());
      return true;
    }
  }
  if (netCount >= MAX_NETS) { Serial.println("[wifi] store full"); return false; }
  nets[netCount++] = { ssid, pw }; saveNets();
  Serial.printf("[wifi] added '%s' (index %d)\n", ssid.c_str(), netCount - 1);
  return true;
}

bool forgetNet(int idx) {
  if (idx < 0 || idx >= netCount) { Serial.println("[wifi] bad index"); return false; }
  Serial.printf("[wifi] forgot '%s'\n", nets[idx].ssid.c_str());
  for (int j = idx; j < netCount - 1; j++) nets[j] = nets[j + 1];
  netCount--; saveNets();
  return true;
}

void listNets() {
  if (netCount == 0) { Serial.println("[wifi] no saved networks"); return; }
  Serial.println("[wifi] saved networks:");
  for (int i = 0; i < netCount; i++)
    Serial.printf("  %d) %s\n", i, nets[i].ssid.c_str());
}

// =======================================================
//  Scan + connect
// =======================================================
int findSavedNetwork() {
  Serial.println("[wifi] scanning...");
  int n = WiFi.scanNetworks();
  if (n <= 0) { Serial.println("[wifi] no networks found"); return -1; }
  Serial.printf("[wifi] %d network(s) visible:\n", n);
  for (int i = 0; i < n; i++)
    Serial.printf("  %-32s RSSI %4d  ch %2d\n",
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  int bestSaved = -1, bestRssi = -9999;
  for (int i = 0; i < n; i++)
    for (int s = 0; s < netCount; s++)
      if (WiFi.SSID(i) == nets[s].ssid && WiFi.RSSI(i) > bestRssi) {
        bestRssi = WiFi.RSSI(i); bestSaved = s;
      }
  WiFi.scanDelete();
  if (bestSaved < 0) Serial.println("[wifi] no saved network is in range");
  return bestSaved;
}

bool connectTo(int idx, uint32_t timeoutMs = 15000) {
  if (idx < 0 || idx >= netCount) { Serial.println("[wifi] bad index"); return false; }
  Serial.printf("[wifi] connecting to '%s'...\n", nets[idx].ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(nets[idx].ssid.c_str(), nets[idx].pw.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("[wifi] connect failed");
  return false;
}

// =======================================================
//  NTP sync
// =======================================================
bool syncNTP(uint32_t timeoutMs = 20000) {
  Serial.println("[ntp] syncing...");
  applyTz();
  configTzTime(tzString, NTP1, NTP2);
  struct tm t;
  uint32_t start = millis();
  while (!getLocalTime(&t, 1000)) {
    if (millis() - start > timeoutMs) {
      Serial.println("[ntp] sync failed");
      return false;
    }
    delay(200);
  }
  char b[32];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &t);
  Serial.printf("[ntp] synced: %s\n", b);
  return true;
}

// =======================================================
//  Display — three tight windows + guarded
// =======================================================

// full screen (boot, deep wake, ghost cleanup)
void drawFullScreen() {
  struct tm t;
  if (!getLocalTime(&t, 100)) {
    Serial.println("[disp] no time - skip full refresh");
    return;
  }
  char hhmm[8], date[16];
  strftime(hhmm, sizeof(hhmm), "%H:%M", &t);
  strftime(date, sizeof(date), "%Y-%m-%d", &t);

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(8, 18);
    display.print(headerText);
    display.setFont(&FreeMonoBold24pt7b);
    display.setCursor(8, 72);
    display.print(hhmm);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(8, 108);
    display.print(date);
  } while (display.nextPage());
}

// header strip only
void drawHeader() {
  display.setPartialWindow(0, 0, 248, 32);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(8, 18);
    display.print(headerText);
  } while (display.nextPage());
}

// time only (the minute ticker)
void drawTimeOnly() {
  struct tm t;
  if (!getLocalTime(&t, 100)) {
    Serial.println("[disp] no time - skip time refresh");
    return;
  }
  char hhmm[8];
  strftime(hhmm, sizeof(hhmm), "%H:%M", &t);

  display.setPartialWindow(0, 40, 248, 48);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold24pt7b);
    display.setCursor(8, 72);
    display.print(hhmm);
  } while (display.nextPage());
}

// date strip only (midnight)
void drawDateOnly() {
  struct tm t;
  if (!getLocalTime(&t, 100)) return;
  char date[16];
  strftime(date, sizeof(date), "%Y-%m-%d", &t);

  display.setPartialWindow(0, 88, 248, 32);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(8, 108);
    display.print(date);
  } while (display.nextPage());
}

void maybeCleanGhosts() {
  if (++partialCount >= 60) {
    partialCount = 0;
    drawFullScreen();
  }
}

void drawFrame() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(0, 0, 250, 122, GxEPD_BLACK);
    display.drawLine(0, 40, 250, 40, GxEPD_BLACK);
    display.drawLine(0, 80, 250, 80, GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(4, 14);  display.print("0,0");
    display.setCursor(4, 118); display.print("250,122");
  } while (display.nextPage());
}

void rotationTest() {
  Serial.println("[disp] rotation test - watch the panel");
  for (int r = 0; r < 4; r++) {
    display.setRotation(r);
    display.setFullWindow();
    display.firstPage();
    do {
      display.fillScreen(GxEPD_WHITE);
      display.drawRect(0, 0, display.width(), display.height(), GxEPD_BLACK);
      display.setFont(&FreeSans9pt7b);
      display.setTextColor(GxEPD_BLACK);
      display.setCursor(6, 22); display.printf("ROT %d", r);
      display.setCursor(6, 44); display.printf("%dx%d", display.width(), display.height());
    } while (display.nextPage());
    delay(2500);
  }
  display.setRotation(displayRotation);
  drawFullScreen();
  Serial.println("[disp] done - pick the wide, upright frame");
}

// =======================================================
//  Rotation setter
// =======================================================
void setDisplayRotation(int r) {
  if (r < 0 || r > 3) { Serial.println("[rot] must be 0-3"); return; }
  displayRotation = r;
  prefs.begin("wifi", false);
  prefs.putInt(NVS_ROT_KEY, r);
  prefs.end();
  display.setRotation(r);
  Serial.printf("[rot] set to %d (saved)\n", r);
  drawFullScreen();
}

// =======================================================
//  Power helper
// =======================================================
void setPowerMode(int m) {
  powerMode = m;
  prefs.begin("wifi", false);
  prefs.putInt(NVS_POWER_KEY, m);
  prefs.end();
  const char* names[] = {"normal", "light sleep", "deep sleep"};
  Serial.printf("[power] set to %s\n", names[m]);
}

// =======================================================
//  Serial parser
// =======================================================
String readLine() { String s = Serial.readStringUntil('\n'); s.trim(); return s; }

bool isNumber(const String& s) {
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); i++)
    if (!isDigit(s[i]) && s[i] != '-' && s[i] != '+') return false;
  return true;
}

void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  wifi                    - scan & connect to strongest saved net");
  Serial.println("  add wifi <ssid> <pw>    - save a network (pw may be empty)");
  Serial.println("  list                    - list saved networks (with indices)");
  Serial.println("  scan                    - scan and list visible networks");
  Serial.println("  connect <n|ssid>        - connect by index or name");
  Serial.println("  forget <n|ssid>         - remove a saved network");
  Serial.println("  sync                    - reconnect wifi & resync NTP");
  Serial.println("  time                    - show current RTC time");
  Serial.println("  zone [<+/-N[.N]>]       - show or set UTC offset (zone +5.5)");
  Serial.println("  header [<text>]         - show or set top label");
  Serial.println("  frame                   - draw calibration frame for 10s");
  Serial.println("  rot [0-3]               - test rotations, or set rotation 0-3");
  Serial.println("  power normal|light|deep - set power mode");
  Serial.println("  help                    - this list");
  Serial.println();
}

void handleCommand(String line) {
  if (line.length() == 0) return;

  if (line.startsWith("add wifi ")) {
    String rest = line.substring(9); rest.trim();
    int sp = rest.indexOf(' ');
    String ssid = (sp < 0) ? rest : rest.substring(0, sp);
    String pw   = (sp < 0) ? ""   : rest.substring(sp + 1); pw.trim();
    if (ssid.length() == 0) { Serial.println("usage: add wifi <ssid> <pw>"); return; }
    addNet(ssid, pw); return;
  }
  if (line == "wifi") {
    int idx = findSavedNetwork();
    if (idx >= 0) { if (connectTo(idx)) syncNTP(); }
    else Serial.println("[wifi] nothing to connect to");
    return;
  }
  if (line == "list") { listNets(); return; }
  if (line == "scan") { findSavedNetwork(); return; }

  if (line == "connect" || line.startsWith("connect ")) {
    String arg = (line.length() > 8) ? line.substring(8) : ""; arg.trim();
    if (arg.length() == 0) {
      if (netCount == 0) { Serial.println("[wifi] no saved networks"); return; }
      connectTo(0); return;
    }
    if (isNumber(arg)) { connectTo(arg.toInt()); return; }
    for (int i = 0; i < netCount; i++)
      if (nets[i].ssid == arg) { connectTo(i); return; }
    Serial.println("[wifi] not saved"); return;
  }
  if (line.startsWith("forget ")) {
    String arg = line.substring(7); arg.trim();
    if (isNumber(arg)) { forgetNet(arg.toInt()); return; }
    for (int i = 0; i < netCount; i++)
      if (nets[i].ssid == arg) { forgetNet(i); return; }
    Serial.println("[wifi] not found"); return;
  }
  if (line == "sync") {
    int idx = findSavedNetwork();
    if (idx >= 0 && connectTo(idx)) syncNTP();
    return;
  }
  if (line == "time") {
    struct tm t;
    if (getLocalTime(&t, 1000)) {
      char b[32]; strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &t);
      Serial.printf("[time] %s\n", b);
    } else Serial.println("[time] not synced");
    return;
  }

  // ---- zone ----
  if (line == "zone" || line == "zone ") {
    Serial.printf("[tz] current offset %+.2f (TZ='%s')\n", tzOffset, tzString);
    return;
  }
  if (line.startsWith("zone ")) {
    String arg = line.substring(5); arg.trim();
    if (arg.length() == 0) {
      Serial.printf("[tz] current offset %+.2f (TZ='%s')\n", tzOffset, tzString);
      return;
    }
    setTzOffset(arg.toFloat());
    Serial.println("[tz] resyncing to apply...");
    int idx = findSavedNetwork();
    if (idx >= 0 && connectTo(idx)) syncNTP();
    drawFullScreen();
    return;
  }

  // ---- header ----
  if (line == "header" || line == "header ") {
    Serial.printf("[hdr] current header '%s'\n", headerText.c_str());
    Serial.println("[hdr] usage: header <text>   (empty resets to 'Tid:')");
    return;
  }
  if (line.startsWith("header ")) {
    String arg = line.substring(7); arg.trim();
    if (arg.length() == 0) arg = "Tid:";
    setHeaderText(arg);
    drawHeader();
    return;
  }

  // ---- rot ----
  if (line == "rot") {
    Serial.printf("[rot] current rotation %d\n", displayRotation);
    rotationTest();
    return;
  }
  if (line.startsWith("rot ")) {
    String arg = line.substring(4); arg.trim();
    if (arg.length() == 0) {
      Serial.printf("[rot] current rotation %d\n", displayRotation);
      return;
    }
    setDisplayRotation(arg.toInt());
    return;
  }

  if (line == "frame") {
    Serial.println("[disp] frame for 10s");
    drawFrame(); delay(10000);
    drawFullScreen();
    return;
  }

  if (line == "power normal" || line == "power off") { setPowerMode(0); return; }
  if (line == "power light") { setPowerMode(1); return; }
  if (line == "power deep")  { setPowerMode(2); return; }

  if (line == "help") { printHelp(); return; }
  Serial.println("[cmd] unknown - type 'help'");
}

// =======================================================
//  Setup
// =======================================================
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== E-Paper Clock boot ===");

  // ---- onboard LED / RGB init ----
  pinMode(8, OUTPUT);
  digitalWrite(8, LOW);
#ifdef RGB_PIN
  rgb.begin();
  rgb.setBrightness(40);
  rgb.setPixelColor(0, 0); rgb.show();
#endif

  // ---- what woke us? ----
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  // ---- load persisted settings ----
  prefs.begin("wifi", true);
  powerMode       = prefs.getInt(NVS_POWER_KEY, 0);
  displayRotation = prefs.getInt(NVS_ROT_KEY, 1);
  tzOffset        = prefs.getFloat(NVS_TZ_KEY, 1.0);
  headerText      = prefs.getString(NVS_HDR_KEY, "Tid:");
  prefs.end();

  applyTz();

  Serial.printf("[cfg] powerMode=%d rot=%d tz=%+.2f header='%s'\n",
                powerMode, displayRotation, tzOffset, headerText.c_str());

  // ---- cold reset -> normal mode ----
  if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    Serial.println("[power] cold reset -> mode normal");
    setPowerMode(0);
  }

  // ---- optional wake button ----
  pinMode(WAKE_BTN, INPUT_PULLUP);
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    Serial.println("[power] wake button -> mode normal");
    setPowerMode(0);
  }

  // ---- display ----
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(displayRotation);

  // ---- wifi + NTP (only if RTC empty) ----
  loadNets();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);

  struct tm t;
  bool haveRTC = getLocalTime(&t, 100);

  if (!haveRTC) {
    Serial.println("[boot] RTC empty - need NTP sync");
    int idx = findSavedNetwork();
    if (idx >= 0) {
      if (connectTo(idx)) syncNTP();
    } else {
      Serial.println("[wifi] no saved net in range - type 'help'");
    }
    drawFullScreen();
  } else {
    // deep-sleep wake (or warm boot): panel RAM is lost on reset,
    // so a full redraw is required or the header/date would vanish.
    Serial.println("[boot] RTC valid - full redraw");
    drawFullScreen();
  }

  printHelp();
}

// =======================================================
//  Loop
// =======================================================
void loop() {
  // ---- serial ----
  if (Serial.available()) {
    String line = readLine();
    handleCommand(line);
  }

  // ---- hourly NTP resync ----
  static uint32_t lastSync = 0;
  if (powerMode == 0 && millis() - lastSync > RESYNC_INTERVAL_MS) {
    lastSync = millis();
    Serial.println("[ntp] hourly resync");
    int idx = findSavedNetwork();
    if (idx >= 0 && connectTo(idx)) syncNTP();
  }

  // ---- minute ticker + midnight date ----
  static int lastMinute = -1;
  static int lastDay = -1;
  struct tm t;
  if (getLocalTime(&t, 100)) {
    if (t.tm_min != lastMinute) {
      lastMinute = t.tm_min;
      drawTimeOnly();
      maybeCleanGhosts();
    }
    if (t.tm_mday != lastDay) {
      lastDay = t.tm_mday;
      drawDateOnly();
    }
  }

  // ---- power handling ----
  if (powerMode == 1) {
    esp_sleep_enable_timer_wakeup(1000000ULL);
    esp_light_sleep_start();
  } else if (powerMode == 2) {
    time_t now = time(nullptr);
    uint64_t secs = 60 - (now % 60);
    if (secs < 2) secs = 2;
    Serial.printf("[power] deep sleep %llu s\n", (unsigned long long)secs);
    Serial.flush();

    blinkAwake(1);   // green blink while awake (no-op if RGB disabled)

    esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
    esp_deep_sleep_enable_gpio_wakeup((1ULL << WAKE_BTN), ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
  } else {
    delay(50);
  }
}

