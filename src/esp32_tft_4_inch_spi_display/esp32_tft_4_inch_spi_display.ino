#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

#define DEP_LIMIT 20
#define MIN_TO_ARRIVE_THRESHOLD 3

// ====== WiFi credentials
const char* ssid = "";
const char* password = "";

// ====== VBB API URL
String apiURL = "https://v6.vbb.transport.rest/stops/<station_id>/departures?duration=60&lines=true&line=S25&results=" + String(DEP_LIMIT);

TFT_eSPI tft = TFT_eSPI();


int getMinutesToArrival(const String& isoTimeStr) {
  int year   = isoTimeStr.substring(0, 4).toInt();
  int month  = isoTimeStr.substring(5, 7).toInt();
  int day    = isoTimeStr.substring(8, 10).toInt();
  int hour   = isoTimeStr.substring(11, 13).toInt();
  int minute = isoTimeStr.substring(14, 16).toInt();
  int second = isoTimeStr.substring(17, 19).toInt();

  struct tm arrivalTm = {0};
  arrivalTm.tm_year = year - 1900;
  arrivalTm.tm_mon  = month - 1;
  arrivalTm.tm_mday = day;
  arrivalTm.tm_hour = hour;
  arrivalTm.tm_min  = minute;
  arrivalTm.tm_sec  = second;
  arrivalTm.tm_isdst = -1;

  time_t arrivalTime = mktime(&arrivalTm);

  struct tm nowTm;
  if (!getLocalTime(&nowTm)) {
    Serial.println("Failed to get current time");

    return -999;
  }

  time_t nowTime = mktime(&nowTm);
  int diffSeconds = difftime(arrivalTime, nowTime);

  return round(diffSeconds / 60.0);
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


int fetchAndDisplayDepartures() {
  HTTPClient http;
  http.begin(apiURL);
  int httpCode = http.GET();

  if (httpCode > 0) {
    String payload = http.getString();
    const size_t capacity = 16384;
    DynamicJsonDocument doc(capacity);

    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.print("JSON parsing failed: ");
      Serial.println(error.c_str());
      return -1;
    }

    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(3);
    tft.setCursor(0, 0);

    int count = 0;

    for (JsonObject dep : doc["departures"].as<JsonArray>()) {
      String line = dep["line"]["name"].as<String>();
      String direction = dep["direction"].as<String>();
      String product = dep["line"]["product"].as<String>();
      int min_to_arrive = getMinutesToArrival(dep["when"].as<String>());
      uint16_t background = getBackgroundColor(product);

      if (min_to_arrive >= MIN_TO_ARRIVE_THRESHOLD) {
        tft.setTextColor(TFT_WHITE, background);
        tft.drawString(line, 0, count * (tft.fontHeight() + 3));

        if (direction.length() > 19) direction = direction.substring(0, 19);
        while (direction.length() < 19) direction += " ";

        String delta_time = (min_to_arrive == 0) ? "now" : String(min_to_arrive) + "\"";
        while (delta_time.length() < 4) delta_time = " " + delta_time;

        String description = decodeUtf8(direction) + delta_time;
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(description, 65, count * (tft.fontHeight() + 3));

        count++;

        if ((count * (tft.fontHeight() + 3)) >= tft.height())
          break;
      }

      if (count >= DEP_LIMIT) 
        break;
    }

    if (count == 0) {
      tft.println("Tried to fetch train data. No service available...");
    }

  } else {
    Serial.print("HTTP error: ");
    Serial.println(httpCode);
    return -2;
  }

  http.end();

  return 0;
}


void showCountdown(int seconds) {
  for (int i = seconds; i >= 0; i--) {
    tft.fillRect(tft.width() / 4, tft.height() / 4, tft.width() * 3 / 4, tft.height() * 3 / 4, TFT_BLACK);
    String message = "Refresh in " + String(i) + " sec...";
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(message, tft.width() / 2, tft.height() / 2);
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
}


void setup() {
  Serial.begin(115200);

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
  tft.println("Starting...");
  delay(1000);

  // ====== Connect WiFi
  WiFi.begin(ssid, password);
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
  tft.println("Connecting WiFi...");
  delay(1000);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("WiFi connected");
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
  tft.println("WiFi connected");
  delay(1000);

  // ====== Configure time for Europe/Berlin
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  delay(1000);
}


void loop() {
  switch (fetchAndDisplayDepartures()) {
    case -1:
      displayError("JSON error");
      showCountdown(10);
      break;

    case -2:
      displayError("HTTP error");
      showCountdown(10);
      break;

    default:
      // ====== Update departures Cycle
      delay(60000);
      break;
  }
}
