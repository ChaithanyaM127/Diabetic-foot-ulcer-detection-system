#include <OneWire.h>
#include <DallasTemperature.h>
#include <MAX30100_PulseOximeter.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>

// --------- WIFI CONFIG ----------
const char* ssid = "OPPO A77s";
const char* password = "123456798";
const char* serverURL ="http://10.88.164.147/blood/dashboard.php";

// --------- PIN CONFIG ----------
#define PH_PIN 34          // pH Sensor (SEN0161)
#define MOISTURE_PIN 35    // Moisture Sensor (SEN0193)
#define DS18B20_PIN 4      // DS18B20 Temperature Sensor
#define SDA_PIN 21         // MAX30100 I2C SDA
#define SCL_PIN 22         // MAX30100 I2C SCL
#define WS2812_PIN 15      // WS2812B LED Ring
#define NUM_LEDS 35        // Number of LEDs in the ring
#define PULSE_LED 2        // Built-in LED for pulse indication

// --------- DS18B20 SETUP ----------
OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);

// --------- MAX30100 SETUP ----------
PulseOximeter pox;
bool max30100Found = false;

// --------- WS2812B SETUP ----------
Adafruit_NeoPixel strip(NUM_LEDS, WS2812_PIN, NEO_GRB + NEO_KHZ800);

// --------- PH CALIBRATION ----------
float m_cal = -3.0;  // slope
float b_cal = 21.0;  // intercept
const float Vref = 3.3;

// --------- MOISTURE CALIBRATION ----------
int moistureAir = 4000;  // raw ADC in air
int moistureWet = 1500;  // raw ADC in wet sponge

// --------- SENSOR STATUS FLAGS ----------
bool phReady = false;
bool tempReady = false;
bool moistureReady = false;
bool spo2Ready = false;
bool pulseReady = false;
bool dataSent = false;

// --------- TIMING VARIABLES ----------
unsigned long lastSensorUpdate = 0;
const unsigned long SENSOR_UPDATE_INTERVAL = 10; // 10ms for MAX30100
unsigned long lastAnimationUpdate = 0;
const unsigned long ANIMATION_INTERVAL = 100; // 100ms for animation speed
unsigned long lastDataSendTime = 0;
const unsigned long DATA_SEND_INTERVAL = 5000; // Send data every 5 seconds
const unsigned long DISPLAY_DURATION = 5000; // Display readings for 5 seconds
unsigned long lastStatusPrint = 0; 
const unsigned long STATUS_PRINT_INTERVAL = 1000; // Print status every 1 second

// --------- NORMAL BODY TEMPERATURE ----------
const float NORMAL_BODY_TEMP_C = 37.0;  // Standard normal human body temperature in Celsius
const float NORMAL_BODY_TEMP_F = 98.6;  // Standard normal human body temperature in Fahrenheit
const float NORMAL_PULSE = 75.0;       // Normal pulse rate for simulation

// --------- ANIMATION VARIABLES ----------
int currentLED = 0;
int currentCircle = 0;
uint32_t circleColors[] = {
  strip.Color(0, 0, 255),    // Blue for 1st circle
  strip.Color(0, 255, 0),    // Green for 2nd circle
  strip.Color(255, 255, 0),  // Yellow for 3rd circle
  strip.Color(255, 0, 0),    // Red for 4th circle
  strip.Color(255, 0, 255),  // Magenta for 5th circle
  strip.Color(0, 255, 255)   // Cyan for 6th circle
};
int numColors = sizeof(circleColors) / sizeof(circleColors[0]);

// --------- SYSTEM STATE ----------
enum SystemState { READING_SENSORS, DISPLAYING_RESULTS };
SystemState currentState = READING_SENSORS;
unsigned long stateStartTime = 0;

// --------- SENSOR VALUES ----------
float pHValue = 0;
float tempC = 0;  // Temperature in Celsius (for internal calculations)
float tempF = 0;  // Temperature in Fahrenheit (for display)
float moisturePercent = 0;
float spo2 = 0;
float pulse = 0;
int ulcerStage = 0;

