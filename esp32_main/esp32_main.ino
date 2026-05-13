/*
  =====================================================
  Smart Skleník – ESP32
  =====================================================
  Komponenty:
    - Senzor vlhkosti pôdy            → GPIO35
    - DHT11 (teplota/vlhkosť vzduchu) → GPIO13
    - Senzor hladiny vody             → GPIO32
    - Relé (čerpadlo)                 → GPIO25
    - Motor/ventilátor (PWM)          → GPIO26
    - RGB pásik WS2812B               → GPIO4

  Komunikácia: MQTT over TLS → HiveMQ Cloud
  =====================================================
  KNIŽNICE:
    - PubSubClient     (Nick O'Leary)
    - FastLED          (Daniel Garcia)
    - ArduinoJson      (Benoit Blanchon)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <DHT.h>
#include <FastLED.h>
#include <ArduinoJson.h>

// KONFIGURÁCIA
const char* WIFI_SSID   = "Nothing";
const char* WIFI_PASS   = "pepesko123";

const char* MQTT_SERVER = "1d9c5feaaa414759afcb34e04d9dedb6.s1.eu.hivemq.cloud";
const int   MQTT_PORT   = 8883;
const char* MQTT_USER   = "esp32";
const char* MQTT_PASS   = "ESP32heslo123";
const char* MQTT_CLIENT = "SmartGreenhouse_ESP32_001";

#define TOPIC_SENSORS    "smartgreenhouse/sensors"
#define TOPIC_STATUS     "smartgreenhouse/status"
#define TOPIC_CMD_LED    "smartgreenhouse/cmd/led"
#define TOPIC_CMD_PUMP   "smartgreenhouse/cmd/pump"
#define TOPIC_CMD_FAN    "smartgreenhouse/cmd/fan"
#define TOPIC_CMD_AUTO   "smartgreenhouse/cmd/auto"

// PINY
#define PIN_SOIL_MOISTURE  35
#define PIN_WATER_LEVEL    32
#define PIN_DHT            13
#define PIN_RELAY_PUMP     25
#define PIN_MOTOR_FAN      26
#define PIN_LED_DATA        4

#define FAN_PWM_FREQ     5000
#define FAN_PWM_BITS     8

// NASTAVENIA
#define LED_COUNT          8
#define LED_MAX_BRIGHTNESS 76

#define PUMP_DUTY_PERIOD_MS  1000UL

#define SOIL_AIR           4095
#define SOIL_DRY_HIGH      4000
#define SOIL_DRY_LOW       2400
#define SOIL_MOIST_LOW     1480

#define SOIL_AUTO_ON       3200
#define SOIL_AUTO_OFF      2000

#define TEMP_HOT_THRESHOLD   28.0
#define WATER_LOW_THRESHOLD  500

#define SENSOR_INTERVAL  5000
#define PUMP_MAX_RUN     10000
#define FAN_MAX_RUN      30000

// GLOBÁLNE OBJEKTY
DHT dht(PIN_DHT, DHT11);
CRGB leds[LED_COUNT];
WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);

// STAV SYSTÉMU
// 
struct SystemState {
  int   soilMoisture = 0;
  int   waterLevel   = 0;
  float temperature  = 0;
  float humidity     = 0;
  bool  pumpOn       = false;
  bool  fanOn        = false;
  bool  ledOn        = true;
  bool  autoMode     = false;  // štartuje VYPNUTÝ – užívateľ zapne manuálne
  uint8_t ledR = 0, ledG = 150, ledB = 80;
  uint8_t fanSpeed   = 255;
  uint8_t pumpSpeed  = 100;   // 10–100%, default plný výkon
  unsigned long pumpStartTime  = 0;
  unsigned long fanStartTime   = 0;
  unsigned long lastSensorRead = 0;
  // Duty cycle stav (non-blocking software PWM pre pumpu)
  bool  pumpPhysical    = false;  // skutočný stav relé
  unsigned long pumpDutyTick = 0; // čas začiatku aktuálnej fázy
  // Soft-start ventilatora
  bool  fanSoftStarting  = false; // prebieha soft-start?
  unsigned long fanSoftStartBegin = 0; // kedy sa zacal
  uint8_t fanTargetSpeed = 255;   // cielova PWM po soft-starte
} state;

// Dlzka full-power fazy pri starte [ms]
#define FAN_SOFTSTART_MS  800

unsigned long lastReconnectAttempt = 0;

// FORWARD DECLARATIONS
void connectWiFi();
void connectMQTT();
void readSensors();
void publishSensors();
void publishStatus(const char* status);
void autoControl();
void setPump(bool on, uint8_t speed = 100);  // speed = 10–100%
void setFan(bool on, uint8_t speed = 255);  // default len tu
void setLedColor(uint8_t r, uint8_t g, uint8_t b);
void updateStatusLed();
void mqttCallback(char* topic, byte* payload, unsigned int length);
int soilToPercent(int raw);
String soilDescription(int raw);

// SETUP
void setup() {
  digitalWrite(PIN_RELAY_PUMP, HIGH); 
  pinMode(PIN_RELAY_PUMP, OUTPUT);

  Serial.begin(115200);
  Serial.println("\n=== Smart Skleník ===");

  // Ventilátor – PWM
  pinMode(PIN_MOTOR_FAN, OUTPUT);
  digitalWrite(PIN_MOTOR_FAN, LOW);   // fyzicky LOW pred tým než PWM prevezme kontrolu
  ledcAttach(PIN_MOTOR_FAN, FAN_PWM_FREQ, FAN_PWM_BITS);
  ledcWrite(PIN_MOTOR_FAN, 0);        // duty = 0 → ventilátor vypnutý

  //dht.setup(PIN_DHT, DHTesp::DHT11);
  dht.begin();
  delay(2000);

  FastLED.addLeds<WS2812B, PIN_LED_DATA, GRB>(leds, LED_COUNT);
  FastLED.setBrightness(LED_MAX_BRIGHTNESS);
  setLedColor(0, 0, 50);
  FastLED.show();

  connectWiFi();

  wifiClient.setInsecure();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  connectMQTT();

  setLedColor(state.ledR, state.ledG, state.ledB);
  FastLED.show();
  Serial.println("Systém pripravený!");
}

// HLAVNÁ SLUČKA
void loop() {
  yield();

  if (!mqtt.connected()) {
    unsigned long now2 = millis();
    if (now2 - lastReconnectAttempt >= 10000) {
      lastReconnectAttempt = now2;
      Serial.println("MQTT odpojený, pokúšam sa znova...");
      if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)) {
        Serial.println("MQTT reconnect OK!");
        mqtt.subscribe(TOPIC_CMD_LED);
        mqtt.subscribe(TOPIC_CMD_PUMP);
        mqtt.subscribe(TOPIC_CMD_FAN);
        mqtt.subscribe(TOPIC_CMD_AUTO);
        publishStatus("online");
      }
    }
    return;
  }
  mqtt.loop();

  unsigned long now = millis();
  if (now - state.lastSensorRead >= SENSOR_INTERVAL) {
    state.lastSensorRead = now;
    readSensors();
    publishSensors();
    if (state.autoMode) autoControl();
    if (state.autoMode && state.ledOn) updateStatusLed();
  }

  if (state.pumpOn && (now - state.pumpStartTime >= PUMP_MAX_RUN)) {
    setPump(false);
    publishStatus("pump_timeout");
  }

  if (state.pumpOn) {
    unsigned long onTime  = (PUMP_DUTY_PERIOD_MS * state.pumpSpeed) / 100UL;
    unsigned long offTime = PUMP_DUTY_PERIOD_MS - onTime;
    unsigned long elapsed = now - state.pumpDutyTick;

    if (state.pumpPhysical && offTime > 0 && elapsed >= onTime) {
      // Koniec ON fázy → relé vyp
      digitalWrite(PIN_RELAY_PUMP, HIGH);
      state.pumpPhysical = false;
      state.pumpDutyTick = now;
    } else if (!state.pumpPhysical && elapsed >= offTime) {
      // Koniec OFF fázy → relé zap
      digitalWrite(PIN_RELAY_PUMP, LOW);
      state.pumpPhysical = true;
      state.pumpDutyTick = now;
    }
  }

  if (state.fanOn && (now - state.fanStartTime >= FAN_MAX_RUN)) {
    setFan(false);
    publishStatus("fan_timeout");
  }

  // Soft-start: po FAN_SOFTSTART_MS prepni na cielovu rychlost
  if (state.fanSoftStarting && state.fanOn &&
      (now - state.fanSoftStartBegin >= FAN_SOFTSTART_MS)) {
    state.fanSoftStarting = false;
    state.fanSpeed = state.fanTargetSpeed;
    ledcWrite(PIN_MOTOR_FAN, state.fanTargetSpeed);
    Serial.printf("Ventilator: soft-start hotovy, rychlost=%d (%d%%)\n",
                  state.fanTargetSpeed, map(state.fanTargetSpeed, 0, 255, 0, 100));
  }
}

// WIFI
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(1000);
  Serial.print("Pripájam na WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi CHYBA – reštartujem...");
    ESP.restart();
  }
}

// MQTT
void connectMQTT() {
  int attempts = 0;
  while (!mqtt.connected() && attempts < 5) {
    Serial.print("MQTT pripájam (TLS)...");
    if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)) {
      Serial.println(" OK!");
      mqtt.subscribe(TOPIC_CMD_LED);
      mqtt.subscribe(TOPIC_CMD_PUMP);
      mqtt.subscribe(TOPIC_CMD_FAN);
      mqtt.subscribe(TOPIC_CMD_AUTO);
      // Vymaž retained správy na cmd topicoch (ochrana pred bootom)
      mqtt.publish(TOPIC_CMD_PUMP, "", true);
      mqtt.publish(TOPIC_CMD_FAN,  "", true);
      publishStatus("online");
    } else {
      Serial.printf(" CHYBA: %d\n", mqtt.state());
      delay(3000);
      attempts++;
    }
  }
}

// MQTT CALLBACK
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[length + 1];
  memcpy(msg, payload, length);
  msg[length] = '\0';
  Serial.printf("MQTT [%s]: %s\n", topic, msg);

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, msg)) return;
  String t = String(topic);

  if (t == TOPIC_CMD_LED) {
    state.ledOn = doc["on"] | state.ledOn;
    if (doc.containsKey("r")) state.ledR = doc["r"];
    if (doc.containsKey("g")) state.ledG = doc["g"];
    if (doc.containsKey("b")) state.ledB = doc["b"];
    if (doc.containsKey("brightness")) {
      int br = constrain((int)doc["brightness"], 0, 30);
      FastLED.setBrightness(map(br, 0, 30, 0, 76));
    }
    setLedColor(state.ledOn ? state.ledR : 0,
                state.ledOn ? state.ledG : 0,
                state.ledOn ? state.ledB : 0);
    FastLED.show();
    publishStatus("led_updated");
  }
  else if (t == TOPIC_CMD_PUMP) {
    bool on = doc["on"] | false;
    if (on && state.fanOn) { publishStatus("pump_denied_fan_running"); return; }
    uint8_t spd = state.pumpSpeed;  // zachovaj aktuálnu rýchlosť ak nie je v správe
    if (doc.containsKey("speed")) {
      spd = (uint8_t)constrain((int)doc["speed"], 10, 100);
    }
    setPump(on, spd);
    publishStatus(on ? "pump_on" : "pump_off");
  }
  else if (t == TOPIC_CMD_FAN) {
    bool on = doc["on"] | false;
    uint8_t spd = 255;
    if (doc.containsKey("speed")) {
      int pct = constrain((int)doc["speed"], 0, 100);
      spd = map(pct, 0, 100, 0, 255);
    }
    if (on && state.pumpOn) { publishStatus("fan_denied_pump_running"); return; }
    setFan(on, spd);
    publishStatus(on ? "fan_on" : "fan_off");
  }
  else if (t == TOPIC_CMD_AUTO) {
    state.autoMode = doc["on"] | false;
    publishStatus(state.autoMode ? "auto_on" : "auto_off");
  }
}

// SENZORY
int soilToPercent(int raw) {
  raw = constrain(raw, 1480, 4095);
  return map(raw, 4095, 1480, 0, 100);
}

String soilDescription(int raw) {
  if (raw >= SOIL_DRY_HIGH)  return "Sucho/vzduch";
  if (raw >= SOIL_DRY_LOW)   return "Sucha hlina";
  if (raw >= SOIL_MOIST_LOW) return "Vlhka hlina";
  return "Mokra";
}

// Median filter – vyhodi spike hodnoty spossobene motorom/ventilatorom.
// Zoberie N samplov, zoradi ich a vrati prostredny.
int analogReadMedian(uint8_t pin, int samples = 7) {
  int buf[samples];
  for (int i = 0; i < samples; i++) {
    buf[i] = analogRead(pin);
    delayMicroseconds(200);  // krátka pauza medzi samplami
  }
  // insertion sort (male N, rychle)
  for (int i = 1; i < samples; i++) {
    int key = buf[i], j = i - 1;
    while (j >= 0 && buf[j] > key) { buf[j+1] = buf[j]; j--; }
    buf[j+1] = key;
  }
  return buf[samples / 2];
}

void readSensors() {
  state.soilMoisture = analogReadMedian(PIN_SOIL_MOISTURE);
  state.waterLevel   = analogReadMedian(PIN_WATER_LEVEL);

  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  state.temperature = temperature;
  state.humidity    = humidity;
  delay(1000);

  Serial.printf("Poda:%d (%d%%) Voda:%d Teplota:%.1fC Vlhkost:%.1f%%\n",
    state.soilMoisture, soilToPercent(state.soilMoisture),
    state.waterLevel, state.temperature, state.humidity);
}

// MQTT PUBLIKOVANIE
void publishSensors() {
  StaticJsonDocument<350> doc;
  doc["soil"]        = state.soilMoisture;
  doc["soilPct"]     = soilToPercent(state.soilMoisture);
  doc["soilDesc"]    = soilDescription(state.soilMoisture);
  doc["water"]       = state.waterLevel;
  doc["waterPct"]    = map(constrain(state.waterLevel, 0, 4095), 0, 4095, 0, 100);
  doc["temp"]        = round(state.temperature * 10) / 10.0;
  doc["humidity"]    = round(state.humidity * 10) / 10.0;
  doc["pump"]        = state.pumpOn;
  doc["pumpSpeed"]   = state.pumpSpeed;
  doc["fan"]         = state.fanOn;
  doc["fanSpeed"]    = map(state.fanSpeed, 0, 255, 0, 100);
  doc["led"]         = state.ledOn;
  doc["auto"]        = state.autoMode;
  doc["waterLow"]    = (state.waterLevel < WATER_LOW_THRESHOLD);

  char buf[350];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_SENSORS, buf, false);
}

void publishStatus(const char* status) {
  StaticJsonDocument<100> doc;
  doc["status"] = status;
  doc["millis"] = millis();
  char buf[100];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_STATUS, buf);
}

// AUTO KONTROLA
void autoControl() {
  if (state.waterLevel < WATER_LOW_THRESHOLD) {
    if (state.pumpOn) {
      setPump(false);
      publishStatus("pump_stopped_water_low");
    }
    publishStatus("water_low_warning");
    return;
  }
  if (state.soilMoisture > SOIL_AUTO_ON && !state.pumpOn && !state.fanOn) {
    setPump(true);
    publishStatus("auto_pump_on");
  }
  if (state.soilMoisture < SOIL_AUTO_OFF && state.pumpOn) {
    setPump(false);
    publishStatus("auto_pump_off");
  }
  if (state.temperature > TEMP_HOT_THRESHOLD && !state.fanOn && !state.pumpOn) {
    setFan(true, 200);
    publishStatus("auto_fan_on");
  }
  if (state.temperature < (TEMP_HOT_THRESHOLD - 2.0) && state.fanOn) {
    setFan(false);
    publishStatus("auto_fan_off");
  }
}

// AKTUÁTORY
void setPump(bool on, uint8_t speed) {
  speed = constrain(speed, 10, 100);
  if (on && state.fanOn) {
    state.fanOn = false;
    ledcWrite(PIN_MOTOR_FAN, 0);
  }
  state.pumpOn    = on;
  state.pumpSpeed = speed;
  if (on) {
    state.pumpStartTime = millis();
    state.pumpDutyTick  = millis();
    // Hneď zapni relé (začína ON fázou)
    digitalWrite(PIN_RELAY_PUMP, LOW);
    state.pumpPhysical = true;
    Serial.printf("Cerpadlo: ZAP (%d%% vykon)\n", speed);
  } else {
    // Vypni relé okamžite
    digitalWrite(PIN_RELAY_PUMP, HIGH);
    state.pumpPhysical = false;
    Serial.println("Cerpadlo: VYP");
  }
}

void setFan(bool on, uint8_t speed) {
  if (on && state.pumpOn) {
    state.pumpOn = false;
    digitalWrite(PIN_RELAY_PUMP, HIGH);
  }
  state.fanOn = on;
  if (on) {
    state.fanTargetSpeed   = speed;   // uloz ciel
    state.fanSpeed         = 255;     // fyzicky zacni na plny vykon
    state.fanSoftStarting  = true;
    state.fanSoftStartBegin = millis();
    state.fanStartTime      = millis();
    ledcWrite(PIN_MOTOR_FAN, 255);    // FULL po dobu FAN_SOFTSTART_MS
    Serial.printf("Ventilator: ZAP soft-start (ciel PWM:%d = %d%%)\n",
                  speed, map(speed, 0, 255, 0, 100));
  } else {
    ledcWrite(PIN_MOTOR_FAN, 0);
    state.fanSoftStarting = false;
    Serial.println("Ventilator: VYP");
  }
}

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < LED_COUNT; i++) leds[i] = CRGB(r, g, b);
}

void updateStatusLed() {
  if (state.waterLevel < WATER_LOW_THRESHOLD)      setLedColor(200, 0,   0);
  else if (state.pumpOn)                           setLedColor(0,  50, 200);
  else if (state.fanOn)                            setLedColor(0, 180, 180);
  else if (state.soilMoisture > SOIL_AUTO_ON)      setLedColor(200, 80,  0);
  else                                             setLedColor(0, 150,  50);
  FastLED.show();
}
