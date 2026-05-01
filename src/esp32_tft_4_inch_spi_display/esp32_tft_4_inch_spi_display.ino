#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

#define DEPARTURES_FETCH_LIMIT 20
#define JOURNEYS_FETCH_LIMIT 4
#define JOURNEYS_TRANSFERS_LIMIT 1
#define DEPARTURES_DISPLAY_LIMIT 11
#define JOURNEYS_DISPLAY_LIMIT 1
#define MAX_TRANSFER_TIME 10
#define MIN_TO_ARRIVE_THRESHOLD 3

bool serialPrintForDebugPurpose = false;

unsigned long lastFetchMs = 0UL;
unsigned long lastDisplayMs = 0UL;

// ====== WiFi credentials
const char* ssid = "";
const char* password = "";

// Configurable API endpoints
struct ApiConfig {
  String url;
  size_t jsonCapacity;
  int displayLimit;
};

// VBB API configurations
const ApiConfig apis[] = {
  { "https://v6.vbb.transport.rest/stops/900096458/departures?duration=60&remarks=false&results=" + String(DEPARTURES_FETCH_LIMIT), 32768, DEPARTURES_DISPLAY_LIMIT },  // departures
  { "https://v6.vbb.transport.rest/journeys?duration=60&from=900096458&to=900001202&products=U&suburban=false&transfers=" + String(JOURNEYS_TRANSFERS_LIMIT) + "&via=900009203&remarks=false&verkehrsunternehmen=false&polyline=false&language=en&format=compact&results=" + String(JOURNEYS_FETCH_LIMIT), 65536, JOURNEYS_DISPLAY_LIMIT }  // journeys
};

const int NUM_APIS = sizeof(apis) / sizeof(apis[0]);

// Global data storage (fetched once per loop)
struct ApiData {
  int status = 0;                     // 1=valid, 0=fetch-ok, -1=json, -2=http
  bool stale = false;
  int httpErrorCode = 0;              // http status
  DynamicJsonDocument* temp = nullptr;
  DynamicJsonDocument* doc = nullptr;
  unsigned long lastSuccessMs = 0UL;
  bool INIT_FETCH_VALID = false;
};

ApiData apiData[NUM_APIS] = {};

TFT_eSPI tft = TFT_eSPI();

// Helper: Parse ISO string -> time_t
time_t parseIsoToTime(const String& isoTimeStr) {
  if (isoTimeStr.length() < 19) return (time_t)-1;
  
  struct tm t = {0};
  t.tm_year  = isoTimeStr.substring(0, 4).toInt() - 1900;
  t.tm_mon   = isoTimeStr.substring(5, 7).toInt() - 1;
  t.tm_mday  = isoTimeStr.substring(8, 10).toInt();
  t.tm_hour  = isoTimeStr.substring(11, 13).toInt();
  t.tm_min   = isoTimeStr.substring(14, 16).toInt();
  t.tm_sec   = isoTimeStr.substring(17, 19).toInt();
  t.tm_isdst = -1;
  
  return mktime(&t);
}


String timeToIsoString(time_t t) {
  struct tm tmTime;
  localtime_r(&t, &tmTime);

  char buf[25];  // "2026-04-24T13:05:00"
  strftime(buf, sizeof(buf), "%Y-%m-%iT%H:%M:%S", &tmTime);
  return String(buf);
}


// Get current time as time_t (cached)
time_t getCurrentTime() {
  struct tm nowTm;
  if (!getLocalTime(&nowTm)) {
    Serial.println("Failed to get current time");
    return (time_t)-999;
  }
  return mktime(&nowTm);
}


// Minutes from now -> target time
int getMinutesToDeparture(const String& isoTimeStr) {
  time_t arrival = parseIsoToTime(isoTimeStr);
  if (arrival < 0) return -999;
  
  time_t now = getCurrentTime();
  if (now < 0) return -999;
  
  return round(difftime(arrival, now) / 60.0);
}


// Minutes between two times
int getMinutesBetween(const String& startIsoStr, const String& endIsoStr) {
  time_t start = parseIsoToTime(startIsoStr);
  time_t end   = parseIsoToTime(endIsoStr);
  
  if (start < 0 || end < 0) return -999;
  
  return round(difftime(end, start) / 60.0);
}


