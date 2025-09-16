/*
  ESP32 + 64x32 HUB75 + Irish Rail JSON wrapper + Web Form
  - All text orange
  - Header (TARGET_STATION) at top (baseline 7 to avoid clipping)
  - Two departure lines moved up by 8px
  - Both long lines scroll in sync
  - Web page lets you change TARGET_STATION without re-uploading code
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <WebServer.h>
#include <vector>

// ---------- USER CONFIG ----------
const char* WIFI_SSID = "TUD";
const char* WIFI_PASS = "Password";
String TARGET_STATION  = "";
const uint32_t REFRESH_MS = 20000;   // API refresh period
const uint32_t SCROLL_MS  = 40;      // marquee speed (lower=faster)

// Panel config
HUB75_I2S_CFG mxconfig(64, 32, 1);
MatrixPanel_I2S_DMA* display;

static inline uint16_t c565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
uint16_t COLOR_ORANGE = c565(255, 140, 0);

// Layout (moved up)
const int Y_HEADER = 1;
const int Y_LINE1  = 12;  // was 18 -> now 10 (up by 8px)
const int Y_LINE2  = 24;  // was 28 -> now 20 (up by 8px)
const int SCREEN_W = 64;

// Current lines + widths
String line1 = "";
String line2 = "";
int16_t line1W = 0, line2W = 0;

// Shared scrolling state
int scrollX = SCREEN_W;
unsigned long lastFetch  = 0;
unsigned long lastScroll = 0;

// ---------- Web Server ----------
WebServer server(80);

void handleRoot() {
  String html = R"rawliteral(
  <html>
  <head><title>ESP32 Station</title></head>
  <body>
    <h2>Set Target Station</h2>
    <form action="/set" method="POST">
      <input type="text" name="station" placeholder="Station name">
      <input type="submit" value="Update">
    </form>
    <p>Current target: )rawliteral";
  html += TARGET_STATION;
  html += "</p></body></html>";
  server.send(200, "text/html", html);
}

void handleSet() {
  if (server.hasArg("station")) {
    TARGET_STATION = server.arg("station");
    lastFetch = 0;  // force immediate refresh
    String html = "<html><body><h2>Updated!</h2>"
                  "<p>New target: " + TARGET_STATION + "</p>"
                  "<a href='/'>Back</a></body></html>";
    server.send(200, "text/html", html);
  } else {
    server.send(400, "text/plain", "No station provided");
  }
}

// ---------- PIN MAP ----------
void setCustomPins() {
  mxconfig.gpio.r1 = 25;  mxconfig.gpio.g1 = 26;  mxconfig.gpio.b1 = 27;
  mxconfig.gpio.r2 = 14;  mxconfig.gpio.g2 = 21;  mxconfig.gpio.b2 = 13;

  mxconfig.gpio.a  = 23;  mxconfig.gpio.b  = 19;
  mxconfig.gpio.c  = 5;   mxconfig.gpio.d  = 17;

  mxconfig.gpio.clk = 16;
  mxconfig.gpio.lat = 4;
  mxconfig.gpio.oe  = 22;
}

// ---------- UTILS ----------
static String urlEncodeSpacesLower(const String& s) {
  String o = s; o.toLowerCase(); o.replace(" ", "%20"); return o;
}
void measureTextWidth(const String& s, int16_t& w) {
  int16_t x1, y1; uint16_t w16, h16;
  display->getTextBounds((char*)s.c_str(), 0, 0, &x1, &y1, &w16, &h16);
  w = (int16_t)w16;
}

void drawFrame() {
  display->fillScreen(0);
  display->setTextWrap(false);
  display->setTextSize(1);
  display->setTextColor(COLOR_ORANGE);

  // Header
  display->setCursor(1, Y_HEADER);
  display->print(TARGET_STATION);

  bool s1 = (line1W > SCREEN_W);
  bool s2 = (line2W > SCREEN_W);

  display->setCursor(s1 ? scrollX : 1, Y_LINE1);
  display->print(line1);

  display->setCursor(s2 ? scrollX : 1, Y_LINE2);
  display->print(line2);
}

void tickScroll() {
  unsigned long now = millis();
  if (now - lastScroll < SCROLL_MS) return;
  lastScroll = now;

  if (line1W <= SCREEN_W && line2W <= SCREEN_W) return;

  scrollX -= 1;

  int16_t maxW = (line1W > line2W) ? line1W : line2W;
  if (scrollX < -maxW) scrollX = SCREEN_W;
}

// ---------- NETWORK ----------
bool fetchStationJson(const String& stationLowerPath, JsonDocument& doc) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = "https://api.irishtnt.com/stations/name/" + stationLowerPath + "/90";
  if (!http.begin(client, url)) return false;

  int code = http.POST((uint8_t*)nullptr, 0);
  if (code != 200) { http.end(); return false; }

  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  return !err;
}

// ---------- JSON HELPERS ----------
void getStationDirections(JsonArrayConst trains, const String& targetStation,
                          std::vector<String>& directions) {
  directions.clear();
  for (JsonObjectConst t : trains) {
    const char* dest = t["destination"] | "";
    const char* dir  = t["direction"]   | "";
    if (!dest || !dir) continue;
    if (String(dest) == targetStation) continue;
    bool seen = false;
    for (auto& d : directions) if (d == String(dir)) { seen = true; break; }
    if (!seen) directions.push_back(String(dir));
  }
}

bool getNextTrainForGivenDirection(JsonArrayConst trains, const String& targetStation,
                                   const String& direction, String& destOut, int& dueOut) {
  for (JsonObjectConst t : trains) {
    const char* dest = t["destination"] | "";
    const char* dir  = t["direction"]   | "";
    if (!dest || !dir) continue;
    if (String(dest) == targetStation) continue;
    if (String(dir) == direction) {
      int dueIn = atoi((t["dueIn"] | "0"));
      int late  = atoi((t["late"]  | "0"));
      destOut = String(dest);
      dueOut  = dueIn + late;
      return true;
    }
  }
  return false;
}

void updateLinesFromJson(const JsonDocument& doc) {
  String newL1 = "", newL2 = "";

  JsonObjectConst station = doc["station"].as<JsonObjectConst>();
  JsonArrayConst trains   = station["trains"].as<JsonArrayConst>();

  if (station.isNull() || trains.isNull() || trains.size()==0) {
    newL1 = "No trains";
  } else {
    std::vector<String> directions;
    getStationDirections(trains, TARGET_STATION, directions);

    int shown = 0;
    for (auto& dir : directions) {
      String dest; int due = 0;
      if (getNextTrainForGivenDirection(trains, TARGET_STATION, dir, dest, due)) {
        String line = dest + ":  " + String(due) + " min";
        if (shown == 0) newL1 = line;
        else if (shown == 1) newL2 = line;
        if (++shown == 2) break;
      }
    }
    if (shown == 0) newL1 = "No services";
  }

  bool changed = false;
  if (newL1 != line1) { line1 = newL1; measureTextWidth(line1, line1W); changed = true; }
  if (newL2 != line2) { line2 = newL2; measureTextWidth(line2, line2W); changed = true; }

  if (changed) scrollX = SCREEN_W;
}

// ---------- SETUP & LOOP ----------
void setup() {
  Serial.begin(115200);

  setCustomPins();
  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  display->setBrightness8(80);
  COLOR_ORANGE = display->color565(255, 140, 0);

  line1 = "Matrix OK";
  line2 = "WiFi...";
  measureTextWidth(line1, line1W);
  measureTextWidth(line2, line2W);
  scrollX = SCREEN_W;
  drawFrame();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);

  if (WiFi.status() != WL_CONNECTED) { line1 = "WiFi FAIL"; line2 = ""; }
  else { line1 = "WiFi OK"; line2 = WiFi.localIP().toString(); }
  measureTextWidth(line1, line1W);
  measureTextWidth(line2, line2W);
  scrollX = SCREEN_W;
  drawFrame();

  // Start web server
  server.on("/", handleRoot);
  server.on("/set", HTTP_POST, handleSet);
  server.begin();
  Serial.println("Web server started");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Open http://");
    Serial.print(WiFi.localIP());
    Serial.println("/ in your browser");
  }

  lastFetch = 0;
  lastScroll = millis();
}

void loop() {
  server.handleClient();   // serve web requests

  unsigned long now = millis();

  if (now - lastFetch >= REFRESH_MS && WiFi.status() == WL_CONNECTED) {
    DynamicJsonDocument doc(16384);
    String stationPath = urlEncodeSpacesLower(TARGET_STATION);
    if (fetchStationJson(stationPath, doc)) {
      updateLinesFromJson(doc);
    } else {
      line1 = "API ERR"; line2 = "";
      measureTextWidth(line1, line1W);
      measureTextWidth(line2, line2W);
      scrollX = SCREEN_W;
    }
    lastFetch = now;
  }

  tickScroll();
  drawFrame();
  delay(10);
}
