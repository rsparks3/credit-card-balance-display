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

// 3. Enter your account IDs, their statement dates, and any notes
struct AccountConfig {
  const char* id;
  int statement_day;
  const char* note;
};

AccountConfig accounts[] = {
  {"ACT-XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX", 21, "Pay by the 18th"},
  {"ACT-06ef6ad7-731b-4845-a40f-13c14dd7c353", 13, "Pay by the 10th"},
  {"ACT-b2656bad-0bbb-4e15-9276-00ee15703411", 13, "Pay by the 10th"}, // Adjust your second real ID, date, and note here
};
const int NUM_ACCOUNTS = sizeof(accounts) / sizeof(accounts[0]);

// Fallback sleep duration if time is unavailable (12 hours in microseconds)
const uint64_t FALLBACK_SLEEP_TIME_US = 12ULL * 60 * 60 * 1000000;
const int HTTP_RETRY_DELAY_MS = 30000;
const int MAX_HTTP_RETRY_ATTEMPTS = 5;
const int HTTP_CONNECT_TIMEOUT_MS = 15000;
const int HTTP_READ_TIMEOUT_MS = 20000;
const int WIFI_RECONNECT_TIMEOUT_MS = 15000;

M5Canvas canvas(&M5.Display);

bool ensureWiFiConnected(unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.println("Wi-Fi disconnected. Attempting reconnect...");
  WiFi.disconnect();
  delay(200);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi reconnect successful.");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("Wi-Fi reconnect failed.");
  return false;
}

void drawRetryStatus(int httpCode, int attemptNumber, const String& reason) {
  canvas.fillSprite(TFT_WHITE);
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextSize(4);
  canvas.drawString("HTTP error.", 50, 100);
  canvas.drawString("Code: " + String(httpCode), 50, 150);
  canvas.setTextSize(3);
  canvas.drawString("Retrying in 30 seconds.", 50, 205);
  canvas.setTextSize(2);
  canvas.drawString("Attempt " + String(attemptNumber) + " of " + String(MAX_HTTP_RETRY_ATTEMPTS), 50, 245);
  if (reason.length() > 0) {
    canvas.drawString(reason, 50, 275);
  }
  canvas.pushSprite(0, 0);

  Serial.print("HTTP retry scheduled after code ");
  Serial.print(httpCode);
  Serial.print(". Attempt ");
  Serial.print(attemptNumber);
  Serial.print(" of ");
  Serial.println(MAX_HTTP_RETRY_ATTEMPTS);
  if (reason.length() > 0) {
    Serial.print("Reason: ");
    Serial.println(reason);
  }
}

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