String decodeUtf8(String input) {
  input.replace("\u00e4", "ae");
  input.replace("\u00f6", "oe");
  input.replace("\u00fc", "ue");
  input.replace("\u00c4", "Ae");
  input.replace("\u00d6", "Oe");
  input.replace("\u00dc", "Ue");
  input.replace("\u00df", "ss");

  for (int i = 0; i < input.length(); i++) {
    if (input[i] < 32 || input[i] > 126) input[i] = '?';
  }

  return input;
}


uint16_t getBackgroundColor(String product) {
  uint16_t color = TFT_BLACK;

  if (product == "bus")
    color = TFT_MAGENTA;
  else if (product == "suburban")
    color = TFT_DARKGREEN;
  else if (product == "subway")
    color = TFT_BLUE;
  else if (product == "tram")
    color = TFT_RED;
  else if (product == "ferry")
    color = TFT_SKYBLUE;

  return color;
} 


void showCountdownMessage(int seconds, const char* message) {
  for (int i = seconds; i >= 0; i--) {
    tft.fillRect(tft.width() / 4, tft.height() / 4, tft.width() * 3 / 4, tft.height() * 3 / 4, TFT_BLACK);
    String msg = String(message) + " " + String(i) + " sec...";
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(msg, tft.width() / 2, tft.height() / 2);
    delay(1000);
  }
}


void displayError(const char* message) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.println("Error:");
  tft.println(message);

  showCountdownMessage(3, "Restart in");
}


void displayWiFiOk() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.println("WiFi Ok!");

  showCountdownMessage(10, "Loop over in");
}


void cleanupApiData(int i) {
    apiData[i].status = 0;
    apiData[i].stale = false;
    apiData[i].httpErrorCode = 0;
    apiData[i].INIT_FETCH_VALID = false;
    apiData[i].lastSuccessMs = 0UL;
    if (apiData[i].doc) {
      delete apiData[i].doc;
      apiData[i].doc = nullptr;
    }
    if (apiData[i].temp) {
      delete apiData[i].temp;
      apiData[i].temp = nullptr;
    }
}


template <typename... Args>
void serialPrintfDebug(const char* dbgMsg, Args... args) {
  if (serialPrintForDebugPurpose) {
    Serial.printf(dbgMsg, args...);
  }
}


void handleApiError(int apiIndex, int apiStatus, HTTPClient& http, WiFiClientSecure& client, int delayMs = 100) {
  serialPrintfDebug<int, int>("\t\t ERROR: API<%i> failed to fetch; STATUS: %i\n", apiIndex, apiStatus);
  apiData[apiIndex].status = apiStatus;
  http.end();
  client.stop();
  delay(delayMs);
}


// Fetch ALL APIs -> store in global apiData[]
void fetchApiData() {
  serialPrintfDebug("Fetching...\n");
  for (int i = 0; i < NUM_APIS; i++) {
    serialPrintfDebug<int>("API: %i\n", i);

    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      delay(2000);
      if (WiFi.status() != WL_CONNECTED) 
        serialPrintfDebug("Failed to reconnect WiFi, restart!\n");
        ESP.restart();
    }

    for (int attempt = 0; attempt <= 2; attempt++) {
      serialPrintfDebug<int>("\t ATTEMPT: %i\n", attempt);

      // Fresh client EVERY TIME
      WiFiClientSecure client;
      client.setInsecure();
      client.setTimeout(10000);
      
      HTTPClient http;
      http.useHTTP10(true); // use HTTP/1.0 instead of default HTTP/1.1; HTTP/1.0 avoids trying to keep socket alive -> client typically sends Connection: close
      http.setReuse(false); // HTTPClient forced not to reuse TCP connection for next request; reduces bugs where ESP32 accidentally reuses a stale or wrong connection
      http.begin(client, apis[i].url);
      http.addHeader("User-Agent", "ESP32-Display/1.0");
      http.setTimeout(10000);

      unsigned long t = millis();
      int httpCode = http.GET();
      apiData[i].httpErrorCode = httpCode;
      serialPrintfDebug<unsigned long>("\t http.GET() took %lu ms\n", millis() - t);
      yield();  // Keep connection alive; prevents ESP32 watchdog resets
      serialPrintfDebug<int>("\t HTTP-Code GET: %i\n", httpCode);

      // ERROR Handling
      if (httpCode != HTTP_CODE_OK) {
        handleApiError(i, -2, http, client);
        continue;
      }

      t = millis();
      String payload = http.getString();
      serialPrintfDebug<unsigned long>("\t http.getString() took %lu ms\n", millis() - t);
      //yield();  // Keep connection alive; prevents ESP32 watchdog resets
      serialPrintfDebug<int>("\t Payload bytes: %i\n", payload.length());
      serialPrintfDebug<int>("\t Expected bytes: %i\n", http.getSize());
      serialPrintfDebug<const char*>("\t Payload preview: %.60s\n", payload.c_str());
      
      // ERROR Handling
      if (payload.length() == 0) {
        serialPrintfDebug("\t\t ERROR: Empty payload (EmptyInput likely)\n");
        handleApiError(i, -1, http, client);
        continue;
      }

      // ERROR Handling
      if (payload[0] != '{' && payload[0] != '[') {
        serialPrintfDebug("\t\t ERROR: Payload JSON must start with '{' or '['\n");
        handleApiError(i, -1, http, client);
        continue;
      }

      apiData[i].temp = new DynamicJsonDocument(apis[i].jsonCapacity);
      DeserializationError error = deserializeJson(*apiData[i].temp, payload);

      if (error) {
        serialPrintfDebug<const char*>("\t ERROR: deserializeJson: %s\n", error.c_str());
        handleApiError(i, -1, http, client);
        continue;
      }

      if (serialPrintForDebugPurpose && !error) {
        String preview;
        serializeJson(*apiData[i].temp, preview);
        serialPrintfDebug<const char*>("\t JSON preview: %.60s\n", preview.c_str());
        serialPrintfDebug<int>("\t JSON length: %i\n", preview.length());
      }

      apiData[i].status = 0;
      serialPrintfDebug<int>("\t Status-Code: %i\n", apiData[i].status);

      http.end();
      client.stop();

      delay(500);
      break;
    }
  }
}


