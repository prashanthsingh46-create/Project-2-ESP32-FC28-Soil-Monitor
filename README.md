# Project-2-ESP32-FC28-Soil-Monitor
Soil Wet/ Dry(Conditions) Monitoring using ESP32, FC-28 Soil Sensor and OLED Display

1. Project Overview
This technical document details the implementation of ECOPROSUS ESP32 Node 01, designed for precision soil moisture monitoring and IoT telemetry. The system reads analog and digital signals from an FC-28 soil moisture sensor probe, presents live telemetry on a 0.96″ SSD1306 OLED display, and transmits structured JSON telemetry to a centralized Raspberry Pi MQTT broker every 30 seconds. A closed-loop ACK (acknowledgment) mechanism provides positive hardware indication via dedicated LED patterns.
<img width="355" height="334" alt="image" src="https://github.com/user-attachments/assets/2943f3b2-2efc-4cd0-8137-3b4baa952bac" />

2. System Architecture & Communication Handshake
The node employs a non-blocking asynchronous architecture allowing concurrent sensor sampling, display animation, MQTT socket maintenance, and two-way handshake validation:
• Upstream Telemetry Path: FC-28 Sensor → ESP32 ADC/GPIO → JSON Serialization → Wi-Fi STA → MQTT Broker (Raspberry Pi 172.16.0.132:1883) → Node-RED Flow.
• Closed-Loop Handshake Path: Node-RED verifies incoming packet → Publishes 'ACK' to ecoprosus/esp32-01/ack → ESP32 callback triggers blinkACKLED() (GPIO 23 blinks 3 times).
• Real-Time OLED Display: Renders scrolling corporate title banner, real-time Wi-Fi/MQTT connection states, moisture percentage, soil classification ('dry'/'wet'), and raw ADC integer.
• True UTC Time Architecture: ESP32 internal clock synchronizes to pool.ntp.org with zero GMT offset, ensuring strict ISO-8601 UTC timestamps (YYYY-MM-DDTHH:MM:SSZ) in JSON while dynamically applying IST (+5:30) for local serial monitoring.

3. Complete Hardware Wiring & Pin Diagram 
The table below provides exact pin-to-pin connections for ESP32 Node 01. Ensure common ground between all modules and use series current-limiting resistors for LEDs.
<img width="3774" height="2520" alt="FC-28 Sensor" src="https://github.com/user-attachments/assets/73749acd-4427-4187-aeb8-9c400ad0beac" />

4. Visual LED Signaling & Sequence Logic
The node features explicit visual feedback confirming outgoing telemetry transmission and confirmed broker delivery:
<img width="505" height="156" alt="image" src="https://github.com/user-attachments/assets/fbe4d81a-3581-4f92-b09f-2244b82d1e29" />

<img width="523" height="223" alt="image" src="https://github.com/user-attachments/assets/997cbd5e-6363-4a30-9210-d785455d073e" />
5. MQTT Topics & JSON Telemetry Specification
All data exchanged over the MQTT broker adheres to strict JSON document standards using Benoit Blanchon's ArduinoJson library (v6/v7). The MQTT client buffer is explicitly allocated to 512 bytes to prevent frame truncation.
<img width="509" height="239" alt="image" src="https://github.com/user-attachments/assets/2e4a17be-b88f-486e-b042-0620a2a5ee9f" />

JSON Payload Schema (ecoprosus/esp32-01/soil/moisture)
Example JSON structure published every 30 seconds:
{
  "device_id": "ESP32_Node_01",
  "timestamp": "2026-10-22T07:05:30Z",
  "readings": {
    "soil_moisture_pct": 68.4,
    "soil_moisture_raw": 2420
  },
  "status": {
    "sensor": "wet",
    "reading_valid": true
  }
}
6. FC-28 Calibration & Mathematical Model
The FC-28 sensor's analog output voltage decreases inversely with soil moisture. The onboard ADC generates 12-bit integer readings (0 to 4095). In the firmware, soil moisture percentage is computed with inverse linear interpolation:
Moisture Percentage (%) = [(DRY_VALUE - Analog_Raw) / (DRY_VALUE - WET_VALUE)] × 100.0
<img width="505" height="326" alt="image" src="https://github.com/user-attachments/assets/3543b8bb-1216-46d4-9aa5-d53befddf558" />

7. Software Dependencies & Libraries
The project requires the following standard Arduino/ESP32 core libraries:
8. Commissioning Procedure & Field Verification Checklist
☐  Verify FC-28 VCC is connected to 3.3V (NOT 5V) and AO is wired to GPIO 34.
☐  Verify TX LED (GPIO 18) and ACK LED (GPIO 23) each have 220–330 Ω series resistors to ground.
☐  Verify I²C lines (SDA to GPIO 21, SCL to GPIO 22) and check that OLED address matches 0x3C.
☐  Verify Wi-Fi network 'Ecopro' is active and reachable within ESP32 range.
☐  Confirm Mosquitto MQTT broker on Raspberry Pi (172.16.0.132:1883) is active and accepting connections.
☐  Ensure Node-RED flow subscribes to 'ecoprosus/esp32-01/soil/moisture' and publishes 'ACK' to 'ecoprosus/esp32-01/ack'.
☐  Open Serial Monitor at 115200 baud to observe NTP sync, IP assignment, and periodic 30-second cycles.
☐  Observe TX LED double-blink during publish and ACK LED triple-blink upon Node-RED response.
☐  Confirm OLED screen scrolls 'Ecoprosus' and displays live Wi-Fi, MQTT, Moisture %, and ADC values.

![Uploading image.png…]()





