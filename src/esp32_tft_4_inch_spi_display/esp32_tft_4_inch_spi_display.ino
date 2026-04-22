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
#define MAX_TRANSFER_TIME 6
#define MIN_TO_ARRIVE_THRESHOLD 3

bool serialPrintForDebugPurpose = false;

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
  int status = 0;                     // 0=ok, -1=json, -2=http
  int httpErrorCode = 0;              // http status
  DynamicJsonDocument* doc = nullptr;
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
    apiData[i].httpErrorCode = 0;
    if (apiData[i].doc) {
      delete apiData[i].doc;
      apiData[i].doc = nullptr;
    }
}


template <typename... Args>
void serialPrintfDebug(const char* dbgMsg, Args... args) {
  if (serialPrintForDebugPurpose) {
    Serial.printf(dbgMsg, args...);
  }
}


void handleApiError(int apiIndex, int apiStatus, HTTPClient& http, WiFiClientSecure& client, int delayMs = 500) {
  Serial.printf("\t\tERROR: API %d failed\n", apiIndex);
  apiData[apiIndex].status = apiStatus;
  http.end();
  client.stop();
  delay(delayMs);
}


// Fetch ALL APIs -> store in global apiData[]
void fetchApiData() {
  for (int i = 0; i < NUM_APIS; i++) {
    serialPrintfDebug<int>("API: %i\n", i);

    for (int attempt = 0; attempt <= 2; attempt++) {
      serialPrintfDebug<int>("\tATTEMPT: %i\n", attempt);

      cleanupApiData(i);

      // Fresh client EVERY TIME
      WiFiClientSecure client;
      client.setInsecure();
      client.setTimeout(30000);
      
      HTTPClient http;
      http.useHTTP10(true);
      http.setReuse(false);
      http.begin(client, apis[i].url);
      http.addHeader("User-Agent", "ESP32-Display/1.0");
      http.setTimeout(20000);

      int httpCode = http.GET();
      yield();  // Keep connection alive; prevents ESP32 watchdog resets
      serialPrintfDebug<int>("\tHTTP-Code GET: %i\n", httpCode);

      // ERROR Handling
      if (httpCode != HTTP_CODE_OK) {
        Serial.printf("\t\tERROR: HTTP %i\n", httpCode);
        apiData[i].httpErrorCode = httpCode;
        handleApiError(i, -2, http, client);
        continue;
      }

      String payload = http.getString();
      yield();  // Keep connection alive; prevents ESP32 watchdog resets
      serialPrintfDebug<int>("\tPayload bytes: %d\n", payload.length());
      serialPrintfDebug<const char*>("\tPayload preview: %.120s\n", payload.c_str());
      
      // ERROR Handling
      if (payload.length() == 0) {
        Serial.printf("\t\tERROR: Empty payload (EmptyInput likely)\n");
        handleApiError(i, -1, http, client);
        continue;
      }

      // ERROR Handling
      if (payload[0] != '{' && payload[0] != '[') {
        Serial.printf("\t\tERROR: Payload JSON object/array; heuristic: must start with '{' or '['\n");
        handleApiError(i, -1, http, client);
        continue;
      }

      apiData[i].doc = new DynamicJsonDocument(apis[i].jsonCapacity);
      DeserializationError error = deserializeJson(*apiData[i].doc, payload);

      if (error) {
        Serial.printf("\tJSON error: %s\n", error.c_str());
        delete apiData[i].doc; 
        apiData[i].doc = nullptr;
        handleApiError(i, -1, http, client);
        continue;
      }

      String preview;
      serializeJson(*apiData[i].doc, preview);
      serialPrintfDebug<const char*>("\tJSON preview: %.120s\n", preview.c_str());
      serialPrintfDebug<int>("\tJSON length: %d\n", preview.length());

      apiData[i].status = 0;
      serialPrintfDebug<int>("\t\tAPI Status OK: %i\n", apiData[i].status);

      http.end();
      client.stop();

      delay(500);
      break;
    }
  }
}


// Display DEPARTURES (API 0)
void displayDepartures() {
  if (apiData[0].status != 0 || !apiData[0].doc) {
    tft.println("No departures ...");
    return;
  }
  
  JsonArray deps = (*apiData[0].doc)["departures"];
  serialPrintfDebug<int>("-> Departures array size: %d\n", deps.size());  // Debug!
  
  if (deps.isNull()) return;
  
  if (deps.isNull() || deps.size() == 0) {
    tft.println("No departures found ...");
    return;
  }

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
    
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
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

  if (apiData[1].status != 0 || !apiData[1].doc) {
    tft.drawString("No journeys data ...", 0, DEPARTURES_DISPLAY_LIMIT * (tft.fontHeight() + 3));
    return;
  }
  
  JsonArray journeys = (*apiData[1].doc)["journeys"];
  serialPrintfDebug<int>("-> Journeys array size: %d\n", journeys.size());  // Debug!

  if (journeys.isNull() || journeys.size() == 0) {
    tft.drawString("No journeys found ...", 0, DEPARTURES_DISPLAY_LIMIT * (tft.fontHeight() + 3));
    return;
  }

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
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString(description, 65, count * (tft.fontHeight() + 3));

      description = decodeUtf8(destination) + delta_time;
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
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
  delay(500);

  // ====== Connect WiFi
  WiFi.begin(ssid, password);
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
  tft.printf("Connecting WiFi");
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
    delay(3000);
    // don't restart; let loop() retry later or just spin
    return;  // let loop() handle next retry
  }

  Serial.println("WiFi connected");
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
  tft.println("WiFi connected");

  // ====== Configure time for Europe/Berlin
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  delay(500);
}


void loop() {
  fetchApiData();        // Fetch ALL APIs once
  displayAllData();      // Display everything

  delay(60000);
}