bool validateDeparturesDoc(DynamicJsonDocument& doc) {
  serialPrintfDebug("\t Validating departures...\n");
    if (doc.isNull()) {
    serialPrintfDebug("\t ERROR: DynamicJsonDocument doc is NULL\n");
    return false;
  }

  if (!doc["departures"].is<JsonArray>()) {
    serialPrintfDebug("\t ERROR: Departures not of type <JsonArray>\n");
    return false;
  }

  JsonArray deps = doc["departures"].as<JsonArray>();
  if (deps.isNull()) {
    serialPrintfDebug("\t ERROR: Departures <JsonArray> is NULL\n");
    return false;
  }
  
  if (deps.size() == 0) {
    serialPrintfDebug("\t ERROR: Departures <JsonArray> size 0\n");
    return false;
  }

  JsonObject first = deps[0];
  if (first.isNull()) {
    serialPrintfDebug("\t ERROR: Departures <JsonObject> deps[0] is NULL\n");
    return false;
  }
  if (!first["when"].is<const char*>()) {
    serialPrintfDebug("\t ERROR: Departures <JsonObject> deps[0][\"when\"] is not of type <const char*>\n");
    return false;
  }
  if (!first["line"]["name"].is<const char*>()) {
    serialPrintfDebug("\t ERROR: Departures <JsonObject> deps[0][\"line\"][\"name\"] is not of type <const char*>\n");
    return false;
  }
  if (!first["direction"].is<const char*>()) {
    serialPrintfDebug("\t ERROR: Departures <JsonObject> deps[0][\"direction\"] is not of type <const char*>\n");
    return false;
  }

  return true;
}


bool validateJourneysDoc(DynamicJsonDocument& doc) {
  serialPrintfDebug("\t Validating journeys...\n");
  if (doc.isNull()) {
    serialPrintfDebug("\t ERROR: DynamicJsonDocument doc is NULL\n");
    return false;
  }

  if (!doc["journeys"].is<JsonArray>()) {
    serialPrintfDebug("\t ERROR: Journeys not of type <JsonArray>\n");
    return false;
  }

  JsonArray journeys = doc["journeys"].as<JsonArray>();
  if (journeys.isNull()) {
    serialPrintfDebug("\t ERROR: Journeys <JsonArray> is NULL\n");
    return false;
  }
  if (journeys.size() == 0) {
    serialPrintfDebug("\t ERROR: Journeys <JsonArray> size 0\n");
    return false;
  }

  JsonObject first = journeys[0];
  if (first.isNull()) {
    serialPrintfDebug("\t ERROR: Journeys <JsonObject> journeys[0] is NULL\n");
    return false;
  }
  if (!first["legs"].is<JsonArray>()) {
    serialPrintfDebug("\t ERROR: Journeys <JsonObject> journeys[0][\"legs\"] is not of type <JsonArray>\n");
    return false;
  }

  JsonArray legs = first["legs"].as<JsonArray>();
  if (legs.size() < 1) {
    serialPrintfDebug("\t ERROR: Journeys journeys[0][\"legs\"] size 0\n");
    return false;
  }

  return true;
}


