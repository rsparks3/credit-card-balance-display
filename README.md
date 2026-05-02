# M5Paper Credit Card Balance Display

This project turns an [M5Stack Paper](https://shop.m5stack.com/products/m5paper-v1-1-commemorative-edition-auth-ink-display) e-ink device into a continuous, low-power display for your credit card balances and current spending. It securely connects to your bank accounts using the SimpleFIN Bridge API.

The device calculates how much you have spent since your last statement and your remaining statement balance, waking up automatically to refresh the display before entering a deep sleep state to preserve battery.

## Hardware Required
To run this project, you will need:
* **[M5Paper v1.1 E-Ink Display](https://shop.m5stack.com/products/m5paper-v1-1-commemorative-edition-auth-ink-display)** - Available from the official M5Stack store.

## Software Dependencies
Install the following libraries in your Arduino IDE:
* `M5Unified` (by M5Stack)
* `ArduinoJson` (by Benoit Blanchon)

## SimpleFIN Setup

To get your balances, you need a SimpleFIN account to link your bank accounts:

1. Go to [SimpleFIN Bridge](https://bridge.simplefin.org/) and create an account.
2. Follow the prompts to link your financial institutions.
3. Once linked, SimpleFIN provides an **Access URL**.
4. To find your specific **Account IDs**:
   * Open your specific Access URL in a standard web browser.
   * You will see a raw JSON output containing your linked accounts.
   * Look for the `accounts` array, and find the `id` field for each account you want to display on the M5Paper (e.g., `ACT-XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX`).

## Code Configuration

Before flashing the code to your M5Paper, you must customize the `cc-balance-display-enhanced.ino` file for your localized setup and preferences.

### 1. Network and API Setup
Find the following lines at the top of the file and enter your details:
```cpp
const char* ssid = "WIFI SSID";
const char* password = "WIFI PASSWORD";
const char* simplefin_access_url = "YOUR SIMPLEFIN URL/simplefin/accounts?";
```
*(Note: Exclude the `account=...` portion of the SimpleFIN URL if it is present; the code appends the specific accounts dynamically.)*

### 2. Account Configuration
Map your Account IDs, the day of the month your statement closes, and any display notes:
```cpp
AccountConfig accounts[] = {
  {"ACT-XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX", 21, "Pay by the 18th"},
  // Add your accounts here
};
```

### 3. Personalizations to Modify
The current code includes some hardcoded logic specific to the original authors. You'll likely want to update these lines:

* **Display Title:** Search for `"Romney & Ryan Finances"` and change it to your preferred title.
* **Statement Split Logic:** The code divides the statement balance (55% / 45%). If you don't share finances this way, search for `romney_share` and `ryan_share` and modify or remove this parsing logic to fit your needs. 
* **Timezone:** Search for `"MST7"`. The project is currently hardcoded for Arizona MST time. Change `"MST7"` to your standard POSIX timezone string (e.g., `"PST8PDT"`, `"EST5EDT"`).

## Usage
Once flashed, the M5Paper will connect to Wi-Fi, pull down the latest transaction data, calculate your spending, and format the E-Ink display. It will then configure its internal RTC (Real-Time Clock) to wake up for the next refresh (scheduled at 2:00 AM/PM by default) and enter deep sleep to save power!