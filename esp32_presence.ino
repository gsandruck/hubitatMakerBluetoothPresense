#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <atomic>
#include <nvs_flash.h>

// ==========================================
// CONFIG
// ==========================================
const int RESET_BUTTON_PIN = 9;                 // active-LOW button to GND
const int LED_PIN = 8;
const unsigned long DROPOUT_GRACE_MS = 180000;    // keep reporting "present" for this long after a disconnect
const unsigned long EVALUATION_INTERVAL = 10000; // how often we log/evaluate state

Preferences preferences;
AsyncWebServer server(80);

String storedTrackerName = "ESP32 Presence Tracker";
String storedMakerId = "";
String storedHubitatIp = "";
String storedHubAppId = "1";
String storedMakerToken = "";
uint32_t storedPin = 123456;

NimBLEHIDDevice *hid = nullptr;
NimBLEServer *pServer = nullptr;
std::atomic<bool> bleConnected{false};
std::atomic<uint16_t> activeConnHandle{0};

unsigned long lastConnectedMillis = 0;
bool everConnectedSinceBoot = false;
bool currentPresenceState = false;
unsigned long lastEvalTime = 0;

unsigned long lastBeaconCycleTime = 0;
const unsigned long BEACON_ACTIVE_WINDOW_MS = 5000; // stay awake/advertising for 5 seconds
unsigned long beaconSleepWindowMs = 60000;
bool beaconIsActive = true;
bool lastCheckSucceeded = false;

// Standard USB HID "boot keyboard" report descriptor (report ID 1).
// This is boilerplate straight out of the USB HID spec's keyboard example -
// we never actually send key reports, we just need a valid HID Keyboard
// service so Android's OS-level Bluetooth stack recognizes and auto-reconnects
// to this device the same way it would a real Bluetooth keyboard.
static const uint8_t KEYBOARD_REPORT_MAP[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report Id (1)
    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0xE0,       //   Usage Minimum (224)
    0x29, 0xE7,       //   Usage Maximum (231)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data, Variable, Absolute) - modifier byte
    0x95, 0x01,       //   Report Count (1)
    0x75, 0x08,       //   Report Size (8)
    0x81, 0x01,       //   Input (Constant) - reserved byte
    0x95, 0x06,       //   Report Count (6)
    0x75, 0x08,       //   Report Size (8)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x65,       //   Logical Maximum (101)
    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0x00,       //   Usage Minimum (0)
    0x29, 0x65,       //   Usage Maximum (101)
    0x81, 0x00,       //   Input (Data, Array) - key array (6 bytes)
    0xC0              // End Collection
};

// ==========================================
// SERVER CALLBACKS (this is our presence signal)
// ==========================================
class PresenceServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
        bleConnected.store(true);
        activeConnHandle.store(connInfo.getConnHandle());
        Serial.println(F("[BLE] Phone connected."));
    }

    void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
        bleConnected.store(false);
        Serial.printf("[BLE] Phone disconnected (reason=%d).\n", reason);
        //pServer->startAdvertising();
    }
};