bool validateApiDoc(int apiIndex, DynamicJsonDocument& doc) {
  switch (apiIndex) {
    case 0: return validateDeparturesDoc(doc);
    case 1: return validateJourneysDoc(doc);
    default: return false;
  }
}


void validateApiData() {
  serialPrintfDebug("Validating...\n");
  for (int i = 0; i < NUM_APIS; i++) {

    if (i == 0 && apiData[i].status == 0 && serialPrintForDebugPurpose) {
      JsonArray deps = (*apiData[0].temp)["departures"].as<JsonArray>();
      String preview;
      serializeJson(deps, preview);
      serialPrintfDebug<int, int, const char*>("\t DOC<%i>: %i items: %.80s\n", i, deps.size(), preview.c_str());
    }
    if (i == 1 && apiData[i].status == 0 && serialPrintForDebugPurpose) {
      JsonArray jrny = (*apiData[1].temp)["journeys"].as<JsonArray>();
      String preview;
      serializeJson(jrny, preview);
      serialPrintfDebug<int, int, const char*>("\t DOC<%i>: %i items: %.80s\n", i, jrny.size(), preview.c_str());
    }

    if (apiData[i].status == 0 && validateApiDoc(i, (*apiData[i].temp))) {
      apiData[i].INIT_FETCH_VALID = true;
      apiData[i].status = 1;
      delete apiData[i].doc;
      apiData[i].doc = new DynamicJsonDocument(*apiData[i].temp);

      delete apiData[i].temp;
      apiData[i].temp = nullptr;
      apiData[i].lastSuccessMs = millis(); // set age of successful fetch to current time

      if (i == 0 && serialPrintForDebugPurpose) {
        JsonArray deps = (*apiData[0].doc)["departures"].as<JsonArray>();
        String preview;
        serializeJson(deps, preview);
        serialPrintfDebug<int, int, const char*>("\t\t DOC<%i>: %i items: %.80s\n", i, deps.size(), preview.c_str());
      }
      if (i == 1 && serialPrintForDebugPurpose) {
        JsonArray journeys = (*apiData[1].doc)["journeys"].as<JsonArray>();
        String preview;
        serializeJson(journeys, preview);
        serialPrintfDebug<int, int, const char*>("\t\t DOC<%i>: %i items: %.80s\n", i, journeys.size(), preview.c_str());
      }

      serialPrintfDebug<int>("\t API<%i> Valid Json; LastSuccess: %lu ms\n", i, apiData[i].lastSuccessMs);
    }
    else {
      delete apiData[i].temp;
      apiData[i].temp = nullptr;
      serialPrintfDebug<int>("\t API<%i> Invalid Json; LastSuccess: %lu ms; Age: %i ms\n", i, apiData[i].lastSuccessMs, millis() - apiData[i].lastSuccessMs);
    }
    serialPrintfDebug("\t --------\n");
  }
}


void staleJsonDeparturesDoc(unsigned long ageMs) {
  JsonArray deps = (*apiData[0].doc)["departures"].as<JsonArray>();

  serialPrintfDebug("\t\t Adjusted mins: ");
  for (JsonObject dep : deps) {
    time_t correctedTime = parseIsoToTime(dep["when"].as<String>()) - (ageMs / 60000);
    dep["when"] = timeToIsoString(correctedTime);

    serialPrintfDebug("depWhen: %s\n", dep["when"].as<String>());

    serialPrintfDebug<int>("%i ", getMinutesToDeparture(dep["when"].as<String>()));
  }
  serialPrintfDebug("\n");
}


void staleJsonJourneysDoc(unsigned long ageMs) {
  JsonArray journeys = (*apiData[1].doc)["journeys"].as<JsonArray>();

  serialPrintfDebug("\t\t Adjusted mins: ");
  for (JsonObject jrny : journeys) {
    time_t correctedTime = parseIsoToTime(jrny["legs"][0]["departure"].as<String>()) - (ageMs / 60000);
    jrny["legs"][0]["departure"] = timeToIsoString(correctedTime);

    serialPrintfDebug("jrnyLegsDepartures: %s\n", jrny["legs"][0]["departure"].as<String>());

    serialPrintfDebug<int>("%i ", getMinutesToDeparture(jrny["legs"][0]["departure"].as<String>()));
  }
  serialPrintfDebug("\n");
}


