#include <NeoPixelBus.h>
#ifdef ESP8266
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include "network.hpp"
#include "tween.hpp"


// const uint16_t PixelCount = 162; // bathroom
const uint16_t PixelCount = 60; // kitchen
const uint8_t PixelPin = 2; // make sure to set this to the correct pin, ignored for Esp8266

NeoPixelBus<NeoGrbFeature, NeoWs2812xMethod> strip(PixelCount, PixelPin);

const uint8_t brightness = 64;
const unsigned long followUpTimeMs = 7 * 60 * 1000;
const uint32_t animationDurationMs = 2000;
const uint32_t animationStepTimeMs = 33;

unsigned long turnOffLightsAtMs = 0;
const char *deviceId = "BBKXQ"; //KXQ is the kitchen, BBKXP is the bathroom

constexpr unsigned long nightTimeStartMs = 16 * 60 * 60 * 1000;
constexpr unsigned long nightTimeEndMs = 8 * 60 * 60 * 1000;

RgbColor black(0);
RgbColor amber = RgbColor(0xFF, 0xBF, 0x00);
RgbColor orange = RgbColor(0xFF, 0x99, 0x00);
Tween<int32_t> brightness_tween = Tween<int32_t>(0, 0, 0, 0, animationStepTimeMs);

WiFiClientSecure esp_client;
PubSubClient mqtt_client(esp_client);

void setup_wifi()
{
  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi!");
}

void set_all_pixels(RgbColor &color)
{
  for (uint32_t i = 0; i < PixelCount; i++)
  {
    strip.SetPixelColor(i, color);
  }
  strip.Show();
}

void start_on_animation()
{
  const auto now = millis();
  const auto current_brightness = brightness_tween.get_value(now);
  brightness_tween = Tween<int32_t>(current_brightness, brightness, animationDurationMs, millis(), animationStepTimeMs);
}

void start_off_animation()
{
  const auto now = millis();
  const auto current_brightness = brightness_tween.get_value(now);
  brightness_tween = Tween<int32_t>(current_brightness, 0, animationDurationMs, millis(), animationStepTimeMs);
}

void handle_animation(unsigned long now)
{
  if (brightness_tween.is_done(now))
  {
    return;
  }
  const auto maybeBrightness = brightness_tween.perform_step(now);
  if (maybeBrightness)
  {
    const auto brightness = *maybeBrightness;
    Serial.printf("Setting brightness to %d\n", brightness);
    auto color = orange.Dim(brightness);
    Serial.printf("Color: %02x %02x %02x\n", color.R, color.G, color.B);
    set_all_pixels(color);
  }
}

bool is_night_time(uint64_t timestamp)
{
  const unsigned long currentTimeMs = (timestamp % (86400*1000));
  const auto is_night_time = currentTimeMs > nightTimeStartMs || currentTimeMs < nightTimeEndMs;
  Serial.printf("Is night time: %s (%lu)\n", is_night_time ? "yes" : "no", currentTimeMs);
  return is_night_time;
}

void bump_turn_off_time()
{
  turnOffLightsAtMs = millis() + followUpTimeMs;
  Serial.printf("Next off time at %.1f min after boot\n", turnOffLightsAtMs / (1000.0f * 60.0f));
}

void on_motion_changed(uint32_t motion_value, uint64_t timestamp)
{
  if (motion_value)
  {
    if (is_night_time(timestamp))
    {
      Serial.println("Turning on lights");
      start_on_animation();
    }
    turnOffLightsAtMs = 0;
  }
  else
  {
    bump_turn_off_time();
  }
}

void setup_leds()
{
  strip.Begin();
  strip.Show();
}

void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error)
  {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }
  Serial.println("Payload:");
  serializeJsonPretty(doc, Serial);
  Serial.println();

  const auto topicString = String(topic);
  if (topicString.endsWith("sensorData")) {
    Serial.println("DBG: sensorData");
    if (
        doc.containsKey("type") && doc["type"].as<String>() == "MOTION" && doc.containsKey("value") && doc.containsKey("timestamp") && doc.containsKey("deviceId") && doc["deviceId"].as<String>() == deviceId)
    {
      on_motion_changed(doc["value"], doc["timestamp"]);
    }
  } else if (topicString.endsWith("actuatorData")) {
    char orgaId[40] = {};
    char siteId[40] = {};
    char deviceId[40] = {};
    char actuatorType[40] = {};
    int32_t index = 0;
    sscanf(topic, "rltn-iot/%[^/]/%[^/]/%[^/]/actuator/%[^/]/%d/actuatorData", &orgaId, &siteId, &deviceId, &actuatorType, &index);
    if (String(deviceId) == relution_device_uuid && String(actuatorType) == "TURN_ON_OFF") {
      Serial.println("DBG: actuator matches criteria");
      if (doc.containsKey("value"))
      {
        uint32_t value = doc["value"];
        if (value > 0) {
          start_on_animation();
        } else {
          start_off_animation();
        }
      }
    }
  }

}

void setup_mqtt()
{
  mqtt_client.setBufferSize(1024);
  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqtt_callback);

  // Ensure a secure connection
  esp_client.setInsecure(); // This line ensures a secure connection without certificate validation
}

void reconnect_mqtt()
{
  while (!mqtt_client.connected())
  {
    Serial.print("Attempting MQTT connection...");

    if (mqtt_client.connect(mqtt_client_id, mqtt_user, mqtt_password))
    {
      Serial.println("connected");
      const bool res = mqtt_client.subscribe(mqtt_topic);
      Serial.printf("Subscribed to %s: %s\n", mqtt_topic, res ? "OK" : "Failed");
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(mqtt_client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void maybe_turn_off_lights()
{
  if (turnOffLightsAtMs == 0)
  {
    return;
  }

  const unsigned long now = millis();
  if (now > turnOffLightsAtMs)
  {
    Serial.println("Turning off lights");
    start_off_animation();
    turnOffLightsAtMs = 0;
  }
}

void setup()
{
  Serial.begin(115200);
  setup_leds();
  setup_wifi();
  setup_mqtt();
}

void loop()
{

  if (!mqtt_client.connected())
  {
    reconnect_mqtt();
  }
  mqtt_client.loop();
  maybe_turn_off_lights();
  handle_animation(millis());
}
