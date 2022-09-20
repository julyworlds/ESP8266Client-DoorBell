#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <PubSubClient.h>
#include "settings.h"

// Settings for opening door time and call signal detection cooldown
unsigned long openRelayTime = 2500;
unsigned long callCoolDown = 3500;
//--------

bool reconnect_is_available = false;
bool ota_failsafe_update = true;
unsigned long ota_update_enabled = 0;
bool update_in_progress = false;
String mqtt_clientId = "ESP8266Client-DoorBell1";

bool http_logging_enabled = false;
String log_server = "http://192.168.0.1:8888/";
bool mqtt_logging_enabled = true;
// Topic to send logs
String mqtt_logging = "ESP8266Client-DoorBell1/log";
// Topic to receive command for OTA mode
String mqtt_ota = "ESP8266Client-DoorBell1/ota";

int CALL_PIN = 1;
int RELAY_PIN = 3;
// TX -> GPIO 1 => call signal
// RX -> GPIO 3 => relay / open door

// Wifi and mqtt in settings.h
// MQTT devices for homeassistant using mqtt
String mqtt_doorbell_bell = "homeassistant/binary_sensor/name/state";
String mqtt_doorbell_topic_config = "homeassistant/lock/name/set";
String mqtt_doorbell_topic_status = "homeassistant/lock/name/state";

WiFiClient espClient;
WiFiClient espClientLog;
PubSubClient client(espClient);
PubSubClient clientLog(espClientLog);
WiFiClient espClientHttpLog;

// Relay / Door vars
unsigned long lastOpenRelayTime = 0;
bool receivedOpenRelay = false;
bool openingRelay = false;

// Call signal vars
bool openCall = false;
unsigned long lastOpenCallTime = 0;

void logging(String message, bool disableReconnect = false) {
  if (mqtt_logging_enabled) {
    send_message_log(mqtt_logging, message, disableReconnect);
  }
  if (http_logging_enabled) {
    HTTPClient http;
    http.begin(espClientHttpLog, log_server.c_str());
    http.addHeader("Content-type", "text/plain");
    http.POST(message.c_str());
    http.end();
  }
}

void reconnect(bool isLogger = false, bool forceReconnect = false) {
  if (!reconnect_is_available && !forceReconnect) return;
  int tries = 0;
  if (!isLogger) {
    while (!client.connected() && tries < 10) {
      logging("Attempting MQTT connection...", true);
      // Attempt to connect
      if (client.connect(mqtt_clientId.c_str(), mqtt_user.c_str(), mqtt_password.c_str())) {
        logging("MQTT connected", true);
        client.subscribe(mqtt_doorbell_topic_config.c_str());
        client.subscribe(mqtt_ota.c_str());
      } else {
        logging("MQTT disconnected, reconnecting...", true);
        tries += 1;
        delay(200);
      }
    }
  } else {
    if (!mqtt_logging_enabled) return;
    int tries = 0;
    while (!clientLog.connected() && tries < 10) {
      logging("Attempting MQTT logger connection...", true);
      // Attempt to connect
      if (clientLog.connect((mqtt_clientId + String("-logger")).c_str(), mqtt_user.c_str(), mqtt_password.c_str())) {
        logging("MQTT logger connected", true);
      } else {
        logging("MQTT logger disconnected, reconnecting...", true);
        tries += 1;
        delay(200);
      }
    }  
  }
}

void send_message(String topic, String message) {
  if (!client.connected()) {
    reconnect();
  }
  if (client.connected()) {
    client.publish(topic.c_str(), message.c_str());
  }
}

void send_message_log(String topic, String message, bool disableReconnect) {
  if (!clientLog.connected() && !disableReconnect) {
    reconnect(true);
  }
  if (clientLog.connected()) {
    boolean retained = true;
    clientLog.publish(topic.c_str(), message.c_str(), retained);
  }
}

void message_received(const char* topic, byte* payload, unsigned int length) {
  logging(String("Message received topic: ") + String(topic));
  char payloadString[length+1];
  memcpy(payloadString, payload, length);
  payloadString[length] = '\0';
  String message = String(payloadString);
  logging(String("Message received payload: ") + message);
  if (String(topic) == mqtt_doorbell_topic_config) {
    if (message == "UNLOCK") {
      receivedOpenRelay = true;
    } else if (message == "LOCK") {
      logging("Already locked!");
    }
  } else if (String(topic) == mqtt_ota) {
    if (message == "ON") {
      ArduinoOTA.begin();
      ota_update_enabled = millis();
      ota_failsafe_update = true;
      logging("OTA failsafe update ENABLED for 30 seconds...");
    }
  }
  // Default
  // Do nothing
}

