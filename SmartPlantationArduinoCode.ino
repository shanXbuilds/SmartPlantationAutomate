#include <DHT.h>

// ---------- Pin Definitions ----------
#define SOIL1_PIN    A0     // Field 1 Soil Moisture Sensor
#define SOIL2_PIN    A1     // Field 2 Soil Moisture Sensor
#define DHT_PIN      4      // DHT11 Ambient Temperature & Humidity Sensor
#define DHTTYPE      DHT11

const uint8_t NUM_PUMPS = 3;
// Index 0 = Pump 1 (D7), Index 1 = Pump 2 (D6), Index 2 = Pump 3 (D8 - Manual Only)
const uint8_t RELAY_PINS[NUM_PUMPS] = {7, 6, 8};

DHT dht(DHT_PIN, DHTTYPE);

// ---------- Calibration Parameters ----------
const int DRY_VALUE          = 800; // Raw ADC reading in dry soil (0%)
const int WET_VALUE          = 300; // Raw ADC reading in saturated soil (100%)
const int MOISTURE_THRESHOLD = 40;  // Auto-irrigation threshold percentage (40%)

// ---------- Mode Control ----------
// true = Auto (Pumps 1 & 2) + Manual | false = Manual-Only for all pumps
bool autoModeEnabled = true;

// ---------- Active-LOW Relay Logic ----------
const bool RELAY_ACTIVE_LOW  = true;
const uint8_t RELAY_ON       = RELAY_ACTIVE_LOW ? LOW  : HIGH;
const uint8_t RELAY_OFF      = RELAY_ACTIVE_LOW ? HIGH : LOW;

// ---------- Non-Blocking Timing (milliseconds) ----------
const unsigned long CHECK_INTERVAL = 5000; // 5s telemetry & sensor check cycle
const unsigned long PUMP_RUN_TIME  = 3000; // 3s active pump pulse duration
const unsigned long PUMP_COOLDOWN  = 3000; // 3s safety cooldown between pulses

// ---------- Pump State Variables ----------
bool          pumpRunning[NUM_PUMPS]   = {false, false, false};
unsigned long pumpStartTime[NUM_PUMPS] = {0, 0, 0};
unsigned long pumpLastEnd[NUM_PUMPS]   = {0, 0, 0};

unsigned long lastCheck = 0;

// ==============================================================================
// HARDWARE INITIALIZATION
// ==============================================================================
void setup() {
  Serial.begin(9600);

  // Anti-Glitch: write OFF state before setting pinMode to OUTPUT
  // Prevents active-LOW relays from momentary click/trigger on power up
  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    digitalWrite(RELAY_PINS[i], RELAY_OFF);
    pinMode(RELAY_PINS[i], OUTPUT);
  }

  dht.begin();

  Serial.println(F("Smart gardening system starting..."));
  Serial.println(F("Pumps: Pump 1(D7 auto/man), Pump 2(D6 auto/man), Pump 3(D8 MANUAL ONLY)"));
  Serial.println(F("Commands: '1'=Pump1 | '2'=Pump2 | '3'=Pump3(Manual) | 'A'=Auto ON | 'M'=Manual Only | '0'/'X'=Stop All"));
}

// ==============================================================================
// SENSOR READING & ADC FILTER
// ==============================================================================
int readAnalogFiltered(uint8_t pin) {
  analogRead(pin);       // Dummy read to charge sample capacitor
  delayMicroseconds(20); // Settling pause
  int r1 = analogRead(pin);
  int r2 = analogRead(pin);
  return (r1 + r2) / 2;  // 2-sample average
}

int readMoisturePercent(uint8_t pin) {
  int raw = readAnalogFiltered(pin);
  return constrain(map(raw, DRY_VALUE, WET_VALUE, 0, 100), 0, 100);
}

// ==============================================================================
// PUMP ACTUATION & TIMING CONTROLS
// ==============================================================================
bool startPump(uint8_t i) {
  if (i >= NUM_PUMPS) return false;
  unsigned long now = millis();

  if (pumpRunning[i]) return false; // Already running

  if (pumpLastEnd[i] != 0 && (now - pumpLastEnd[i] < PUMP_COOLDOWN)) {
    Serial.print(F("Pump "));
    Serial.print(i + 1);
    Serial.println(F(" skipped (cooldown)"));
    return false;
  }

  pumpRunning[i]   = true;
  pumpStartTime[i] = now;
  digitalWrite(RELAY_PINS[i], RELAY_ON);

  Serial.print(F("Pump "));
  Serial.print(i + 1);
  Serial.println(F(" ON"));
  return true;
}