// --- HELPER FUNCTION: Calculate next refresh time (2:00 AM or 2:00 PM) ---
time_t getNextRefreshTime() {
  time_t now;
  time(&now);
  struct tm nextRefresh = *localtime(&now);

  // Snap to the hour first
  nextRefresh.tm_min = 0;
  nextRefresh.tm_sec = 0;

  if (nextRefresh.tm_hour < 2) {
    nextRefresh.tm_hour = 2;
  } else if (nextRefresh.tm_hour < 14) {
    nextRefresh.tm_hour = 14;
  } else {
    nextRefresh.tm_mday += 1;
    nextRefresh.tm_hour = 2;
  }

  return mktime(&nextRefresh);
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
  
  time_t now;
  time(&now);
  struct tm * timeinfo = localtime(&now);

  // If time is valid (year > 2020), schedule next wake at 2:00 AM / 2:00 PM.
  // Also configure ESP32 deep-sleep timer as fallback for USB power scenarios.
  if (timeinfo->tm_year > 120) {
    time_t wakeup_t = getNextRefreshTime();
    uint64_t sleepDurationSeconds = (wakeup_t > now) ? (uint64_t)(wakeup_t - now) : 60ULL;
    uint64_t sleepDurationUs = sleepDurationSeconds * 1000000ULL;

    // ESP32 timer fallback if RTC power-off sleep does not trigger as expected
    esp_sleep_enable_timer_wakeup(sleepDurationUs);

    // Sync the hardware RTC with our NTP time
    m5::rtc_date_t rtc_date;
    rtc_date.year    = timeinfo->tm_year + 1900;
    rtc_date.month   = timeinfo->tm_mon + 1;
    rtc_date.date    = timeinfo->tm_mday;
    rtc_date.weekDay = timeinfo->tm_wday;
    
    m5::rtc_time_t rtc_time;
    rtc_time.hours   = timeinfo->tm_hour;
    rtc_time.minutes = timeinfo->tm_min;
    rtc_time.seconds = timeinfo->tm_sec;
    
    M5.Rtc.setDateTime(&rtc_date, &rtc_time);
    struct tm * wakeupinfo = localtime(&wakeup_t);

    char wakeString[48];
    strftime(wakeString, sizeof(wakeString), "%Y-%m-%d %I:%M %p", wakeupinfo);
    Serial.print("Next scheduled refresh: ");
    Serial.println(wakeString);

    m5::rtc_date_t wake_date;
    wake_date.date    = wakeupinfo->tm_mday;
    wake_date.weekDay = wakeupinfo->tm_wday;
    
    m5::rtc_time_t wake_time;
    wake_time.hours   = wakeupinfo->tm_hour;
    wake_time.minutes = wakeupinfo->tm_min;

    Serial.println("Using absolute RTC alarm for 2:00 AM / 2:00 PM schedule.");
    M5.Power.timerSleep(wake_date, wake_time);
  } else {
    // If no NTP time (e.g., Wi-Fi failed), use fallback interval
    esp_sleep_enable_timer_wakeup(FALLBACK_SLEEP_TIME_US);
    Serial.println("No valid time. Using 12-hour fallback timer.");
    M5.Power.timerSleep((int)(12 * 60 * 60));
  }

  // Fallback in case timerSleep fails to cut power (e.g. plugged into USB)
  Serial.println("Fallback: entering ESP32 deep sleep.");
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
  int httpCode = 0;
  String payload = "";

  for (int attempt = 1; attempt <= MAX_HTTP_RETRY_ATTEMPTS; attempt++) {
    if (!ensureWiFiConnected(WIFI_RECONNECT_TIMEOUT_MS)) {
      httpCode = HTTPC_ERROR_CONNECTION_LOST;
      if (attempt < MAX_HTTP_RETRY_ATTEMPTS) {
        drawRetryStatus(httpCode, attempt, "Wi-Fi reconnect failed");
        delay(HTTP_RETRY_DELAY_MS);
        continue;
      }
      break;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(HTTP_READ_TIMEOUT_MS / 1000);

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_READ_TIMEOUT_MS);

    if (!http.begin(client, requestUrl.c_str())) {
      httpCode = HTTPC_ERROR_CONNECTION_REFUSED;
      if (attempt < MAX_HTTP_RETRY_ATTEMPTS) {
        drawRetryStatus(httpCode, attempt, "http.begin() failed");
        delay(HTTP_RETRY_DELAY_MS);
        continue;
      }
      break;
    }

    Serial.print("Sending GET Request (attempt ");
    Serial.print(attempt);
    Serial.print(" of ");
    Serial.print(MAX_HTTP_RETRY_ATTEMPTS);
    Serial.println(")...");

    httpCode = http.GET();

    Serial.print("HTTP Response Code: ");
    Serial.println(httpCode);

    if (httpCode == HTTP_CODE_OK) {
      payload = http.getString();
      http.end();
      break;
    }

    http.end();

    if (httpCode != HTTP_CODE_OK && attempt < MAX_HTTP_RETRY_ATTEMPTS) {
      String reason = "Wi-Fi status: " + String((int)WiFi.status());
      drawRetryStatus(httpCode, attempt, reason);
      delay(HTTP_RETRY_DELAY_MS);
      continue;
    }

    break;
  }

  canvas.fillSprite(TFT_WHITE); 

  // --- DRAW THE TITLE AND UPDATE TIME ---
  canvas.setTextSize(4);
  canvas.drawString("Romney & Ryan Finances", 50, 18);
  
  // Format and draw current time next to the title
  time_t now;
  time(&now);
  struct tm * timeinfo = localtime(&now);
  char updateTimeString[32];
  strftime(updateTimeString, sizeof(updateTimeString), "Updated %m/%d", timeinfo);
  
  canvas.setTextSize(2);
  canvas.setTextColor(TFT_DARKGREY);
  canvas.drawString(updateTimeString, 620, 25);
  canvas.setTextColor(TFT_BLACK);

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
      // Loop through accounts and draw compact cards (fit 3 on screen)
      const int cardX = 50;
      const int cardW = 860;
      const int cardH = 145;
      const int cardGap = 8;
      int y_offset = 58;
      
      JsonArray accountsArray = doc["accounts"];
      for (JsonObject accountData : accountsArray) {
        const char* acctId = accountData["id"];
        const char* acctName = accountData["name"];
        const char* balanceStr = accountData["balance"];
        long balanceDate = accountData["balance-date"]; 
        
        // Find statement date and note for this account
        int statement_day = 1; // fallback
        String account_note = "";
        for (int i = 0; i < NUM_ACCOUNTS; i++) {
          if (String(accounts[i].id) == String(acctId)) {
            statement_day = accounts[i].statement_day;
            if (accounts[i].note != nullptr) {
              account_note = String(accounts[i].note);
            }
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

        // Format current time and start date correctly
        time_t rawtime = (time_t)balanceDate;
        struct tm * timeinfo = localtime(&rawtime);
        char timeString[64];
        strftime(timeString, sizeof(timeString), "as of %b %d (%I:%M %p)", timeinfo);
        
        struct tm * startinfo = localtime(&account_start_date);
        char startString[32];
        strftime(startString, sizeof(startString), "since %b %d", startinfo);

        canvas.drawRoundRect(cardX, y_offset, cardW, cardH, 12, TFT_BLACK); 
        
        canvas.setTextSize(3);
        canvas.drawString(acctName, 75, y_offset + 8);

        canvas.setTextSize(5);
        String mainAmountStr = formatBalanceFloat(this_month_balance);
        canvas.drawString(mainAmountStr, 75, y_offset + 38);
        
        // Draw the "since" and "as of" time immediately after the balance amount in a smaller font
        int balanceWidth = canvas.textWidth(mainAmountStr);
        canvas.setTextSize(2);
        canvas.setTextColor(TFT_BLACK);
        canvas.drawString(String(startString), 75 + balanceWidth + 14, y_offset + 44);
        canvas.setTextColor(TFT_DARKGREY);
        canvas.drawString(timeString, 75 + balanceWidth + 14, y_offset + 64);
        canvas.setTextColor(TFT_BLACK);

        if (statement_balance > 0.01) {
          canvas.setTextSize(2); 
          float romney_share = statement_balance * 0.55;
          float ryan_share = statement_balance * 0.45;
          
          String statementAmountStr = String(formatBalanceFloat(statement_balance)) + 
              " (Romney: " + formatBalanceFloat(romney_share) + 
              ", Ryan: " + formatBalanceFloat(ryan_share) + ")";
              
          canvas.drawString("Statement Bal: " + statementAmountStr, 75, y_offset + 94);
        } else {
          canvas.setTextSize(2);
          canvas.setTextColor(TFT_DARKGREY);
          canvas.drawString("Statement Bal: $0.00", 75, y_offset + 94);
          canvas.setTextColor(TFT_BLACK);
        }
        
        if (account_note.length() > 0) {
          canvas.setTextColor(TFT_DARKGREY); // Slightly lighter to distinguish as a note
          canvas.setTextSize(2);
          canvas.drawString(account_note, 75, y_offset + 116);
          canvas.setTextColor(TFT_BLACK); // Reset
        }

        // Move to the next card's starting Y position
        y_offset += (cardH + cardGap); 
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
}