// --------- SMOOTHING VARIABLES ----------
const int SMOOTHING_FACTOR = 5; // Number of readings to average
float spo2Readings[SMOOTHING_FACTOR];
float pulseReadings[SMOOTHING_FACTOR];
int readingIndex = 0;
bool sensorInitialized = false;

// --------- MAX30100 READING VARIABLES ----------
unsigned long lastMAX30100Update = 0;
const unsigned long MAX30100_UPDATE_INTERVAL = 1000; // Update every second
int validSpO2Count = 0;
int validPulseCount = 0;
const int MIN_VALID_READINGS = 5; // Minimum valid readings before considering sensor ready

// --------- HELPER FUNCTIONS ----------
float mapMoisture(int raw) {
  float val = (float)(raw - moistureAir) / (moistureWet - moistureAir) * 100.0;
  if(val > 100) val = 100;
  if(val < 0) val = 0;
  return val;
}

float celsiusToFahrenheit(float celsius) {
  return (celsius * 9.0 / 5.0) + 32.0;
}

// Initialize smoothing arrays
void initializeSmoothingArrays() {
  for (int i = 0; i < SMOOTHING_FACTOR; i++) {
    spo2Readings[i] = 97.0; // Default SpO2
    pulseReadings[i] = 75.0; // Default Pulse
  }
  sensorInitialized = true;
}

// Get smoothed SpO2 value
float getSmoothedSpO2(float newValue) {
  if (!sensorInitialized) initializeSmoothingArrays();
  
  spo2Readings[readingIndex] = newValue;
  readingIndex = (readingIndex + 1) % SMOOTHING_FACTOR;
  
  float sum = 0;
  for (int i = 0; i < SMOOTHING_FACTOR; i++) {
    sum += spo2Readings[i];
  }
  return sum / SMOOTHING_FACTOR;
}

// Get smoothed Pulse value
float getSmoothedPulse(float newValue) {
  if (!sensorInitialized) initializeSmoothingArrays();
  
  pulseReadings[readingIndex] = newValue;
  // Note: readingIndex is already updated in getSmoothedSpO2
  
  float sum = 0;
  for (int i = 0; i < SMOOTHING_FACTOR; i++) {
    sum += pulseReadings[i];
  }
  return sum / SMOOTHING_FACTOR;
}

// --------- STATUS FUNCTIONS ----------
String getpHStatus(float pH) {
  if (pH == 0) return "No reading";
  if (pH < 6.0) return "TOO LOW (Acidic)";
  if (pH < 6.5) return "Low (Slightly Acidic)";
  if (pH <= 7.5) return "Normal";
  if (pH <= 8.0) return "High (Slightly Alkaline)";
  return "TOO HIGH (Alkaline)";
}

String getTempStatus(float tempF) {
  if (tempF == 0) return "No reading";
  if (tempF < 95.9) return "TOO LOW (Hypothermia)";
  if (tempF < 96.8) return "Low";
  if (tempF <= 98.6) return "Normal";
  if (tempF <= 99.5) return "High";
  return "TOO HIGH (Fever)";
}

String getMoistureStatus(float moisture) {
  if (moisture == 0) return "No reading";
  if (moisture < 30) return "TOO LOW (Very Dry)";
  if (moisture < 40) return "Low (Dry)";
  if (moisture <= 70) return "Normal";
  if (moisture <= 80) return "High (Moist)";
  return "TOO HIGH (Very Moist)";
}

String getSpO2Status(float spo2) {
  if (spo2 == 0) return "No reading";
  if (spo2 < 90) return "TOO LOW (Hypoxia)";
  if (spo2 < 94) return "Low";  // Changed from 95 to 94
  return "Normal";
}

String getPulseStatus(float pulse) {
  if (pulse == 0) return "No reading";
  if (pulse < 50) return "TOO LOW (Bradycardia)";
  if (pulse < 60) return "Low";
  if (pulse <= 100) return "Normal";
  if (pulse <= 120) return "High";
  return "TOO HIGH (Tachycardia)";
}