void updatePump(uint8_t i) {
  if (pumpRunning[i] && (millis() - pumpStartTime[i] >= PUMP_RUN_TIME)) {
    digitalWrite(RELAY_PINS[i], RELAY_OFF);
    pumpRunning[i] = false;
    pumpLastEnd[i] = millis();

    Serial.print(F("Pump "));
    Serial.print(i + 1);
    Serial.println(F(" OFF"));
  }
}

void stopAllPumps() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    digitalWrite(RELAY_PINS[i], RELAY_OFF);
    pumpRunning[i] = false;
    pumpLastEnd[i] = now;
  }
  Serial.println(F("All pumps shut OFF immediately."));
}

// ==============================================================================
// TELEMETRY TRANSMISSION & AUTOMATIC EVALUATION
// ==============================================================================
void checkSensorsAndAutomate() {
  int soil1 = readMoisturePercent(SOIL1_PIN);
  int soil2 = readMoisturePercent(SOIL2_PIN);
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();
  bool dhtOk = !isnan(temp) && !isnan(hum);

  // PlantoPRO telemetry stream format
  Serial.print(F("SOIL1="));
  Serial.print(soil1);
  Serial.print(F("%  SOIL2="));
  Serial.print(soil2);
  Serial.print(F("%  TEMP="));
  if (dhtOk) Serial.print(temp, 1); else Serial.print(F("ERR"));
  Serial.print(F("C  HUMIDITY="));
  if (dhtOk) Serial.print(hum, 1); else Serial.print(F("ERR"));
  Serial.print(F("%  MODE="));
  Serial.println(autoModeEnabled ? F("AUTO") : F("MANUAL"));

  // Automatic Trigger Logic (only active if autoModeEnabled == true)
  if (autoModeEnabled) {
    // 1. Pump 1: Field 1 auto-pulse when moisture is below 40%
    if (soil1 < MOISTURE_THRESHOLD) {
      startPump(0);
    }

    // 2. Pump 2: Field 2 auto-pulse when moisture is below 40%
    if (soil2 < MOISTURE_THRESHOLD) {
      startPump(1);
    }

    // PUMP 3 IS STRICTLY MANUAL ONLY:
    // Never triggered here automatically.
  }
}

// ==============================================================================
// INBOUND SERIAL COMMAND PARSER (MANUAL COMMANDS)
// ==============================================================================
void processSerialCommands() {
  while (Serial.available() > 0) {
    char cmd = Serial.read();

    // Discard whitespace / line feeds
    if (cmd == '\r' || cmd == '\n' || cmd == ' ') continue;

    switch (cmd) {
      // --- Manual Pump Triggers ---
      case '1':
        Serial.println(F("[MANUAL] Triggering Pump 1"));
        startPump(0);
        break;

      case '2':
        Serial.println(F("[MANUAL] Triggering Pump 2"));
        startPump(1);
        break;

      case '3':
        // Pump 3: Strictly manual override valve
        Serial.println(F("[MANUAL] Triggering Pump 3 (Manual Valve)"));
        startPump(2);
        break;

      // --- Mode Selection for Pumps 1 & 2 ---
      case 'A':
      case 'a':
        autoModeEnabled = true;
        Serial.println(F("[MODE] Automatic Mode ENABLED (Pumps 1 & 2 auto + manual)"));
        break;

      case 'M':
      case 'm':
        autoModeEnabled = false;
        Serial.println(F("[MODE] Manual-Only Mode ENABLED (Auto triggers paused)"));
        break;

      // --- Emergency / Manual Stop ---
      case 'X':
      case 'x':
      case '0':
        Serial.println(F("[COMMAND] Emergency Stop triggered"));
        stopAllPumps();
        break;

      // --- Status Query ---
      case 'S':
      case 's':
      case '?':
        checkSensorsAndAutomate();
        break;

      default:
        break;
    }
  }
}

// ==============================================================================
// MAIN LOOP
// ==============================================================================
void loop() {
  // 1. Instantly process any incoming manual commands ('1', '2', '3', 'A', 'M', 'X')
  processSerialCommands();

  // 2. Service non-blocking 3-second pump duration timers
  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    updatePump(i);
  }

  // 3. Periodic telemetry and automated soil checks (every 5 seconds)
  if (millis() - lastCheck >= CHECK_INTERVAL) {
    lastCheck = millis();
    checkSensorsAndAutomate();
  }
}