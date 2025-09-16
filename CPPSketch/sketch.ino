#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <vector>
#include <algorithm>

// ---------- USER CONFIG ----------
const char* WIFI_SSID = "SSID";
const char* WIFI_PASS = "Password";
String TARGET_STATION  = "";
const uint32_t REFRESH_MS = 20000;
const uint32_t SCROLL_MS  = 40;

HUB75_I2S_CFG mxconfig(64, 32, 1);
MatrixPanel_I2S_DMA* display;

uint16_t COLOR_ORANGE;

const int Y_HEADER = 1;
const int Y_LINE1  = 12;
const int Y_LINE2  = 24;
const int SCREEN_W = 64;

String line1 = "", line2 = "";
int16_t line1W = 0, line2W = 0;
int scrollX = SCREEN_W;
unsigned long lastFetch  = 0;
unsigned long lastScroll = 0;

// Async fetch support
volatile bool fetchInProgress = false;
volatile bool newLinesReady = false;
String pendingLine1 = "";
String pendingLine2 = "";
TaskHandle_t fetchTaskHandle = nullptr;

// Forward declarations
void startFetchTask();
void fetchTask(void*);

std::vector<String> stationNames;

AsyncWebServer server(80);

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
  display->setCursor(1, Y_HEADER);
  display->print(TARGET_STATION);
  bool s1 = (line1W > SCREEN_W);
  bool s2 = (line2W > SCREEN_W);
  display->setCursor(s1 ? scrollX : 1, Y_LINE1); display->print(line1);
  display->setCursor(s2 ? scrollX : 1, Y_LINE2); display->print(line2);
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
  if (!http.begin(client, url)) {
    Serial.println("[station] http.begin failed");
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  int code = http.POST((uint8_t*)nullptr, 0);
  if (code != 200) {
    Serial.print("[station] POST status: "); Serial.println(code);
    http.end();
    return false;
  }
  // Read entire payload first (prevents IncompleteInput)
  WiFiClient * stream = http.getStreamPtr();
  String payload; payload.reserve(4096);
  uint32_t start = millis();
  const uint32_t READ_TIMEOUT_MS = 4000;
  while (http.connected() && (millis()-start) < READ_TIMEOUT_MS) {
    while (stream->available()) {
      payload += (char)stream->read();
    }
    if (!stream->available()) delay(3);
  }
  http.end();
  if (payload.isEmpty()) {
    Serial.println("[station] Empty response body");
    return false;
  }
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("[station] JSON error: "); Serial.println(err.c_str());
    return false;
  }
  return true;
}

