#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <atomic>

// ==========================================
// CONSTANTS & CONFIGURATION
// ==========================================
const int RESET_BUTTON_PIN = 9; 
const unsigned long STATIC_DROPOUT_THRESHOLD = 60; // seconds
const unsigned long EVALUATION_INTERVAL = 10000;   // 10 seconds

Preferences preferences;
AsyncWebServer server(80);

// Dynamic runtime configuration variables
String storedTrackerName = "ESP32 Secure Tracker";
String storedMakerId = "";
String storedHubitatIp = "";
String storedHubAppId = "1";
String storedMakerToken = "";
uint32_t storedPin = 123456;

// Thread-safe flags and tracking variables
std::atomic<bool> hasSavedKey{false};
std::atomic<unsigned long> packetCounter{0};

unsigned long lastCheckTime = 0;
unsigned long lastSeenTime = 0;
bool phoneEverSeen = false;
bool currentPresenceState = false;

// ==========================================
// BLE SCAN CALLBACKS
// ==========================================
class MyScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        int currentRssi = advertisedDevice->getRSSI();
        
        if (currentRssi <= -80) {
            return;
        }

        const std::vector<unsigned char>& rawData = advertisedDevice->getPayload();
        if (rawData.empty()) {
            return;
        }

        bool isGoogleTracker = (rawData.size() >= 4 && rawData[0] == 0x03 && rawData[1] == 0x03 && rawData[2] == 0x9f && rawData[3] == 0xfe);
        bool isGooglePresence = (rawData.size() >= 4 && rawData[1] == 0x16 && rawData[2] == 0xf3 && rawData[3] == 0xfe);

        bool isAppleDevice = false;
        for (size_t i = 0; i < rawData.size() - 2; i++) {
            if (rawData[i] == 0xff && rawData[i+1] == 0x4c && rawData[i+2] == 0x00) {
                isAppleDevice = true;
                break;
            }
            if (i == 0 && rawData[0] == 0x4c && rawData[1] == 0x00) {
                isAppleDevice = true;
                break;
            }
        }

        if ((isGoogleTracker || isGooglePresence || isAppleDevice) && NimBLEDevice::getNumBonds() > 0) {
            packetCounter++; 
            lastSeenTime = millis();
            phoneEverSeen = true;
        }
    }
};

// ==========================================
// SERVER CALLBACKS
// ==========================================
class MyServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        Serial.println(F("\n[SECURE_CONNECT] Phone physically bound to GATT channel."));
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        if (connInfo.isEncrypted() || connInfo.isBonded()) {
            preferences.begin("phone-tracker", false);
            preferences.putBool("has_key", true);
            preferences.end();
            Serial.println(F("[BOND_SUCCESS] Keys locked into NVS Flash. Rebooting..."));
            delay(3000);
            ESP.restart();
        } else {
            Serial.println(F("[SECURITY ERROR] Authentication failed or connection not encrypted."));
        }
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
            "<h2>ESP32 Tracker Configuration</h2>"
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
// SYSTEM FUNCTIONS (Dual-Tier Reset Handler)
// ==========================================
void checkPhysicalReset() {
    // Assuming active-LOW button wiring (Button pressed connects pin to GND)
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        delay(50); // Debounce
        if (digitalRead(RESET_BUTTON_PIN) == LOW) {
            unsigned long pressStartTime = millis();
            
            // Wait while the user holds the button to measure hold duration
            while (digitalRead(RESET_BUTTON_PIN) == LOW) {
                delay(100);
                // Optional: Print progress or provide visual feedback here if desired
            }
            
            unsigned long pressDuration = millis() - pressStartTime;

            if (pressDuration >= 3000) {
                // Tier 2: Long Press (>= 3 seconds) -> Full Factory Reset (Wipes Config + Bonds)
                preferences.begin("phone-tracker", false);
                preferences.clear();
                preferences.end();
                NimBLEDevice::deleteAllBonds();
                Serial.println(F("\n[FACTORY RESET] Full memory wipe. Rebooting into Config Portal..."));
                delay(1000);
                ESP.restart();
            } else {
                // Tier 1: Quick Press (< 3 seconds) -> Clear Bluetooth Bonds Only
                preferences.begin("phone-tracker", false);
                preferences.putBool("has_key", false);
                preferences.end();
                NimBLEDevice::deleteAllBonds();
                Serial.println(F("\n[BLE RESET] Security bonds cleared. Rebooting into Pairing Mode..."));
                delay(1000);
                ESP.restart();
            }
        }
    }
}

// Hubitat Maker API Dispatcher
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

