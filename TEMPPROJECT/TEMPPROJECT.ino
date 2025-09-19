/* ESP32 - DHT (11/22) + DS3231 RTC + SSD1306 OLED + Firebase RTDB (+Blynk optional)
   Per 60s: Take RTC time, read DHT, write to OLED, write to Firebase.
   NOTE: edit DHTTYPE: DHT11 or DHT22.
*/

/* From Blynk IoT (Take from Console) - (you can erase if you don't want mobil application) */
#define BLYNK_TEMPLATE_ID   "TMPL647V3aX05"
#define BLYNK_TEMPLATE_NAME "TEMPPROJECT"
#define BLYNK_AUTH_TOKEN    "EkzaELEiyJr4CBiXgRox8omvGicj7pLP"

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <RTClib.h>
#include <BlynkSimpleEsp32.h>

// Firebase (Mobizt)
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ---------- CONFIG ----------
const char* WIFI_SSID = "nurss";     //Your wi-fi name
const char* WIFI_PASSWORD = "nurss123";     //Your wi-fi password

#define API_KEY       "AIzaSyBohqbLoE33LVaahqRCffYA9_4P5PqKstY"
#define DATABASE_URL  "https://tempproject-f7b51-default-rtdb.europe-west1.firebasedatabase.app/"
#define USER_EMAIL    "tempproject@gmail.com"
#define USER_PASSWORD "TEMPPROJECT"

// --------- SENSOR / DISPLAY PINS & TYPES ----------
#define DHTPIN 14
// ====== SET YOUR SENSOR TYPE HERE ======
#define DHTTYPE DHT11    // <--- Eğer sensör DHT22 ise burayı DHT22 olarak değiştir
DHT dht(DHTPIN, DHTTYPE);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

RTC_DS3231 rtc;

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

String databasePath; // "/UsersData/<uid>/readings"

// timing
unsigned long prevMillis = 0;
const unsigned long sendInterval = 60000UL; // 60s

// forward
void connectWiFi();
void scanI2C();

void connectWiFi(){
  Serial.print("WiFi connecting");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
    if (millis() - t0 > 30000) {
      Serial.println("\nWiFi timeout, restarting...");
      ESP.restart();
    }
  }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  delay(100);

  Wire.begin(); // default SDA=21, SCL=22
  delay(20);

  Serial.println("I2C scan:");
  scanI2C();

  // OLED init (try 0x3C then 0x3D)
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 @0x3C fail, trying 0x3D");
    delay(10);
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)){
      Serial.println("SSD1306 init failed - check wiring and address!");
    } else {
      Serial.println("SSD1306 OK @0x3D");
    }
  } else {
    Serial.println("SSD1306 OK @0x3C");
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // DHT init
  dht.begin();

  // RTC init
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC (DS3231) - check connection!");
  } else {
    Serial.println("RTC found");
  }

  connectWiFi();

  // Blynk (optional)
  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASSWORD);

  // Sync time from NTP and update RTC if available
  Serial.println("NTP sync attempt...");
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  delay(2000);
  time_t t = time(nullptr);
  if (t > 1600000000UL) { // reasonable epoch (2020+)
    Serial.print("NTP time OK: "); Serial.println((unsigned long)t);
    // set RTC
    rtc.adjust(DateTime((uint32_t)t));
    Serial.println("RTC adjusted from NTP.");
  } else {
    Serial.println("NTP failed or no network time obtained.");
  }

  // Firebase config
  config.api_key = API_KEY;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;
  config.max_token_generation_retry = 5;
  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(4096);

  Firebase.begin(&config, &auth);
  Serial.println("Firebase init, waiting for UID...");
  unsigned long waitStart = millis();
  while ((auth.token.uid) == "") {
    Serial.print(".");
    delay(500);
    if (millis() - waitStart > 20000) {
      Serial.println("\nFirebase auth timeout, continuing (check auth).");
      break;
    }
  }
  String uid = auth.token.uid.c_str();
  Serial.println();
  Serial.print("UID: "); Serial.println(uid);
  databasePath = "/UsersData/" + uid + "/readings";
  Serial.print("Database path: "); Serial.println(databasePath);

  // show start screen
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("Weather Station");
  display.println("Initializing...");
  display.display();
}