// ==========================================
// CONFIG MODE (Access Point & Web Server)
// ==========================================
void enterConfigMode() {
    Serial.println(F("\n[CONFIG MODE] Starting Access Point..."));
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32-Tracker-Setup");

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/html",
            "<html><head><style>"
            "body{font-family:Arial;margin:30px;background:#f4f4f9;}"
            "h2{color:#333;}"
            "form{background:#fff;padding:20px;border-radius:8px;box-shadow:0 0 10px rgba(0,0,0,0.1);max-width:400px;}"
            "input{width:100%;padding:8px;margin:8px 0 16px 0;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;}"
            "input[type=submit]{background:#007BFF;color:white;border:none;cursor:pointer;font-size:16px;}"
            "input[type=submit]:hover{background:#0056b3;}"
            "</style></head><body>"
            "<h2>ESP32 Presence Tracker Configuration</h2>"
            "<form action='/save' method='POST'>"
            "<label>Tracker Unique Name / Room:</label><input type='text' name='tracker_name' value='LivingRoomTracker' required>"
            "<label>Wi-Fi SSID:</label><input type='text' name='ssid' required>"
            "<label>Wi-Fi Password:</label><input type='password' name='password'>"
            "<label>Hubitat Local IP:</label><input type='text' name='hub_ip' placeholder='192.168.1.50' required>"
            "<label>Maker API App ID (e.g. 1):</label><input type='text' name='hub_app_id' value='1' required>"
            "<label>Hubitat Maker API Token:</label><input type='text' name='hub_token' required>"
            "<label>Maker Device ID (Virtual Presence ID):</label><input type='text' name='maker_id' required>"
            "<label>Pairing PIN (6 digits):</label><input type='number' name='pin' value='123456' required>"
            "<input type='submit' value='Save & Reboot'>"
            "</form></body></html>");
    });

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        String trackerName = request->getParam("tracker_name", true)->value();
        String ssid = request->getParam("ssid", true)->value();
        String pass = request->getParam("password", true)->value();
        String hubIp = request->getParam("hub_ip", true)->value();
        String hubAppId = request->getParam("hub_app_id", true)->value();
        String hubToken = request->getParam("hub_token", true)->value();
        String makerId = request->getParam("maker_id", true)->value();
        String pinStr = request->getParam("pin", true)->value();

        preferences.begin("phone-tracker", false);
        preferences.putString("tracker_name", trackerName);
        preferences.putString("wifi_ssid", ssid);
        preferences.putString("wifi_pass", pass);
        preferences.putString("hub_ip", hubIp);
        preferences.putString("hub_app_id", hubAppId);
        preferences.putString("hub_token", hubToken);
        preferences.putString("maker_id", makerId);
        preferences.putUInt("pair_pin", pinStr.toInt());
        preferences.end();

        request->send(200, "text/plain", "Configuration Saved Successfully! Rebooting device...");
        delay(1500);
        ESP.restart();
    });

    server.begin();
    while (true) { delay(100); }
}

// ==========================================
// RESET BUTTON (tiered: short press = clear bonds, long press = full wipe)
// ==========================================
// NimBLEDevice::deleteAllBonds() has a known upstream bug where it doesn't
// reliably persist the deletion to NVS (it calls ble_store_clear(), which
// can leave stale bond records behind). Deleting bonds one at a time by
// address routes through ble_gap_unpair() instead, which flushes correctly.
void clearAllBleBonds() {
    if (bleConnected.load() && pServer != nullptr) {
        Serial.println(F("[BLE] Phone is currently connected; disconnecting before unpairing..."));
        pServer->disconnect(activeConnHandle.load());
        delay(500); // give the link time to actually drop before we try to unpair
    }
    NimBLEDevice::deleteAllBonds();
    // 2. Direct NVS Namespace Wipe (This catches the stubborn "ghost" bond)
    nvs_handle_t nvs_handle;
    // NimBLE typically stores security/bond data in these namespaces:
    if (nvs_open("nimble_bond", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_erase_all(nvs_handle);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        Serial.println(F("[NVS] Cleared 'nimble_bond' namespace from flash."));
    }
    
    // Some versions use "ble_config" or generic storage namespaces
    if (nvs_open("ble_config", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_erase_all(nvs_handle);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        Serial.println(F("[NVS] Cleared 'ble_config' namespace from flash."));
    }

    
   // 3. Force-start advertising so it's active before reboot or if we stay alive
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    if (pAdvertising) {
        pAdvertising->start();
    }
    delay(500);
    Serial.printf("[BLE] Bonds remaining after clear: %d\n", NimBLEDevice::getNumBonds());
}

void checkPhysicalReset() {
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        delay(50); // debounce
        if (digitalRead(RESET_BUTTON_PIN) == LOW) {
            unsigned long pressStartTime = millis();
            while (digitalRead(RESET_BUTTON_PIN) == LOW) {
                delay(100);
            }
            unsigned long pressDuration = millis() - pressStartTime;

            if (pressDuration >= 3000) {
                preferences.begin("phone-tracker", false);
                preferences.clear();
                preferences.end();
                clearAllBleBonds();
                Serial.println(F("\n[FACTORY RESET] Wiping config + bonds. Rebooting into Config Portal..."));
                delay(1500);
                ESP.restart();
            } else {
                clearAllBleBonds();
                Serial.println(F("\n[BLE RESET] Bonds cleared. Re-pair your phone as a Bluetooth keyboard device."));
                delay(1500);
                ESP.restart();
            }
        }
    }
}

// ==========================================
// Hubitat Maker API Dispatcher
// ==========================================
void updateHubitatPresence(bool isPresent) {
    if (storedHubitatIp.length() == 0 || storedMakerId.length() == 0) return;

    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String stateCommand = isPresent ? "arrived" : "departed";
        String url = "http://" + storedHubitatIp + "/apps/api/" + storedHubAppId + "/devices/" + storedMakerId + "/" + stateCommand + "?access_token=" + storedMakerToken;

        http.begin(url);
        int httpResponseCode = http.GET();

        if (httpResponseCode > 0) {
            Serial.printf("[HUBITAT] Dispatch successful. State set to: %s (Code: %d)\n", isPresent ? "PRESENT" : "NOT PRESENT", httpResponseCode);
        } else {
            Serial.printf("[HUBITAT ERROR] Dispatch failed. Error: %s\n", http.errorToString(httpResponseCode).c_str());
        }
        http.end();
    } else {
        Serial.println(F("[WARNING] Wi-Fi disconnected. Unable to send Hubitat update."));
    }
}