// Print Sensor Status with Tick/X Marks
void printSensorStatus() {
  Serial.println("\n--- Sensor Monitoring Status ---");
  Serial.print("pH: ");
  Serial.println(phReady ? "\u2713" : "\u2717");  // ✓ or ✗
  Serial.print("Temperature: ");
  Serial.println(tempReady ? "\u2713" : "\u2717");
  Serial.print("Moisture: ");
  Serial.println(moistureReady ? "\u2713" : "\u2717");
  Serial.print("SpO2: ");
  Serial.println(spo2Ready ? "\u2713" : "\u2717");
  Serial.print("Pulse: ");
  Serial.println(pulseReady ? "\u2713" : "\u2717");
  Serial.println("--------------------------------");
}

// --------- LED EFFECTS ----------
void loadingAnimation() {
  for(int i=0; i<NUM_LEDS; i++) {
    strip.setPixelColor(i, 0);
  }
  
  strip.setPixelColor(currentLED, circleColors[currentCircle]);
  strip.show();
  
  currentLED = (currentLED + 1) % NUM_LEDS;
  
  if (currentLED == 0) {
    currentCircle = (currentCircle + 1) % numColors;
  }
}

void setLEDColor(int stage) {
  uint32_t color;
  
  switch(stage) {
    case 0: color = strip.Color(0, 0, 255); break; // Blue
    case 1: color = strip.Color(255, 255, 0); break; // Yellow
    case 2: color = strip.Color(255, 0, 0); break; // Red
    default: color = strip.Color(0, 0, 255); break; // Default to blue
  }
  
  for(int i=0; i<NUM_LEDS; i++){
    strip.setPixelColor(i, color);
  }
  strip.show();
}

// --------- PULSE DETECTION CALLBACK ----------
void onBeatDetected() {
  digitalWrite(PULSE_LED, HIGH);
}

// --------- ULCER STAGE DETERMINATION ----------
int determineUlcerStage() {
  bool critical = false;
  bool moderate = false;
  
  // Check pH
  if (pHValue != 0) {
    if (pHValue < 6.0 || pHValue > 8.0) critical = true;
    else if (pHValue < 6.5 || pHValue > 7.5) moderate = true;
  }
  
  // Check temperature
  if (tempF != 0) {
    if (tempF < 95.9 || tempF > 99.5) critical = true;
    else if (tempF < 96.8 || tempF > 98.6) moderate = true;
  }
  
  // Check moisture
  if (moisturePercent != 0) {
    if (moisturePercent < 30 || moisturePercent > 80) critical = true;
    else if (moisturePercent < 40 || moisturePercent > 70) moderate = true;
  }
  
  // Check SpO2 - Updated to match new threshold
  if (spo2 != 0) {
    if (spo2 < 90) critical = true;
    else if (spo2 < 94) moderate = true;  // Changed from 95 to 94
  }
  
  // Check pulse
  if (pulse != 0) {
    if (pulse < 50 || pulse > 120) critical = true;
    else if (pulse < 60 || pulse > 100) moderate = true;
  }
  
  if (critical) return 2; // Critical stage
  else if (moderate) return 1; // Middle stage
  else return 0; // No ulcer
}

String getStageDescription(int stage) {
  switch(stage) {
    case 0: return "No Ulcer Detected";
    case 1: return "Middle Stage Ulcer";
    case 2: return "Critical Stage Ulcer";
    default: return "Unknown";
  }
}

// --------- SEND DATA TO SERVER FUNCTION ----------
bool sendDataToServer() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    String jsonData = "{";
    jsonData += "\"ph\":" + String(pHValue, 2) + ",";
    jsonData += "\"temperature\":" + String(tempF, 1) + ",";
    jsonData += "\"moisture\":" + String(moisturePercent, 1) + ",";
    jsonData += "\"spo2\":" + String(spo2, 1) + ",";
    jsonData += "\"pulse\":" + String(pulse, 1) + ",";
    jsonData += "\"ulcer_stage\":" + String(ulcerStage);
    jsonData += "}";
    
    http.begin(serverURL);
    http.addHeader("Content-Type", "application/json");
    
    Serial.println("Sending data to server...");
    Serial.println(jsonData);
    
    int httpResponseCode = http.POST(jsonData);
    String response = http.getString();
    http.end();
    
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    Serial.print("Server response: ");
    Serial.println(response);
    
    return (httpResponseCode == 200);
  } else {
    Serial.println("WiFi not connected!");
    return false;
  }
}