void loop() {
  Blynk.run();

  if (millis() - prevMillis >= sendInterval) {
    prevMillis = millis();

    // read DHT
    float t = dht.readTemperature(); // C
    float h = dht.readHumidity();

    // debug prints
    Serial.print("Raw DHT read -> T: ");
    Serial.print(t, 4);
    Serial.print("  H: ");
    Serial.println(h, 4);

    // sanity check: DHT can produce NaN on failure, or wildly wrong values:
    bool invalid = false;
    if (isnan(t) || isnan(h)) invalid = true;
    if (t < -40.0 || t > 80.0) invalid = true;
    if (h < 0.0 || h > 100.0) invalid = true;

    if (invalid) {
      Serial.println("Invalid DHT reading -> skipping write. Check wiring/sensor type.");
      // show on screen
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0,0);
      display.println("DHT read failed");
      display.display();
      return; // skip this cycle
    }

    // read RTC time (use DS3231). If not available use NTP fallback
    DateTime now = rtc.now();
    uint32_t epoch;
    if(now.year() < 2000) {
      // RTC not set or invalid, fallback to NTP/ESP time
      epoch = (uint32_t) time(nullptr);
      Serial.println("RTC invalid; using NTP epoch as fallback.");
    } else {
      epoch = (uint32_t) now.unixtime();
    }

    // Format and display on OLED
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(0,0);
    display.print("T: "); display.print(t, 1); display.println("C");
    display.print("H: "); display.print(h, 1); display.println("%");
    display.setTextSize(1);
    display.setCursor(0, 44);
    // print date/time (if RTC valid)
    if(now.year() >= 2000) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d",
               now.day(), now.month(), now.year(),
               now.hour(), now.minute(), now.second());
      display.println(buf);
    } else {
      time_t tt = epoch;
      struct tm *tm = localtime(&tt);
      char buf[32];
      snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d",
               tm->tm_mday, tm->tm_mon+1, tm->tm_year+1900,
               tm->tm_hour, tm->tm_min, tm->tm_sec);
      display.println(buf);
    }
    display.display();

    // send to Blynk (if used)
    Blynk.virtualWrite(V1, t);
    Blynk.virtualWrite(V2, h);

    // Firebase write
    String node = databasePath + "/" + String(epoch);

    // write numeric fields individually (avoid string/locale conversions)
    String p_temp = node + "/temperature";
    String p_hum  = node + "/humidity";
    String p_ts   = node + "/timestamp";

    bool ok1 = Firebase.RTDB.setFloat(&fbdo, p_temp.c_str(), t);
    bool ok2 = Firebase.RTDB.setFloat(&fbdo, p_hum.c_str(), h);
    bool ok3 = Firebase.RTDB.setInt(&fbdo, p_ts.c_str(), (long)epoch);

    // additionally update a 'latest' short summary
    FirebaseJson json;
    json.set("temperature", t);
    json.set("humidity", h);
    json.set("timestamp", (long)epoch);
    bool okLatest = Firebase.RTDB.setJSON(&fbdo, (databasePath + "/latest").c_str(), &json);

    if(ok1 && ok2 && ok3 && okLatest) {
      Serial.println("Firebase push OK (history + latest).");
    } else {
      Serial.print("Firebase error: ");
      Serial.println(fbdo.errorReason());
    }
  }
}

// ----- utility: I2C scanner -----
void scanI2C(){
  byte error, address;
  int nDevices = 0;
  Serial.println("Scanning I2C bus...");
  for(address = 1; address < 127; address++ ) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("I2C device found at 0x");
      if (address<16) Serial.print("0");
      Serial.print(address, HEX);
      Serial.println(" !");
      nDevices++;
    } else if (error==4) {
      Serial.print("Unknown error at 0x");
      if (address<16) Serial.print("0");
      Serial.println(address, HEX);
    }
  }
  if (nDevices == 0) Serial.println("No I2C devices found. Check wiring.");
  else Serial.println("I2C scan done.");
}
