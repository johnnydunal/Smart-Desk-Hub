#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <time.h>

namespace Config {
constexpr char WIFI_SSID[] = "CHANGE_ME";
constexpr char WIFI_PASSWORD[] = "CHANGE_ME";

// The Raspberry Pi service should expose these small JSON endpoints:
// GET /api/weather -> {"temperature_c":21.4,"condition":"Clear"}
// GET /api/calendar -> {"events":[{"time":"10:00","title":"Stand-up"}]}
constexpr char WEATHER_URL[] = "http://192.168.1.100:8080/api/weather";
constexpr char CALENDAR_URL[] = "http://192.168.1.100:8080/api/calendar";
constexpr char ASSISTANT_URL[] = "http://192.168.1.100:8080/api/assistant/status";
constexpr char NTP_SERVER[] = "pool.ntp.org";
constexpr long UTC_OFFSET_SECONDS = 0;
constexpr int DAYLIGHT_OFFSET_SECONDS = 0;

constexpr uint32_t SENSOR_INTERVAL_MS = 30'000;
constexpr uint32_t WEATHER_INTERVAL_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t CALENDAR_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t DISPLAY_INTERVAL_MS = 60'000;
}  // namespace Config

struct RoomMetrics {
  float temperatureC = NAN;
  float humidityPercent = NAN;
  bool valid = false;
};

struct Weather {
  float temperatureC = NAN;
  String condition;
  bool valid = false;
};

struct CalendarEvent {
  String time;
  String title;
};

struct DeskState {
  RoomMetrics room;
  Weather weather;
  CalendarEvent nextEvent;
  bool hasNextEvent = false;
  bool wifiConnected = false;
  uint32_t lastUpdateMs = 0;
};

DeskState state;
uint32_t lastSensorMs = 0;
uint32_t lastWeatherMs = 0;
uint32_t lastCalendarMs = 0;
uint32_t lastDisplayMs = 0;

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    state.wifiConnected = true;
    return;
  }

  state.wifiConnected = false;
  Serial.printf("Connecting to Wi-Fi network '%s'...\n", Config::WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startMs < 15'000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  state.wifiConnected = WiFi.status() == WL_CONNECTED;
  if (state.wifiConnected) {
    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
    configTime(Config::UTC_OFFSET_SECONDS, Config::DAYLIGHT_OFFSET_SECONDS,
               Config::NTP_SERVER);
  } else {
    Serial.println("Wi-Fi unavailable; continuing with local-only features.");
  }
}

void readRoomMetrics() {
  // TODO: Replace this with the sensor selected for the final PCB.
  // Keeping the value invalid is safer than showing stale or fabricated data.
  state.room.valid = false;
  state.room.temperatureC = NAN;
  state.room.humidityPercent = NAN;
}

bool getJson(const char* url, JsonDocument& document) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient client;
  client.setTimeout(5'000);
  if (!client.begin(url)) {
    Serial.printf("Could not initialize HTTP request: %s\n", url);
    return false;
  }

  const int statusCode = client.GET();
  if (statusCode < 200 || statusCode >= 300) {
    Serial.printf("GET %s failed with HTTP %d\n", url, statusCode);
    client.end();
    return false;
  }

  const DeserializationError error = deserializeJson(document, client.getString());
  client.end();
  if (error) {
    Serial.printf("Invalid JSON from %s: %s\n", url, error.c_str());
    return false;
  }
  return true;
}

void updateWeather() {
  JsonDocument document;
  if (!getJson(Config::WEATHER_URL, document)) {
    return;
  }

  const JsonVariantConst temperature = document["temperature_c"];
  const JsonVariantConst condition = document["condition"];
  if (!temperature.is<float>() || !condition.is<const char*>()) {
    Serial.println("Weather response is missing required fields.");
    return;
  }

  state.weather.temperatureC = temperature.as<float>();
  state.weather.condition = condition.as<const char*>();
  state.weather.valid = true;
}