// Fetch station list (returns true on success). Tries POST then falls back to GET.
bool fetchStationList() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  const char* url = "https://api.irishtnt.com/stations/";
  if (!http.begin(client, url)) {
    Serial.println("[stations] http.begin failed");
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  int code = http.POST((uint8_t*)nullptr, 0);
  if (code != 200) {
    Serial.print("[stations] POST status: "); Serial.println(code);
    http.end();
    return false;
  }
  int contentLen = http.getSize(); // may be -1 (chunked)
  Serial.print("[stations] HTTP 200 (POST), Content-Length: "); Serial.println(contentLen);
  WiFiClient * stream = http.getStreamPtr();
  String payload; payload.reserve(contentLen > 0 && contentLen < 70000 ? contentLen : 30000);
  uint32_t start = millis();
  const uint32_t READ_TIMEOUT_MS = 6000;
  // Read until length reached OR connection closes OR timeout
  while (http.connected() && (millis()-start) < READ_TIMEOUT_MS) {
    while (stream->available()) {
      payload += (char)stream->read();
    }
    if (contentLen > 0 && (int)payload.length() >= contentLen) break;
    if (!stream->available()) delay(4);
  }
  http.end();
  Serial.print("[stations] Bytes received: "); Serial.println(payload.length());
  if (payload.length() == 0) {
    Serial.println("[stations] Empty response body");
    return false;
  }
  if (contentLen > 0 && (int)payload.length() != contentLen) {
    Serial.println("[stations] Warning: received length mismatch (possible truncation)");
  }
  // Determine structure: array root or object with "stations" array
  bool rootIsArray = payload.startsWith("[");
  bool rootIsObject = !rootIsArray && payload.startsWith("{");
  DynamicJsonDocument doc((payload.length() + 2048) > 65536 ? 65536 : (payload.length() + 2048));
  DeserializationError err = deserializeJson(doc, payload);
  JsonArray arr; // will point to stations
  if (!err) {
    if (rootIsArray && doc.is<JsonArray>()) {
      arr = doc.as<JsonArray>();
    } else if (rootIsObject && doc.is<JsonObject>() && doc["stations"].is<JsonArray>()) {
      arr = doc["stations"].as<JsonArray>();
    } else {
      Serial.println("[stations] JSON root shape unexpected");
    }
  } else {
    Serial.print("[stations] JSON parse error: "); Serial.println(err.c_str());
  }
  stationNames.clear();
  if (!arr.isNull()) {
    for (JsonObject st : arr) {
      const char* name = st["name"];
      if (name && *name) stationNames.push_back(String(name));
    }
  }
  if (stationNames.empty()) {
    // Fallback manual extraction of "name" tokens
    const char * key = "\"name\""; size_t klen = 6; int idx = 0; int safety = 0;
    while ((idx = payload.indexOf(key, idx)) != -1 && safety < 3000) {
      safety++;
      int colon = payload.indexOf(':', idx + klen); if (colon == -1) break;
      int firstQuote = payload.indexOf('"', colon+1); if (firstQuote == -1) break;
      int secondQuote = firstQuote+1;
      while (secondQuote < (int)payload.length()) {
        if (payload.charAt(secondQuote) == '"' && payload.charAt(secondQuote-1) != '\\') break;
        secondQuote++;
      }
      if (secondQuote >= (int)payload.length()) break;
      String name = payload.substring(firstQuote+1, secondQuote);
      if (name.length()>0) stationNames.push_back(name);
      idx = secondQuote+1;
    }
    Serial.print("[stations] Fallback names extracted: "); Serial.println(stationNames.size());
  }
  if (stationNames.empty()) {
    Serial.println("[stations] No station names found");
    int tailStart = payload.length() > 140 ? payload.length()-140 : 0;
    Serial.println(payload.substring(0,120));
    Serial.println("...tail...");
    Serial.println(payload.substring(tailStart));
    return false;
  }
  Serial.print("[stations] Loaded stations: "); Serial.println(stationNames.size());
  return true;
}

// Normalize direction
String normalizeDirection(const String& dir) {
  if (dir.startsWith("To ")) return dir.substring(3);
  return dir;
}

// update lines
void updateLinesFromJson(const JsonDocument& doc) {
  String newL1 = "", newL2 = "";
  JsonObjectConst station = doc["station"].as<JsonObjectConst>();
  JsonArrayConst trains   = station["trains"].as<JsonArrayConst>();
  if (station.isNull() || trains.isNull() || trains.size()==0) {
    newL1 = "No trains";
  } else {
    struct TrainPick { String dir; String dest; int due; };
    std::vector<TrainPick> picks; // one per direction (earliest)
    for (JsonObjectConst t : trains) {
      const char* destC = t["destination"] | "";
      const char* dirC  = t["direction"]   | "";
      if (!destC || !dirC) continue;
      if (String(destC) == TARGET_STATION) continue; // skip trains terminating here
      String normDir = normalizeDirection(String(dirC));
      int dueIn = atoi((t["dueIn"] | "0"));
      int late  = atoi((t["late"]  | "0"));
      int due   = dueIn + late; if (due < 0) due = 0;
      // Find existing direction
      bool found=false;
      for (auto &pk : picks) {
        if (pk.dir == normDir) {
          if (due < pk.due) { // earlier train for this direction
            pk.due = due; pk.dest = String(destC);
          }
          found=true; break;
        }
      }
      if (!found) {
        picks.push_back({normDir, String(destC), due});
      }
    }
    if (picks.empty()) {
      newL1 = "No services";
    } else {
      std::sort(picks.begin(), picks.end(), [](const TrainPick& a, const TrainPick& b){return a.due < b.due;});
      newL1 = picks[0].dest + ":  " + String(picks[0].due) + " min";
      if (picks.size() > 1) newL2 = picks[1].dest + ":  " + String(picks[1].due) + " min";
    }
  }
  pendingLine1 = newL1;
  pendingLine2 = newL2;
  newLinesReady = true;
}

