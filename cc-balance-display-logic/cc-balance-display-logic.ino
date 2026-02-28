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
const char* base_url = "YOUR SIMPLEFIN URL/simplefin";

// 3. Enter your specific Account ID
const char* target_account_id = "ACT-XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"; // Paste your full ID here

// 4. Enter your Timezone Offset from UTC in SECONDS
// Examples: EST = -18000, CST = -21600, MST = -25200, PST = -28800
const long timezone_offset = -25200; 

// Time to sleep between checks (12 hours)
const uint64_t SLEEP_TIME = 12ULL * 60 * 60 * 1000000;

M5Canvas canvas(&M5.Display);

// --- HELPER FUNCTION: Format Float to Currency ---
String formatBalance(float bal) {
  if (bal < 0.01 && bal > -0.01) bal = 0.0; 
  bal = abs(bal); 
  String balStr = String(bal, 2); 
  
  int dotIndex = balStr.indexOf('.');
  String intPart = balStr.substring(0, dotIndex);
  String decPart = balStr.substring(dotIndex);
  
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
  
  Serial.begin(115200);
  delay(1000); 
  
  M5.Display.setRotation(1); 
  M5.Display.setEpdMode(epd_mode_t::epd_quality); 
  M5.Display.clear(TFT_WHITE);     
  
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  canvas.setTextColor(TFT_BLACK);
  
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    // --- SYNC TIME VIA NTP WITH TIMEZONE ---
    // Using your local timezone prevents the statement from rolling over 
    // a day early due to UTC time differences!
    configTime(timezone_offset, 0, "pool.ntp.org", "time.nist.gov");
    struct tm timeinfo;
    int timeAttempts = 0;
    while (!getLocalTime(&timeinfo) && timeAttempts < 10) {
      delay(500);
      timeAttempts++;
    }
    
    if (timeAttempts < 10) {
      fetchAndDisplayBalance(timeinfo);
    } else {
      canvas.setTextSize(4);
      canvas.drawString("NTP Time Sync Failed.", 50, 50);
      canvas.pushSprite(0, 0); 
    }
  } else {
    canvas.setTextSize(4);
    canvas.drawString("Wi-Fi Connection Failed.", 50, 50);
    canvas.pushSprite(0, 0); 
  }

  delay(2500); 
  esp_sleep_enable_timer_wakeup(SLEEP_TIME);
  esp_deep_sleep_start();
}

void loop() {}

void fetchAndDisplayBalance(struct tm timeinfo) {
  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure(); 
  HTTPClient http;

  // --- 1. CALCULATE THE 18TH OF THE MONTH ---
  struct tm statement_tm = timeinfo;
  statement_tm.tm_hour = 0;
  statement_tm.tm_min = 0;
  statement_tm.tm_sec = 0;
  
  if (timeinfo.tm_mday < 18) {
    statement_tm.tm_mon -= 1;
    if (statement_tm.tm_mon < 0) {
      statement_tm.tm_mon = 11;
      statement_tm.tm_year -= 1;
    }
  }
  statement_tm.tm_mday = 18;
  time_t statement_epoch = mktime(&statement_tm);

  char dateString[64];
  strftime(dateString, sizeof(dateString), "Since %b 18", &statement_tm);

  // --- 2. FETCH ACCOUNT & TRANSACTIONS ---
  // We still try to pass the start-date to be polite to SimpleFIN's servers
  String accounts_url = String(base_url) + "/accounts?start-date=" + String((unsigned long)statement_epoch);
  
  http.begin(*client, accounts_url);
  http.setTimeout(20000); 
  int httpCode = http.GET();
  
  if (httpCode != HTTP_CODE_OK) {
    canvas.fillSprite(TFT_WHITE);
    canvas.setTextSize(4);
    canvas.drawString("HTTP Request Failed.", 50, 100);
    canvas.pushSprite(0, 0);
    delete client;
    return;
  }

  // --- 3. THE "HARD FILTER" JSON PARSER ---
  DynamicJsonDocument filter(512);
  filter["accounts"][0]["id"] = true;
  filter["accounts"][0]["name"] = true;
  filter["accounts"][0]["balance"] = true;
  filter["accounts"][0]["transactions"][0]["amount"] = true;
  // NEW: Tell Arduino to extract the transaction dates!
  filter["accounts"][0]["transactions"][0]["posted"] = true; 
  
  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  
  if (err) {
    canvas.fillSprite(TFT_WHITE);
    canvas.setTextSize(4);
    canvas.drawString("JSON Parse Error.", 50, 100);
    canvas.pushSprite(0, 0);
    delete client;
    return;
  }

  String acctName = "";
  float totalBalance = 0.0;
  float newPurchases = 0.0;
  bool accountFound = false;

  JsonArray accounts = doc["accounts"].as<JsonArray>();
  for (JsonObject acct : accounts) {
    if (acct["id"].as<String>() == String(target_account_id)) {
      acctName = acct["name"].as<String>();
      totalBalance = abs(acct["balance"].as<float>());
      accountFound = true;
      
      JsonArray transactions = acct["transactions"].as<JsonArray>();
      for (JsonObject tx : transactions) {
        
        // NEW: The Hard Date Filter
        long postedDate = tx["posted"].as<long>();
        if (postedDate >= statement_epoch) { 
          
          float amt = tx["amount"].as<float>();
          if (amt < 0) { 
            newPurchases += abs(amt);
          }
          
        }
      }
      break; 
    }
  }

  canvas.fillSprite(TFT_WHITE);

  if (!accountFound) {
    canvas.setTextSize(4);
    canvas.drawString("Account ID Not Found!", 50, 100);
    canvas.pushSprite(0, 0);
    delete client;
    return; 
  }

  // --- 4. CALCULATE BUDGET LOGIC ---
  float statementBalance = max(0.0f, totalBalance - newPurchases);
  float mainDisplayBalance = totalBalance - statementBalance;

  // --- 5. DRAW TO SCREEN ---
  int batPercent = M5.Power.getBatteryLevel();
  canvas.setTextSize(3);
  canvas.drawString(String(batPercent) + "%", 860, 20);
  canvas.drawRoundRect(790, 20, 50, 25, 4, TFT_BLACK);        
  canvas.fillRoundRect(840, 26, 4, 13, 2, TFT_BLACK);         
  canvas.fillRoundRect(792, 22, (batPercent * 46) / 100, 21, 2, TFT_BLACK); 

  canvas.drawRoundRect(50, 80, 860, 230, 15, TFT_BLACK); 

  canvas.setTextSize(4);
  canvas.drawString(acctName, 80, 100);
  
  canvas.setTextSize(10); 
  canvas.drawString(formatBalance(mainDisplayBalance), 80, 150);
  
  if (statementBalance > 0.01) { 
    canvas.setTextSize(3);
    String statementText = "Unpaid Statement: " + formatBalance(statementBalance);
    canvas.drawString(statementText, 80, 240);
  } else {
    canvas.setTextSize(3);
    canvas.drawString("Statement Paid in Full!", 80, 240);
  }

  canvas.setTextSize(2);
  canvas.drawString(String(dateString) + " (Updates Daily)", 80, 280);

  canvas.pushSprite(0, 0);
  delete client;
}