void startExtractionMode() {
    Serial.println(F("\n[STATUS] Launching Secure Pairing Environment..."));
    WiFi.mode(WIFI_OFF);
    
    NimBLEDevice::init(storedTrackerName.c_str());
    NimBLEDevice::setOwnAddrType(BLE_ADDR_PUBLIC); 
    
    NimBLEDevice::setSecurityAuth(true, true, true); 
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY); 
    NimBLEDevice::setSecurityPasskey(storedPin);
    NimBLEDevice::setSecurityInitKey(0x03); 
    NimBLEDevice::setSecurityRespKey(0x03);
    
    NimBLEServer *pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    NimBLEService *pService = pServer->createService("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
    NimBLECharacteristic *pSecureChar = pService->createCharacteristic(
        "beb5483e-36e1-4688-b7f5-ea07361b26a8",
        NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN
    );
    pSecureChar->setValue("SECURE_TOKEN");
    pService->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->setName(storedTrackerName.c_str());
    pAdvertising->addServiceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
    pAdvertising->enableScanResponse(true);
    pAdvertising->start();
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000); 
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
    
    Serial.println(F("\n--- TELEMETRY ENGINE ONLINE ---"));

    preferences.begin("phone-tracker", true);
    bool keyStored = preferences.getBool("has_key", false);
    storedTrackerName = preferences.getString("tracker_name", "ESP32 Secure Tracker");
    String wifiSsid = preferences.getString("wifi_ssid", "");
    String wifiPass = preferences.getString("wifi_pass", "");
    storedHubitatIp = preferences.getString("hub_ip", "");
    storedHubAppId = preferences.getString("hub_app_id", "1");
    storedMakerToken = preferences.getString("hub_token", "");
    storedMakerId = preferences.getString("maker_id", "");
    storedPin = preferences.getUInt("pair_pin", 123456);
    preferences.end(); 

    hasSavedKey.store(keyStored);

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

    if (!hasSavedKey.load()) {
        startExtractionMode();
    } else {
        Serial.println(F("\n[STATUS] Launching Continuous Telemetry Engine..."));
        NimBLEDevice::init("");
        NimBLEDevice::setOwnAddrType(BLE_ADDR_PUBLIC);
        
        NimBLEDevice::setSecurityAuth(true, true, true);
        NimBLEDevice::setSecurityInitKey(0x03);
        NimBLEDevice::setSecurityRespKey(0x03);
        
        NimBLEScan* pNimBLEScan = NimBLEDevice::getScan();
        pNimBLEScan->setScanCallbacks(new MyScanCallbacks(), false);
        pNimBLEScan->setActiveScan(true);
        pNimBLEScan->setInterval(97);
        pNimBLEScan->setWindow(97);
        
        pNimBLEScan->start(0, false); 
        Serial.println(F("[RUNNING] Interception array completely open."));
    }
}

void loop() {
    checkPhysicalReset();
    if (!hasSavedKey.load()) { 
        delay(10); 
        return; 
    }

    unsigned long currentMillis = millis();
    if (currentMillis - lastCheckTime >= EVALUATION_INTERVAL || lastCheckTime == 0) { 
        lastCheckTime = currentMillis;

        Serial.println(F("\n========================================="));
        Serial.println(F("       10-SECOND EVALUATION SUMMARY      "));
        Serial.println(F("========================================="));
        
        Serial.printf("[DATABASE] Secure Authorized Bonds: %d\n", NimBLEDevice::getNumBonds());
        Serial.printf("[RADIO HEALTH] Packets caught in last window: %lu\n", packetCounter.load());
        
        bool evaluatedPresenceThisWindow = false;

        if (!phoneEverSeen) {
            Serial.println(F("[TIMING] Last Target Presence: NEVER SEEN SINCE BOOT"));
        } else {
            unsigned long timeDiffSeconds = (currentMillis - lastSeenTime) / 1000;
            unsigned long minutes = timeDiffSeconds / 60;
            unsigned long seconds = timeDiffSeconds % 60;
            
            Serial.printf("[TIMING] Last Target Presence: %lu min %lu sec ago\n", minutes, seconds);
            Serial.printf("[CONFIG] Static Dropout Wall: %lu seconds\n", STATIC_DROPOUT_THRESHOLD);
            
            if (timeDiffSeconds <= STATIC_DROPOUT_THRESHOLD) {
                evaluatedPresenceThisWindow = true;
            }
        }

        if (evaluatedPresenceThisWindow != currentPresenceState) {
            currentPresenceState = evaluatedPresenceThisWindow;
            updateHubitatPresence(currentPresenceState);
        }

        if (currentPresenceState) {
            Serial.println(F(">>> STATE EVALUATION: TARGET PHONE DEBOUNCED AS [PRESENT] <<<"));
        } else {
            Serial.println(F(">>> STATE EVALUATION: TARGET PHONE DEBOUNCED AS [NOT PRESENT] <<<"));
        }

        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan->isScanning() == false) {
            Serial.println(F("[WARNING] BLE Background Task Stopped. Re-initializing engine..."));
            pScan->stop();
            pScan->clearResults();
            pScan->start(0, false);
            Serial.println(F("[RECOVERY] Background scanning channel forced online."));
        }
        
        packetCounter.store(0); 
        Serial.println(F("=========================================\n"));
    }
}