void setup() {
  //GPIO 1 (TX) swap the pin to a GPIO.
  pinMode(1, FUNCTION_3); 
  //GPIO 3 (RX) swap the pin to a GPIO.
  pinMode(3, FUNCTION_3);
  // PINS RELAY, CALL SIGNAL
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(CALL_PIN, INPUT_PULLUP); //test pullup resistor
  ////
  // WIFI
  WiFi.hostname(mqtt_clientId.c_str());
  WiFi.begin(wifi_essid.c_str(), wifi_password.c_str());
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  ////
  logging("Board init: wifi connected");
  // Arduino OTA
  ArduinoOTA.onStart([]() {
    update_in_progress = true;
    logging("Start OTA update");
  });
  ArduinoOTA.onEnd([]() {
    update_in_progress = false;
    logging("OTA update finished");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    char progressMessage[30];
    sprintf(progressMessage, "Progress: %u%%\r", (progress / (total / 100)));
    logging(String(progressMessage));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    update_in_progress = false;
    char errorMessage[20];
    sprintf(errorMessage, "Error[%u]: ", error);
    logging(String(errorMessage));
    if (error == OTA_AUTH_ERROR) logging("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) logging("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) logging("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) logging("Receive Failed");
    else if (error == OTA_END_ERROR) logging("End Failed");
  });
  // ArduinoOTA.setPassword("123");
  ArduinoOTA.begin();
  ota_update_enabled = millis();
  logging(String("Board init: Arduino OTA"));
  // MQTT logger
  if (mqtt_logging_enabled) {
    clientLog.setServer(mqtt_server.c_str(), mqtt_port);
    reconnect(true, true);
  }
  // MQTT server
  client.setServer(mqtt_server.c_str(), mqtt_port);
  client.setCallback(message_received);
  reconnect(false, true);
  reconnect_is_available = true;
  //
  logging(String("Board init: Arduino OTA IP address: ") + WiFi.localIP().toString());
  logging("Board init: setup completed");
  logging("OTA failsafe update for 30 seconds...");
}

void loop() {
  // Handle OTA update
  if (update_in_progress || ota_failsafe_update) {
    ArduinoOTA.handle();
    // Disable failsafe update after 30 seconds
    if (!update_in_progress && ota_failsafe_update && millis() > ota_update_enabled + 30000) {
      ArduinoOTA.end();
      logging("OTA failsafe update disabled, start running ...");
      ota_failsafe_update = false;
    }
    return;
  }
  // loop start
  // check wifi is connected
  if (WiFi.status() == WL_CONNECTED) {
    // Reconnect if necessary MQTT
    reconnect();
    // check open relay command
    if (receivedOpenRelay && !openingRelay) {
      // code block 1
      logging("openingRelay ON");
      digitalWrite(RELAY_PIN, HIGH);
      lastOpenRelayTime = millis();
      openingRelay = true;
      receivedOpenRelay = false;
      // send command door open
      send_message(mqtt_doorbell_topic_status, "UNLOCKED");
    } else if (openingRelay && millis() > (lastOpenRelayTime + openRelayTime)) {
      logging("openingRelay OFF");
      // code block 2
      digitalWrite(RELAY_PIN, LOW);
      openingRelay = false;
      // send command door closed
      send_message(mqtt_doorbell_topic_status, "LOCKED");
    } else {
      // check call signal
      bool callDetected = false;
      int callPinState = digitalRead(CALL_PIN);
      if (callPinState == LOW) {
        callDetected = true;
      }
      ////
      if (callDetected && !openCall) {
        logging("callSignal ON");
        // code block 3
        lastOpenCallTime = millis();
        openCall = true;
        // send bell ringing
        send_message(mqtt_doorbell_bell, "ON");
      } else if (!callDetected && openCall && millis() > (lastOpenCallTime + callCoolDown)) {
        logging("callSignal OFF");
        // code block 4
        openCall = false;
        // send bell ringing
        send_message(mqtt_doorbell_bell, "OFF");
      }
    }
    client.loop();
    delay(100);
  } else {
    // wait for wifi connection
    delay(500);
  }
  // loop end
}