void updateCalendar() {
  JsonDocument document;
  if (!getJson(Config::CALENDAR_URL, document)) {
    return;
  }

  JsonArrayConst events = document["events"].as<JsonArrayConst>();
  if (events.isNull() || events.size() == 0) {
    state.hasNextEvent = false;
    return;
  }

  JsonObjectConst event = events[0];
  const JsonVariantConst time = event["time"];
  const JsonVariantConst title = event["title"];
  if (!time.is<const char*>() || !title.is<const char*>()) {
    Serial.println("Calendar response contains an invalid first event.");
    return;
  }

  state.nextEvent.time = time.as<const char*>();
  state.nextEvent.title = title.as<const char*>();
  state.hasNextEvent = true;
}

String currentTimeText() {
  struct tm localTime;
  if (!getLocalTime(&localTime, 100)) {
    return "--:--";
  }

  char buffer[6];
  strftime(buffer, sizeof(buffer), "%H:%M", &localTime);
  return String(buffer);
}

void renderDisplay() {
  // Serial output is the bring-up UI until the e-paper panel is selected.
  // The state and formatting below are intentionally independent of the
  // eventual GxEPD2 display driver.
  Serial.println("\n--- Smart Desk Hub ---");
  Serial.printf("Time: %s\n", currentTimeText().c_str());
  Serial.printf("Network: %s\n", state.wifiConnected ? "online" : "offline");

  if (state.room.valid) {
    Serial.printf("Room: %.1f C, %.0f%% humidity\n",
                  state.room.temperatureC, state.room.humidityPercent);
  } else {
    Serial.println("Room: sensor unavailable");
  }

  if (state.weather.valid) {
    Serial.printf("Weather: %.1f C, %s\n", state.weather.temperatureC,
                  state.weather.condition.c_str());
  } else {
    Serial.println("Weather: unavailable");
  }

  if (state.hasNextEvent) {
    Serial.printf("Next: %s - %s\n", state.nextEvent.time.c_str(),
                  state.nextEvent.title.c_str());
  } else {
    Serial.println("Next: no events");
  }
  Serial.println("----------------------");
}

void publishAssistantStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  JsonDocument payload;
  payload["device"] = "smart-desk-hub";
  payload["online"] = true;
  payload["room"]["temperature_c"] = state.room.valid
                                         ? state.room.temperatureC
                                         : nullptr;
  payload["room"]["humidity_percent"] =
      state.room.valid ? state.room.humidityPercent : nullptr;

  String body;
  serializeJson(payload, body);

  HTTPClient client;
  client.setTimeout(5'000);
  if (!client.begin(Config::ASSISTANT_URL)) {
    Serial.println("Could not initialize assistant status request.");
    return;
  }
  client.addHeader("Content-Type", "application/json");
  const int statusCode = client.POST(body);
  if (statusCode < 200 || statusCode >= 300) {
    Serial.printf("Assistant status update failed with HTTP %d\n", statusCode);
  }
  client.end();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nSmart Desk Hub firmware starting.");

  connectWifi();
  readRoomMetrics();
  updateWeather();
  updateCalendar();
  renderDisplay();
}

void loop() {
  const uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED &&
      now - state.lastUpdateMs >= 15'000) {
    connectWifi();
    state.lastUpdateMs = now;
  }

  if (now - lastSensorMs >= Config::SENSOR_INTERVAL_MS) {
    lastSensorMs = now;
    readRoomMetrics();
    publishAssistantStatus();
  }

  if (now - lastWeatherMs >= Config::WEATHER_INTERVAL_MS) {
    lastWeatherMs = now;
    updateWeather();
  }

  if (now - lastCalendarMs >= Config::CALENDAR_INTERVAL_MS) {
    lastCalendarMs = now;
    updateCalendar();
  }

  if (now - lastDisplayMs >= Config::DISPLAY_INTERVAL_MS) {
    lastDisplayMs = now;
    renderDisplay();
  }

  delay(50);
}