// ==========================================
// BLE HID keyboard setup (hand-rolled, NimBLE-Arduino 2.x only, no
// third-party keyboard library, no unused Bluedroid/1.x compatibility paths)
// ==========================================
void setupPresenceBeacon() {
    String bleName = storedTrackerName;
    if (bleName.length() > 15) bleName = bleName.substring(0, 15); // keep advertising packet small/reliable

    NimBLEDevice::init(bleName.c_str());
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    //NimBLEDevice::setSecurityPasskey(storedPin);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new PresenceServerCallbacks());

    hid = new NimBLEHIDDevice(pServer);
    hid->setManufacturer("DIY Presence");
    hid->setPnp(0x02, 0x05ac, 0x820a, 0x0210);
    hid->setHidInfo(0x00, 0x01);
    hid->setReportMap((uint8_t *)KEYBOARD_REPORT_MAP, sizeof(KEYBOARD_REPORT_MAP));
    hid->getInputReport(1); // creates the report-ID-1 characteristic; we never write to it
    hid->startServices();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->setAppearance(HID_KEYBOARD);
    pAdvertising->addServiceUUID(hid->getHidService()->getUUID());
    pAdvertising->enableScanResponse(true);
   
    pAdvertising->setName(bleName.c_str()); // Explicitly inject name into advertisement payload

    pAdvertising->start();

    Serial.printf("[BLE] Advertising as HID keyboard, name='%s'. Pair from your phone's Bluetooth settings.\n", bleName.c_str());
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    Serial.println(F("\n--- PRESENCE ENGINE ONLINE ---"));

    preferences.begin("phone-tracker", true);
    storedTrackerName = preferences.getString("tracker_name", storedTrackerName);
    String wifiSsid = preferences.getString("wifi_ssid", "");
    String wifiPass = preferences.getString("wifi_pass", "");
    storedHubitatIp = preferences.getString("hub_ip", "");
    storedHubAppId = preferences.getString("hub_app_id", "1");
    storedMakerToken = preferences.getString("hub_token", "");
    storedMakerId = preferences.getString("maker_id", "");
    storedPin = preferences.getUInt("pair_pin", 123456);
    preferences.end();

    if (wifiSsid.length() == 0 || storedHubitatIp.length() == 0 || storedMakerId.length() == 0) {
        enterConfigMode();
    }

    Serial.printf("[WIFI] Connecting to SSID: %s\n", wifiSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] Connected! IP Address: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println(F("\n[WIFI ERROR] Failed to connect within timeout. Proceeding anyway, but network calls may fail."));
    }

    setupPresenceBeacon();
}