void handleStaleJSON() {
  serialPrintfDebug("Handling stale json...\n");
  for (int i = 0; i < NUM_APIS; i++) {
    if (apiData[i].INIT_FETCH_VALID) {
      unsigned long ageMs = millis() - apiData[i].lastSuccessMs;

      serialPrintfDebug<int, int>("\t API<%i>-STATUS: %i\n", i, apiData[i].status);
      serialPrintfDebug<unsigned long>("\t\t age: %lu\n", ageMs);

      if (apiData[i].status != 1 && ageMs > 60000 && ageMs < 180000) { // handle stale json data (of current invalid fetch)
        apiData[i].stale = true;
        serialPrintfDebug("\t\t Updating with STALE-JSON...\n");
        //if (i == 0) staleJsonDeparturesDoc(ageMs);
        //if (i == 1) staleJsonJourneysDoc(ageMs);
      }
      else if (ageMs > 180000) {
        ESP.restart();  // Instant full reset
        //cleanupApiData(i);
        //serialPrintfDebug("\t\t Cleaned and reseted API struct from outdated JSON!\n");
      }
      else if (apiData[i].status == 1) {
        apiData[i].stale = false;
        serialPrintfDebug("\t\t Object not stale. Continue...\n");
      }
    }
  }
}


// Display DEPARTURES (API 0)
void displayDepartures() {

  if (!apiData[0].doc) {
    tft.println("No departures ...");
    return;
  }
  
  JsonArray deps = (*apiData[0].doc)["departures"];
  serialPrintfDebug<int>("\t -> Departures array size: %i\n", deps.size());  // Debug!
  
  // if (deps.isNull()) return;
  
  // if (deps.isNull() || deps.size() == 0) {
  //   tft.println("No departures found ...");
  //   return;
  // }

  uint16_t tft_color = apiData[0].stale ? TFT_LIGHTGREY : TFT_WHITE;

  int count = 0;
  for (JsonObject dep : deps) {
    if (count >= apis[0].displayLimit) break;
    
    String line = dep["line"]["name"].as<String>();
    String direction = dep["direction"].as<String>();
    String product = dep["line"]["product"].as<String>();
    int min_to_arrive = getMinutesToDeparture(dep["when"].as<String>());
    
    if (min_to_arrive < MIN_TO_ARRIVE_THRESHOLD) continue;
    
    uint16_t bg = getBackgroundColor(product);
    tft.setTextColor(TFT_WHITE, bg);
    tft.drawString(line, 0, count * (tft.fontHeight() + 3));
    
    direction = decodeUtf8(direction).substring(0, 19);
    while (direction.length() < 19) direction += " ";
    
    String delta = (min_to_arrive <= 0) ? "now" : String(min_to_arrive) + "'";
    while (delta.length() < 4) delta = " " + delta;
    
    tft.setTextColor(tft_color, TFT_BLACK);
    tft.drawString(direction + delta, 65, count * (tft.fontHeight() + 3));
    count++;
  }
  
  if (count == 0) tft.println("No departures ...");
}