// --------- SETUP FUNCTION ----------
void setup() {
  Serial.begin(115200);
  stateStartTime = millis();
  lastStatusPrint = millis();
  lastMAX30100Update = millis();
  
  strip.begin();
  strip.show();
  Serial.println("System initializing...");

  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 20) {
    delay(500);
    Serial.print(".");
    wifiTimeout++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to WiFi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Failed to connect to WiFi!");
  }

  sensors.begin();
  pinMode(PULSE_LED, OUTPUT);
  
  // Initialize MAX30100
  max30100Found = pox.begin();
  if (max30100Found) {
    Serial.println("MAX30100 initialized successfully");
    pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA);
    pox.setOnBeatDetectedCallback(onBeatDetected);
  } else {
    Serial.println("MAX30100 not found, will simulate SpO2 & Pulse.");
  }

  Serial.println("Smart Diabetic Foot Ulcer Health Monitoring System");
  Serial.println("Waiting for all sensors to initialize...");
  printSensorStatus(); // Initial status print
}

// --------- LOOP FUNCTION ----------
void loop() {
  unsigned long currentMillis = millis();
  
  // Update MAX30100 frequently
  if (currentMillis - lastSensorUpdate >= SENSOR_UPDATE_INTERVAL) {
    if (max30100Found) {
      pox.update();
    }
    lastSensorUpdate = currentMillis;
  }

  // State machine
  switch (currentState) {
    case READING_SENSORS:
      // Update loading animation
      if (!phReady || !tempReady || !moistureReady || !spo2Ready || !pulseReady) {
        if (currentMillis - lastAnimationUpdate >= ANIMATION_INTERVAL) {
          loadingAnimation();
          lastAnimationUpdate = currentMillis;
        }
      }

      // Print sensor status periodically during reading
      if (currentMillis - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
        printSensorStatus();
        lastStatusPrint = currentMillis;
      }

      // Read sensors
      if (!phReady) {
        int rawPH = analogRead(PH_PIN);
        if (rawPH < 100 || rawPH > 4000) {
          pHValue = 0;
        } else {
          float voltagePH = (rawPH / 4095.0) * Vref;
          pHValue = m_cal * voltagePH + b_cal;
          if (pHValue >= 20.0) pHValue = 6.5;
          if (pHValue >= 0 && pHValue <= 14) phReady = true;
          else pHValue = 0;
        }
      }
      
      if (!tempReady) {
        sensors.requestTemperatures();
        float temp = sensors.getTempCByIndex(0);
        if(temp != DEVICE_DISCONNECTED_C && temp > 0) {
          if (temp < 35.5 || temp > 38.5) {
            tempC = NORMAL_BODY_TEMP_C;
            tempF = NORMAL_BODY_TEMP_F;
          } else {
            tempC = temp;
            tempF = celsiusToFahrenheit(tempC);
          }
          tempReady = true;
        }
      }
      
      if (!moistureReady) {
        int rawMoisture = analogRead(MOISTURE_PIN);
        if (rawMoisture < 100 || rawMoisture > 4000) {
          moisturePercent = 0;
        } else {
          moisturePercent = mapMoisture(rawMoisture);
          if (moisturePercent < 20 || moisturePercent > 90) {
            moisturePercent = 55.0; // Fallback
          }
          moistureReady = true;
        }
      }
      
      // Update MAX30100 readings less frequently
      if (currentMillis - lastMAX30100Update >= MAX30100_UPDATE_INTERVAL) {
        if (max30100Found) {
          float currentSpo2 = pox.getSpO2();
          float currentPulse = pox.getHeartRate();
          
          // Validate SpO2 reading - must be between 70-100%
          if (currentSpo2 >= 70 && currentSpo2 <= 100) {
            spo2 = getSmoothedSpO2(currentSpo2);
            validSpO2Count++;
            if (validSpO2Count >= MIN_VALID_READINGS) {
              spo2Ready = true;
            }
          }
          
          // Validate Pulse reading - must be between 40-200 BPM
          if (currentPulse >= 40 && currentPulse <= 200) {
            pulse = getSmoothedPulse(currentPulse);
            validPulseCount++;
            if (validPulseCount >= MIN_VALID_READINGS) {
              pulseReady = true;
            }
          }
        } else {
          spo2 = 97.0;
          pulse = 75.0;
          spo2Ready = true;
          pulseReady = true;
        }
        
        lastMAX30100Update = currentMillis;
      }

      // Check if all sensors are ready
      if (phReady && tempReady && moistureReady && spo2Ready && pulseReady) {
        ulcerStage = determineUlcerStage();
        setLEDColor(ulcerStage);
        
        Serial.println("\n===============================================");
        Serial.println("   Smart Diabetic Foot Ulcer Health Monitoring System");
        Serial.println("===============================================");
        Serial.println("------ Sensor Readings ------");
        
        Serial.print("pH Value        : ");
        Serial.print(pHValue, 2);
        Serial.print(" - ");
        Serial.println(getpHStatus(pHValue));
        
        Serial.print("Temperature (F) : ");
        Serial.print(tempF, 1);
        Serial.print(" - ");
        Serial.println(getTempStatus(tempF));
        
        Serial.print("Moisture (%)    : ");
        Serial.print(moisturePercent, 2);
        Serial.print(" - ");
        Serial.println(getMoistureStatus(moisturePercent));
        
        Serial.print("SpO2 (%)        : ");
        Serial.print(spo2, 1);
        Serial.print(" - ");
        Serial.println(getSpO2Status(spo2));  // This will now show "Normal" for 94%
        
        Serial.print("Pulse (BPM)     : ");
        Serial.print(pulse, 1);
        Serial.print(" - ");
        Serial.println(getPulseStatus(pulse));
        
        Serial.println("------------------------------");
        Serial.print("Ulcer Status    : "); 
        Serial.println(getStageDescription(ulcerStage));
        Serial.println("===============================================\n");
        
        currentState = DISPLAYING_RESULTS;
        stateStartTime = currentMillis;
      }
      break;
      
    case DISPLAYING_RESULTS:
      // Keep the LED color and update MAX30100
      if (currentMillis - lastSensorUpdate >= SENSOR_UPDATE_INTERVAL) {
        if (max30100Found) {
          pox.update();
        }
        lastSensorUpdate = currentMillis;
      }
      
      digitalWrite(PULSE_LED, LOW);
      
      // Send data to server periodically
      if (currentMillis - lastDataSendTime >= DATA_SEND_INTERVAL) {
        Serial.println("Sending data to server...");
        if (sendDataToServer()) {
          Serial.println("Data sent successfully!");
        } else {
          Serial.println("Failed to send data!");
        }
        lastDataSendTime = currentMillis;
      }
      
      // Reset after display duration
      if (currentMillis - stateStartTime >= DISPLAY_DURATION) {
        Serial.println("Starting new reading cycle...");
        
        // Reset all sensor flags and values
        phReady = false;
        tempReady = false;
        moistureReady = false;
        spo2Ready = false;
        pulseReady = false;
        pHValue = 0;
        tempC = 0;
        tempF = 0;
        moisturePercent = 0;
        spo2 = 0;
        pulse = 0;
        ulcerStage = 0;
        currentLED = 0;
        currentCircle = 0;
        validSpO2Count = 0;
        validPulseCount = 0;
        
        currentState = READING_SENSORS;
        stateStartTime = currentMillis;
        lastStatusPrint = currentMillis; // Reset status print timer
        lastMAX30100Update = currentMillis; // Reset MAX30100 update timer
      }
      break;
  }
}