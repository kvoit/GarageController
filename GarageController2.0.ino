#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <HC_SR04.h>
#include <PubSubClient.h>
#include "GarageControllerConfig.h"

const char *ssid = CONFIG_SSID;
const char *password = CONFIG_PASSWORD;

const char *mqtt_server = CONFIG_MQTT_SERVER;
const char *mqtt_user = CONFIG_MQTT_USER;
const char *mqtt_pw = CONFIG_MQTT_PW;
const char *mqtt_cmd_topic = "home-le/cellar/garage/maindoor/cmd";
const char *mqtt_quant_topic = "home-le/cellar/garage/maindoor/distance";
const char *mqtt_qual_topic = "home-le/cellar/garage/maindoor/status";
unsigned long mqttReconnectTime = millis();
unsigned long mqttReconnectInterval = 5000;
unsigned long mqttUpdateTime = millis();
unsigned long mqttUpdateInterval = 600000;

const int configPin = D3;
const int ledPin = D4;
const int relayPin = D1;
const int trigPin = D6;
const int echoPin = D7;

const int relayOnInterval = 400;
const int minClosedDist = 40;
const int maxOpenDist = 15;
const int distReadInterval = 1000;
const uint32_t blockCmdInterval = 5000;
uint32_t blockCmdTime = millis();
const char *openString = "open";
const char *isOpenString = "isopen";
const char *closedString = "closed";
const char *isClosedString = "isclosed";
const char *isBlockedString = "isblocked";
const char *undefString = "undefined";
const char *okString = "ok";

unsigned long relayOnTime = millis();
unsigned long distReadTime = millis();
unsigned int distance = (minClosedDist + maxOpenDist) / 2; //If possible, initialized as undefined
const char *statusString = undefString;
const char *statusStringPre = undefString;

ESP8266WebServer server ( 80 );
WiFiClient espClient;
PubSubClient mqttclient(espClient);

void handleStatus() {
  server.send ( 200, "text/plain", statusString );
}

void handleDistance() {
  server.send ( 200, "text/plain", String(distance) );
}

void handleOpen() {
  if ( millis() - blockCmdTime < blockCmdInterval ) {
    server.send ( 200, "text/plain", isBlockedString );
  }
  else if (distance >= minClosedDist) {
    blockCmdTime = millis();
    digitalWrite(relayPin, HIGH);
    Serial.println("Setting relayPin to HIGH");
    relayOnTime = millis();
    server.send ( 200, "text/plain", okString );
  }
  else if (distance <= maxOpenDist) {
    server.send ( 200, "text/plain", isOpenString );
  }
  else {
    server.send ( 200, "text/plain", undefString );
  }
}

void handleClose() {
  if ( millis() - blockCmdTime < blockCmdInterval) {
    server.send ( 200, "text/plain", isBlockedString );
  }
  else if (distance >= minClosedDist) {
    server.send ( 200, "text/plain", isClosedString );
  }
  else if (distance <= maxOpenDist) {
    blockCmdTime = millis();
    digitalWrite(relayPin, HIGH);
    Serial.println("Setting relayPin to HIGH");
    relayOnTime = millis();
    server.send ( 200, "text/plain", okString );
  }
  else {
    server.send ( 200, "text/plain", undefString );
  }
}

void handleRoot() {
  server.send ( 200, "text/plain", "root" );
}

void handleConfigpin() {
  server.send ( 200, "text/plain", String(digitalRead(configPin)) );
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += ( server.method() == HTTP_GET ) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for ( uint8_t i = 0; i < server.args(); i++ ) {
    message += " " + server.argName ( i ) + ": " + server.arg ( i ) + "\n";
  }

  server.send ( 404, "text/plain", message );
}

void readDist() {
  Serial.print("Reading dist: ");
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  distance = pulseIn(echoPin, HIGH, 12000) / 58;
  Serial.println(String(distance));

  if ( strcmp(statusString, undefString)) {
    statusStringPre = statusString;
  }

  if (distance >= minClosedDist) {
    statusString = closedString;
  }
  else if (distance <= maxOpenDist) {
    statusString = openString;
  }
  else {
    statusString = undefString;
  }

  if (strcmp(statusString, statusStringPre) && strcmp(statusString, undefString))
  {
    Serial.print("New status: ");
    Serial.print(statusString);
    Serial.print(" from ");
    Serial.println(statusStringPre);
    mqtt_pubstatus();
    mqtt_pubdist();
  }
}