// build HTML page
String buildPage() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Station</title></head><body>";
  html += "<h2>Select Station</h2><form action='/set'>";
  html += "<select name='station'>";
  for (auto& n : stationNames) {
    html += "<option value='" + n + "'";
    if (n == TARGET_STATION) html += " selected";
    html += ">" + n + "</option>";
  }
  html += "</select><input type='submit' value='Go'></form>";
  html += "<p>Current: " + TARGET_STATION + "</p>";
  html += "</body></html>";
  return html;
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  setCustomPins();
  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  display->setBrightness8(80);
  COLOR_ORANGE = display->color565(255, 140, 0);
  line1="Matrix OK"; line2="WiFi..."; measureTextWidth(line1,line1W);measureTextWidth(line2,line2W);scrollX=SCREEN_W;drawFrame();
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASS);
  uint32_t t0=millis(); while(WiFi.status()!=WL_CONNECTED && millis()-t0<15000) delay(200);
  if(WiFi.status()!=WL_CONNECTED){line1="WiFi FAIL";line2="";} else {line1="WiFi OK";line2=WiFi.localIP().toString();}
  measureTextWidth(line1,line1W);measureTextWidth(line2,line2W);scrollX=SCREEN_W;drawFrame();
  if(WiFi.status()==WL_CONNECTED) {
    if(!fetchStationList()) {
      // Add placeholder so dropdown renders something
      if (stationNames.empty()) stationNames.push_back("(no stations)");
    }
  }

  // Web server handlers
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
    req->send(200,"text/html",buildPage());
  });
  server.on("/set", HTTP_GET, [](AsyncWebServerRequest *req){
    if(req->hasParam("station")){
      TARGET_STATION=req->getParam("station")->value();
      scrollX=SCREEN_W; // reset scroll
    }
    req->redirect("/");
  });
  // Manual refresh endpoint in case list failed at boot
  server.on("/refresh", HTTP_GET, [](AsyncWebServerRequest *req){
    bool ok = fetchStationList();
    String msg = ok ? "Refreshed OK" : "Refresh FAILED";
    req->send(200, "text/plain", msg);
  });
  server.begin();
  lastFetch=0; lastScroll=millis();
  // Create fetch task pinned to core 0 (loop usually on core 1)
  xTaskCreatePinnedToCore(fetchTask, "fetchTask", 8192, nullptr, 1, &fetchTaskHandle, 0);
}

// ---------- LOOP ----------
void loop() {
  unsigned long now=millis();
  if(now-lastFetch>=REFRESH_MS && WiFi.status()==WL_CONNECTED){
    if(!fetchInProgress && TARGET_STATION.length()>0 && TARGET_STATION != "(no stations)") {
      fetchInProgress = true; // signal task to do a cycle
    }
    lastFetch=now;
  }
  // Apply new lines if ready (main thread only touches display vars)
  if (newLinesReady) {
    newLinesReady = false;
    bool changed=false;
    if (pendingLine1!=line1){line1=pendingLine1;measureTextWidth(line1,line1W);changed=true;}
    if (pendingLine2!=line2){line2=pendingLine2;measureTextWidth(line2,line2W);changed=true;}
    if (changed) scrollX=SCREEN_W;
  }
  tickScroll();
  drawFrame();
  delay(10);
}

// ---------------- ASYNC FETCH TASK ----------------
void fetchTask(void* ) {
  for(;;) {
    if (fetchInProgress) {
      if (WiFi.status()==WL_CONNECTED && TARGET_STATION.length()>0 && TARGET_STATION != "(no stations)") {
        DynamicJsonDocument doc(20000);
        String stationPath=urlEncodeSpacesLower(TARGET_STATION);
        if(fetchStationJson(stationPath,doc)) {
          updateLinesFromJson(doc);
        } else {
          pendingLine1 = "API ERR"; pendingLine2 = ""; newLinesReady = true;
        }
      }
      fetchInProgress = false;
    }
    vTaskDelay(pdMS_TO_TICKS(50)); // yield
  }
}

void startFetchTask() {
  // Not used (task started directly in setup), kept for potential future control
}
