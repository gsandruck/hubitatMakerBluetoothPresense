# ESP32 BLE Presence Tracker for Hubitat

An intelligent, energy-efficient presence detection engine built for the ESP32. It emulates a Bluetooth Low Energy (BLE) Human Interface Device (HID) keyboard to leverage automatic OS-level Bluetooth reconnections (such as Android/iOS) without requiring dedicated smartphone background apps.

When your phone comes within range, it connects to the ESP32. The firmware translates this connection state into home automation presence events, dispatching updates to **Hubitat Elevation** via the Maker API.

## Features

* **No Mobile App Required:** Uses standard BLE HID keyboard profiling, allowing your smartphone's operating system to handle auto-reconnection natively.
* **Power & RF Efficient:** Implements a randomized radio cycle (advertising continuously when searching, and cycling radio sleep states when confirmed present) to conserve RF bandwidth and lower power overhead.
* **Hubitat Integration:** Sends HTTP GET requests directly to Hubitat's local **Maker API** for `arrived` and `departed` state changes.
* **Web-based Config Portal:** Includes an Access Point (`ESP32-Tracker-Setup`) and web-based configuration manager so you never need to hardcode Wi-Fi or API tokens into your source code.
* **Physical Button Support:**
  * **Short Press (< 3s):** Clears BLE bond pairings and forces fresh re-advertising without losing Wi-Fi settings.
  * **Long Press (≥ 3s):** Performs a full factory reset, clearing stored Wi-Fi credentials and Hubitat configuration.
* **Dropout Grace Period:** Configured with a grace period (default: 3 minutes) to prevent false "departed" toggles if BLE drops momentarily.

## Hardware Requirements & Flash Settings

* **ESP32 Development Board** (e.g., ESP32-WROOM-32, ESP32-C3, or ESP32-S3)
* **Reset/Action Button** (Connected to `GPIO 9` and `GND`)
* **Status LED** (Connected to `GPIO 8`)
* Micro-USB or USB-C cable for power and flashing

> [!IMPORTANT]
> **Partition Scheme Required:** Because this project uses `WiFi`, `ESPAsyncWebServer`, and `NimBLE` simultaneously, the compiled binary is too large for default ESP32 partition schemes.
> * **Arduino IDE:** Select **Tools > Partition Scheme > Huge APP (3MB No OTA / 1MB SPIFFS)** before uploading.
> * **PlatformIO:** Add `board_build.partitions = huge_app.csv` to your `platformio.ini` file.

## Prerequisites & Dependencies

Before compiling, ensure you have the following libraries installed in PlatformIO or Arduino IDE:

* [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) (v2.x recommended)
* [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)
* [AsyncTCP](https://github.com/me-no-dev/AsyncTCP) (required for ESP32 web server functionality)

## Step-by-Step Setup Guide

### Step 1: Create the Hubitat Virtual Device

Before configuring the ESP32, you must set up the device in Hubitat so you have its **Device ID** ready:

1. In Hubitat, navigate to **Devices** > **Add Device** > **Virtual**.
2. Name your device (e.g., `Phone BLE Presence`) and select **Virtual Presence** as the Type.
3. Save the device and note down its **Device ID** from the browser URL (e.g., in `.../device/edit/123`, the Device ID is `123`).
4. Open your **Maker API** app settings in Hubitat and ensure:
   * The new Virtual Presence device is checked under allowed devices.
   * You have noted your **Maker API App ID** and **Access Token**.

---

### Step 2: Upload Firmware

1. Open the project in Arduino IDE or PlatformIO.
2. Select your ESP32 board and set the partition scheme to **Huge APP (3MB No OTA)** as described above.
3. Flash the firmware to your board.

---

### Step 3: Device Configuration (Web Portal)

1. On initial boot (or after a factory reset), the ESP32 will launch a Wi-Fi Access Point named **`ESP32-Tracker-Setup`**.
2. Connect your phone or computer to this network.
3. Open a web browser and navigate to `http://192.168.4.1` (or complete the captive portal prompt).
4. Enter your parameters:
   * **Tracker Name:** Room or area identifier (e.g., `LivingRoomTracker`)
   * **Wi-Fi SSID & Password:** Your local home network credentials
   * **Hubitat Local IP:** IP address of your Hubitat hub (e.g., `192.168.1.50`)
   * **Maker API App ID:** The App ID from Step 1
   * **Hubitat Maker API Token:** Your secret access token from Step 1
   * **Maker Device ID:** The Virtual Presence Device ID from Step 1
   * **Pairing PIN:** 6-digit Bluetooth security PIN (Default: `123456`)
5. Click **Save & Reboot**.

---

### Step 4: Pair Your Smartphone

1. Once connected to Wi-Fi, the ESP32 starts advertising as a Bluetooth Keyboard.
2. On your smartphone, open **Bluetooth Settings**.
3. Locate the device under available devices (e.g., `LivingRoomTracker`) and tap to pair.
4. Complete the pairing request. Once paired, your phone will recognize the device as an input device, allowing seamless OS-level auto-reconnections.

---

## Reset Controls

The device uses a single physical button connected to `RESET_BUTTON_PIN` (`GPIO 9`):

| Action | Duration | Result |
| ----- | ----- | ----- |
| **Short Press** | `< 3 seconds` | Clears all saved Bluetooth bonds in flash memory and re-opens advertising. |
| **Long Press** | `≥ 3 seconds` | Erases stored Wi-Fi credentials, Maker API config, and BLE bonds, rebooting into the Config Portal. |

## License

Distributed under the MIT License. See `LICENSE` for more information.
