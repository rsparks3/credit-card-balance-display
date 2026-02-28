#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

// 1. Enter your Wi-Fi details
const char* ssid = "WIFI SSID";
const char* password = "WIFI PASSWORD";

// 2. Enter your private SimpleFIN URL
const char* private_url = "YOUR SIMPLEFIN URL/simplefin/accounts?account=ACT-XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX";

// Time to sleep between checks (12 hours in microseconds)
const uint64_t SLEEP_TIME = 12ULL * 60 * 60 * 1000000;

M5Canvas canvas(&M5.Display);

// --- HELPER FUNCTION: Format the Balance ---
String formatBalance(String balStr) {
  if (balStr.startsWith("-")) {
    balStr = balStr.substring(1);
  }
  
  int dotIndex = balStr.indexOf('.');
  String intPart = balStr;
  String decPart = "";
  if (dotIndex != -1) {
    intPart = balStr.substring(0, dotIndex);
    decPart = balStr.substring(dotIndex);
  }
  
  String result = "";
  int len = intPart.length();
  for (int i = 0; i < len; i++) {
    result += intPart[i];
    if ((len - 1 - i) % 3 == 0 && i != len - 1) {
      result += ",";
    }
  }
  return "$" + result + decPart;
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  
  // --- START DEBUGGING ---
  Serial.begin(115200);
  delay(1000); 
  Serial.println("\n\n--- M5Paper Booting (M5Unified) ---");
  
  M5.Display.setRotation(1); 
  M5.Display.setEpdMode(epd_mode_t::epd_quality); 
  M5.Display.clear(TFT_WHITE);     
  
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  canvas.setTextColor(TFT_BLACK);
  
  Serial.print("Attempting to connect to SSID: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  int attempts = 0;
  
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print("."); 
    attempts++;
  }
  Serial.println(); 

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("SUCCESS: Wi-Fi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    fetchAndDisplayBalance();
  } else {
    Serial.println("ERROR: Wi-Fi Connection Failed.");
    Serial.print("Final Wi-Fi Status Code: ");
    Serial.println(WiFi.status());

    canvas.setTextSize(4);
    canvas.drawString("Wi-Fi Connection Failed.", 50, 50);
    canvas.pushSprite(0, 0); 
  }

  Serial.println("Waiting for E-Ink screen to finish drawing...");
  delay(2500); 

  Serial.println("Going to Deep Sleep...");
  esp_sleep_enable_timer_wakeup(SLEEP_TIME);
  esp_deep_sleep_start();
}

void loop() {
  // Empty
}

void fetchAndDisplayBalance() {
  Serial.println("Starting HTTPS connection to SimpleFIN...");
  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure(); 

  HTTPClient http;
  http.begin(*client, private_url);

  Serial.println("Sending GET Request...");
  int httpCode = http.GET();
  
  Serial.print("HTTP Response Code: ");
  Serial.println(httpCode);

  canvas.fillSprite(TFT_WHITE); 

  // --- DRAW THE BATTERY ICON ---
  int batPercent = M5.Power.getBatteryLevel();
  Serial.print("Battery Level: ");
  Serial.print(batPercent);
  Serial.println("%");

  canvas.setTextSize(3);
  canvas.drawString(String(batPercent) + "%", 860, 20);
  canvas.drawRoundRect(790, 20, 50, 25, 4, TFT_BLACK);        
  canvas.fillRoundRect(840, 26, 4, 13, 2, TFT_BLACK);         
  int fillWidth = (batPercent * 46) / 100;     
  canvas.fillRoundRect(792, 22, fillWidth, 21, 2, TFT_BLACK); 

  // --- PARSE AND DRAW THE DATA ---
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.println("Received Payload:");
    Serial.println(payload);
    
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print("JSON Parsing Failed: ");
      Serial.println(error.c_str());
      
      canvas.setTextSize(4);
      canvas.drawString("Error parsing bank data.", 50, 100);
      canvas.drawString(error.c_str(), 50, 150);
    } else {
      Serial.println("JSON Parsed Successfully.");
      const char* acctName = doc["accounts"][0]["name"];
      const char* balanceStr = doc["accounts"][0]["balance"];
      long balanceDate = doc["accounts"][0]["balance-date"]; 
      
      Serial.print("Account: "); Serial.println(acctName);
      Serial.print("Balance: "); Serial.println(balanceStr);

      time_t rawtime = (time_t)balanceDate;
      struct tm * timeinfo = localtime(&rawtime);
      char timeString[64];
      strftime(timeString, sizeof(timeString), "Balance as of %b %d, %Y", timeinfo);

      canvas.drawRoundRect(50, 80, 860, 200, 15, TFT_BLACK); 
      canvas.setTextSize(4);
      canvas.drawString(acctName, 80, 100);
      canvas.setTextSize(10); 
      String displayAmount = formatBalance(String(balanceStr));
      canvas.drawString(displayAmount, 80, 150);
      canvas.setTextSize(2);
      canvas.drawString(timeString, 80, 245);
    }
  } else {
    Serial.println("HTTP Request Failed.");
    canvas.setTextSize(4);
    canvas.drawString("Failed to fetch data.", 50, 100);
    canvas.drawString("HTTP Code: " + String(httpCode), 50, 150);
  }

  Serial.println("Pushing graphic to E-Ink display...");
  canvas.pushSprite(0, 0);
  Serial.println("Display update complete.");

  http.end();
  delete client;
}