void loop() {
    checkPhysicalReset();
    unsigned long now = millis();
    bool hasBonds = (NimBLEDevice::getNumBonds() > 0);
    bool connectedNow = bleConnected.load();

    if (connectedNow) {
        lastConnectedMillis = now;
        everConnectedSinceBoot = true;
        lastCheckSucceeded = true; // We successfully saw the phone!
    }

    // Determine current presence based on connection or active grace window (for Hubitat)
    bool present = connectedNow ||
                   (everConnectedSinceBoot && (now - lastConnectedMillis) <= DROPOUT_GRACE_MS);

    digitalWrite(LED_PIN, present ? HIGH : LOW);

    // ==========================================
    // RADIO CYCLE MANAGEMENT
    // ==========================================
    // If we have no bonds, OR if our last periodic check failed to find the phone,
    // we drop out of the sleep cycle and stay ON continuously!
    if (!hasBonds || !lastCheckSucceeded) {
        if (!beaconIsActive) {
            beaconIsActive = true;
            NimBLEDevice::getAdvertising()->start();
            Serial.println(F("[BLE] Check missed. Radio active / advertising continuously for instant re-detection."));
        }
        lastBeaconCycleTime = now; 
    } 
    else {
        // ==========================================
        // PRESENT & CHECK PASSED: Run power-saving cycle (5s ON, 55s OFF)
        // ==========================================
        randomSeed(esp_random());
        if (connectedNow && pServer != nullptr) {
            pServer->disconnect(activeConnHandle.load());
        } 

        if (beaconIsActive && (now - lastBeaconCycleTime >= BEACON_ACTIVE_WINDOW_MS)) {
            // Active window just closed. Did we successfully connect during this window?
            // If `connectedNow` was never triggered during this 5-second window, mark the check as failed!
            // (We check if a connection happened recently or check connection state)
            
            beaconIsActive = false;
            lastBeaconCycleTime = now;
            NimBLEDevice::getAdvertising()->stop();
            beaconSleepWindowMs = random(60000, 120001);
            Serial.println(F("[BLE RADIO] Sleeping radio to save power & RF bandwidth."));
            
            // If we didn't connect at all during that 5-second window, flag it as a missed check for the next cycle
            if (!connectedNow && (now - lastConnectedMillis > BEACON_ACTIVE_WINDOW_MS)) {
                lastCheckSucceeded = false;
            }
        } 
        //else if (!beaconIsActive && (now - lastBeaconCycleTime >= BEACON_SLEEP_WINDOW_MS)) {
        else if (!beaconIsActive && (now - lastBeaconCycleTime >= beaconSleepWindowMs)) {    
            beaconIsActive = true;
            lastBeaconCycleTime = now;
            NimBLEDevice::getAdvertising()->start();
            Serial.println(F("[BLE RADIO] Waking up radio for periodic presence check..."));
        }
    }

    // ==========================================
    // EVALUATION INTERVAL & HUBITAT DISPATCH
    // ==========================================
    // Hubitat still uses the generous 90-second grace window so your smart home 
    // automation doesn't flap if your phone misses a poll, but your radio gets instant vigilance!
    if (now - lastEvalTime >= EVALUATION_INTERVAL || lastEvalTime == 0) {
        lastEvalTime = now;

        if (present != currentPresenceState) {
            currentPresenceState = present;
            Serial.printf("[PRESENCE CHANGED] State: %s\n", present ? "PRESENT" : "NOT PRESENT");
            updateHubitatPresence(currentPresenceState);
        }
    }
}