// Display JOURNEYS (API 1)
void displayJourneys() {
  int count = DEPARTURES_DISPLAY_LIMIT;
  // draw thin separator line
  tft.drawLine(0, (count * (tft.fontHeight() + 3)) - 2, tft.width() - 1, (count * (tft.fontHeight() + 3)) - 2, TFT_WHITE);

  if (!apiData[1].doc) {
    tft.drawString("No journeys data ...", 0, DEPARTURES_DISPLAY_LIMIT * (tft.fontHeight() + 3));
    return;
  }
  
  JsonArray journeys = (*apiData[1].doc)["journeys"];
  serialPrintfDebug<int>("\t -> Journeys array size: %i\n", journeys.size());  // Debug!

  // if (journeys.isNull() || journeys.size() == 0) {
  //   tft.drawString("No journeys found ...", 0, DEPARTURES_DISPLAY_LIMIT * (tft.fontHeight() + 3));
  //   return;
  // }

  uint16_t tft_color = apiData[1].stale ? TFT_LIGHTGREY : TFT_WHITE;

  for (JsonObject jrny : journeys) {

    String line0 = jrny["legs"][0]["line"]["name"].as<String>();
    String direction0 = "HER";
    String product0 = jrny["legs"][0]["line"]["product"].as<String>();

    String line2 = jrny["legs"][2]["line"]["name"].as<String>();
    String destination = jrny["legs"][2]["destination"]["name"].as<String>();
    String product2 = jrny["legs"][2]["line"]["product"].as<String>();

    int min_to_arrive = getMinutesToDeparture(jrny["legs"][0]["departure"].as<String>());
    int transfer_time = getMinutesBetween(jrny["legs"][0]["arrival"].as<String>(), jrny["legs"][2]["departure"].as<String>());

    uint16_t background0 = getBackgroundColor(product0);
    uint16_t background2 = getBackgroundColor(product2);

    if (min_to_arrive >= MIN_TO_ARRIVE_THRESHOLD && transfer_time <= MAX_TRANSFER_TIME) {
      tft.setTextColor(TFT_WHITE, background0);
      tft.drawString(line0, 0, count * (tft.fontHeight() + 3));
      direction0 += " -> ";

      tft.setTextColor(TFT_WHITE, background2);
      tft.drawString(line2, 190, count * (tft.fontHeight() + 3));
      if (destination.length() > 8) destination = destination.substring(0, 8);

      while ((direction0.length()+line2.length()+destination.length()) < 17) destination += " ";

      String delta_time = (min_to_arrive == 0) ? "now" : String(min_to_arrive) + "\'";
      while (delta_time.length() < 4) delta_time = " " + delta_time;

      String description = decodeUtf8(direction0);
      tft.setTextColor(tft_color, TFT_BLACK);
      tft.drawString(description, 65, count * (tft.fontHeight() + 3));

      description = decodeUtf8(destination) + delta_time;
      tft.setTextColor(tft_color, TFT_BLACK);
      tft.drawString(description, 260, count * (tft.fontHeight() + 3));

      count++;
    }

    if (count >= (DEPARTURES_DISPLAY_LIMIT+1)) 
      break;
  }

  if (count == DEPARTURES_DISPLAY_LIMIT) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("No journeys at the time ...", 0, count * (tft.fontHeight() + 3));
  }
}


// Display ALL data (calls individual displays)
void displayAllData() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(3);
  tft.setCursor(0, 0);

  serialPrintfDebug("Displaying...\n");

  displayDepartures();
  displayJourneys();
}


void setup() {
  Serial.begin(115200);
  delay(100);

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
  tft.println("Starting...");
  serialPrintfDebug("Starting...\n");
  delay(500);

  // ====== Connect WiFi
  WiFi.begin(ssid, password);
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
  tft.printf("Connecting WiFi");
  serialPrintfDebug("Connecting WiFi\n");
  delay(500);

  int c = 0;
  while (WiFi.status() != WL_CONNECTED && c < 20) {
    delay(500);
    tft.printf(".");
    c++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(TFT_BLACK);
    tft.println("WiFi failed!");
    serialPrintfDebug("WiFi failed!\n");
    //delay(3000);

    ESP.restart();
    return;  // let loop() handle next retry
  }

  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
  tft.println("WiFi connected");
  serialPrintfDebug("WiFi connected\n");

  // ====== Configure time for Europe/Berlin
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

  // Trigger initial fetches immediately
  lastFetchMs = millis() - 31000;   // Pretend last fetch 29s ago → fetch NOW
  lastDisplayMs = millis() - 61000; // Pretend last display 59s ago → display NOW

  delay(100);
}


void loop() {
  unsigned long now = millis();
  
  // Fetch APIs every 30s
  if (now - lastFetchMs >= 30000) {
    fetchApiData();    // Fetch ALL APIs once
    validateApiData(); // Validate JSON objects
    handleStaleJSON(); // In case fetch failed
    lastFetchMs = now;
  }
  
  // Redraw every 60s (uses latest data)
  if (now - lastDisplayMs >= 60000) {
    displayAllData();  // Display everything
    lastDisplayMs = now;
  }
  
  serialPrintfDebug<int, int, int>("\nFree heap: %d/%d; [%d%% usage]\n", ESP.getFreeHeap(), ESP.getHeapSize(), (1.0-(ESP.getFreeHeap()/(float)ESP.getHeapSize()))*100.0);
  serialPrintfDebug("--------\n");

  delay(15000);
}
