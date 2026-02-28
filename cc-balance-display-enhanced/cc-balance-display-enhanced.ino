#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

// 1. Enter your Wi-Fi details
const char* ssid = "WIFI SSID";
const char* password = "WIFI PASSWORD";

// 2. Enter your private SimpleFIN Access URL (WITHOUT the account=... part)
const char* simplefin_access_url = "YOUR SIMPLEFIN URL/simplefin/accounts?";

// 3. Enter your account IDs and their statement dates
struct AccountConfig {
  const char* id;
  int statement_day;
};

AccountConfig accounts[] = {
  {"ACT-XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX", 24},
  {"ACT-b2656bad-0bbb-4e15-9276-00ee15703411", 13}, // Adjust your second real ID and date here
};
const int NUM_ACCOUNTS = sizeof(accounts) / sizeof(accounts[0]);

// Time to sleep between checks (12 hours in microseconds)
const uint64_t SLEEP_TIME = 12ULL * 60 * 60 * 1000000;

M5Canvas canvas(&M5.Display);

// --- HELPER FUNCTION: Get the most recent statement date ---
time_t getMostRecentDate(int target_day) {
  time_t now;
  time(&now);
  struct tm * timeinfo = localtime(&now);
  
  // Set to midnight
  timeinfo->tm_hour = 0;
  timeinfo->tm_min = 0;
  timeinfo->tm_sec = 0;
  
  if (timeinfo->tm_mday >= target_day) {
    // Current month's target day
    timeinfo->tm_mday = target_day;
  } else {
    // Previous month's target day
    timeinfo->tm_mday = target_day;
    timeinfo->tm_mon -= 1;
    if (timeinfo->tm_mon < 0) {
      timeinfo->tm_mon = 11;
      timeinfo->tm_year -= 1;
    }
  }
  
  return mktime(timeinfo);
}