void setup() {
  pinMode(ledPin, OUTPUT);
  pinMode(relayPin, OUTPUT);
  pinMode(configPin, INPUT);
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  digitalWrite ( configPin, HIGH );
  digitalWrite ( ledPin, LOW );
  Serial.begin ( 115200 );
  WiFi.begin ( ssid, password );
  Serial.println ( "" );

  // Wait for connection
  while ( WiFi.status() != WL_CONNECTED ) {
    delay ( 500 );
    Serial.print ( "." );
  }

  Serial.println ( "" );
  Serial.print ( "Connected to " );
  Serial.println ( ssid );
  Serial.print ( "IP address: " );
  Serial.println ( WiFi.localIP() );

  if ( MDNS.begin ( mqtt_user ) ) {
    Serial.println ( "MDNS responder started" );
  }

  mqttclient.setServer(mqtt_server, 1883);
  mqttclient.setCallback(mqtt_callback);

  server.on ( "/", handleRoot );
  server.on ( "/status", handleStatus );
  server.on ( "/distance", handleDistance );
  server.on ( "/open", handleOpen );
  server.on ( "/close", handleClose );
  server.on ( "/configpin", handleConfigpin );

  server.onNotFound ( handleNotFound );
  server.begin();
  Serial.println ( "HTTP server started" );
  digitalWrite ( ledPin, LOW );
}

void mqtt_reconnect() {
  // Loop until we're reconnected
  if (!mqttclient.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (mqttclient.connect("garagecontroller", mqtt_user, mqtt_pw, mqtt_qual_topic, 1, true, "offline")) {
      Serial.println("connected");

      // Once connected, publish an announcement...
      mqtt_pubstatus();
      mqtt_pubdist();
      // ... and resubscribe
      mqttclient.subscribe(mqtt_cmd_topic);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttclient.state());
      Serial.println(" try again in 5 seconds");
    }
  }
}

void mqtt_pubstatus() {
  if (mqttclient.connected()) {
    if (distance >= minClosedDist) {
      mqttclient.publish(mqtt_qual_topic, closedString, true);
    }
    else if (distance <= maxOpenDist) {
      mqttclient.publish(mqtt_qual_topic, openString, true);
    }
    else {
      mqttclient.publish(mqtt_qual_topic, undefString, true);
    }
  }
}

void mqtt_pubdist() {
  if (mqttclient.connected()) {
    char distchar[5];
    sprintf(distchar, "%1d", distance);
    mqttclient.publish(mqtt_quant_topic, distchar, true);
  }
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  char message_buff[length + 1];
  int i;
  for (i = 0; i < length; i++)
  {
    message_buff[i] = payload[i];
  }
  message_buff[i] = '\0';
  const char *p_payload = message_buff;

  if ( strcmp(topic, mqtt_cmd_topic) ) {
    return;
  }

  if ( !strcmp(p_payload, "open") ) {
    handleOpen();
  }
  else if ( !strcmp(p_payload, "close") ) {
    handleClose();
  }
  else if ( !strcmp(p_payload, "update") ) {
    mqtt_pubstatus();
    mqtt_pubdist();
  }
  else {
    Serial.println("Unknown command");
  }
}

void loop() {
  if (!mqttclient.connected()) {
    if (millis() - mqttReconnectTime > mqttReconnectInterval) {
      mqtt_reconnect();
      mqttReconnectTime = millis();
    }
  }
  else {
    if (millis() - mqttUpdateTime > mqttUpdateInterval) {
      mqtt_pubdist();
      mqttUpdateTime = millis();
    }
    mqttclient.loop();
  }

  server.handleClient();

  if (millis() - distReadTime > distReadInterval) {
    distReadTime = millis();
    readDist();
  }

  if (millis() - relayOnTime > relayOnInterval ) {
    digitalWrite(relayPin, LOW);
  }
}