// --- HELPER FUNCTION: Setup Time via NTP ---
void setupTime() {
  Serial.println("Setting up NTP (Arizona MST)...");
  // Arizona is MST all year round, no Daylight Saving Time
  configTzTime("MST7", "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  int retry = 0;
  // Wait until time is clearly synced (year > 2020)
  while (retry < 30) {
    getLocalTime(&timeinfo, 1000); 
    if (timeinfo.tm_year > 120) {
      break;
    }
    Serial.print(".");
    delay(500);
    retry++;
  }
  
  if (timeinfo.tm_year <= 120) {
    Serial.println("\nWARNING: Failed to sync NTP time. Time may be incorrect.");
  } else {
    Serial.println("\nTime configured.");
  }
}

// --- HELPER FUNCTION: Format Float Balance ---
String formatBalanceFloat(float val) {
  bool isCredit = false;
  if (val < 0) {
    isCredit = true;
    val = -val;
  }
  
  char buf[32];
  sprintf(buf, "%.2f", val);
  String balStr = String(buf);
  
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
  
  String finalStr = "$" + result + decPart;
  if (isCredit) {
    finalStr = "-" + finalStr; // negative indicates credit
  }
  return finalStr;
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
  // Setup NTP to ensure correct time calculation
  setupTime();

  // Find the earliest start date among all configured accounts
  time_t earliest_start = -1;
  for (int i = 0; i < NUM_ACCOUNTS; i++) {
    time_t acct_start = getMostRecentDate(accounts[i].statement_day);
    if (earliest_start == -1 || acct_start < earliest_start) {
      earliest_start = acct_start;
    }
  }

  // Create request URL with the earliest start-date
  String requestUrl = String(simplefin_access_url) + "&start-date=" + String((unsigned long)earliest_start);

  // Append all account IDs to the query
  for (int i = 0; i < NUM_ACCOUNTS; i++) {
    requestUrl += "&account=" + String(accounts[i].id);
  }

  Serial.println("Starting HTTPS connection to SimpleFIN...");
  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure(); 

  HTTPClient http;
  http.begin(*client, requestUrl.c_str());

  Serial.println("Sending GET Request...");
  int httpCode = http.GET();
  
  Serial.print("HTTP Response Code: ");
  Serial.println(httpCode);

  canvas.fillSprite(TFT_WHITE); 

  // --- DRAW THE TITLE ---
  canvas.setTextSize(4);
  canvas.drawString("Romney & Ryan Finances", 50, 18);

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
    
    // Increased document size to accommodate transactions list
    DynamicJsonDocument doc(16384);
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print("JSON Parsing Failed: ");
      Serial.println(error.c_str());
      
      canvas.setTextSize(4);
      canvas.drawString("Error parsing bank data.", 50, 100);
      canvas.drawString(error.c_str(), 50, 150);
    } else {
      Serial.println("JSON Parsed Successfully.");
      // Loop through accounts and draw cards
      int y_offset = 60; // Start high enough for multiple cards
      
      JsonArray accountsArray = doc["accounts"];
      for (JsonObject accountData : accountsArray) {
        const char* acctId = accountData["id"];
        const char* acctName = accountData["name"];
        const char* balanceStr = accountData["balance"];
        long balanceDate = accountData["balance-date"]; 
        
        // Find statement date for this account
        int statement_day = 1; // fallback
        for (int i = 0; i < NUM_ACCOUNTS; i++) {
          if (String(accounts[i].id) == String(acctId)) {
            statement_day = accounts[i].statement_day;
            break;
          }
        }
        time_t account_start_date = getMostRecentDate(statement_day);

        Serial.print("Account: "); Serial.println(acctName);
        Serial.print("Balance: "); Serial.println(balanceStr);

        // --- CALCULATE BALANCES ---
        float api_balance = atof(balanceStr);
        float total_owed = -api_balance; // assuming negative means debt
        float spend_since_last_statement = 0.0;

        JsonArray transactions = accountData["transactions"];
        for (JsonObject txn : transactions) {
          long posted = txn["posted"];
          if (posted >= account_start_date) {
            float amt = atof(txn["amount"]);
            if (amt < 0) {
              // charge
              spend_since_last_statement += (-amt);
            }
          }
        }

        float statement_balance = total_owed - spend_since_last_statement;
        float this_month_balance = spend_since_last_statement;

        if (statement_balance < 0) {
          // More payments made than statement debt
          this_month_balance += statement_balance;
          statement_balance = 0;
        }

        // Format current time correctly
        time_t rawtime = (time_t)balanceDate;
        struct tm * timeinfo = localtime(&rawtime);
        char timeString[64];
        strftime(timeString, sizeof(timeString), "as of %b %d (%I:%M %p)", timeinfo);

        canvas.drawRoundRect(50, y_offset, 860, 200, 15, TFT_BLACK); 
        
        canvas.setTextSize(3); // Reduced slightly for better fit
        canvas.drawString(acctName, 80, y_offset + 15);

        canvas.setTextSize(7); // Reduced slightly for better fit
        String mainAmountStr = formatBalanceFloat(this_month_balance);
        canvas.drawString(mainAmountStr, 80, y_offset + 55);
        
        // Draw the "as of" time immediately after the balance amount in a smaller font
        int balanceWidth = canvas.textWidth(mainAmountStr);
        canvas.setTextSize(2);
        canvas.drawString(timeString, 80 + balanceWidth + 15, y_offset + 90);

        if (statement_balance > 0.01) {
          canvas.setTextSize(2); 
          float romney_share = statement_balance * 0.55;
          float ryan_share = statement_balance * 0.45;
          
          String statementAmountStr = String(formatBalanceFloat(statement_balance)) + 
              " (Romney: " + formatBalanceFloat(romney_share) + 
              ", Ryan: " + formatBalanceFloat(ryan_share) + ")";
              
          canvas.drawString("Statement Bal: " + statementAmountStr, 80, y_offset + 140);
        }

        // Move to the next card's starting Y position
        y_offset += 230; 
      }
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
