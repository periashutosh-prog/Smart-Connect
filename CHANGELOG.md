# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [1.5.0] - 2026-07-16

### Added
- **ESP8266 support** (ESP-12E / NodeMCU 1.0 / Wemos D1 mini). The library now
  builds and runs on both `esp32` and `esp8266` architectures.
- **`tick()`** — drives the receiver state machine on ESP8266, which has no
  FreeRTOS in the Arduino core. Call it once per `loop()`. On ESP32 it is a
  no-op, so a single sketch runs unmodified on both platforms.
- ESP8266: `esp_now_set_self_role(ESP_NOW_ROLE_COMBO)` is set automatically.
  Without it the ESP8266 silently refuses to transmit, which would have broken
  all receiver-to-transmitter replies (ACK, PARTIAL_OK, CONNECT_OK, telemetry).

### Changed
- `library.properties`: `architectures=esp32,esp8266`; `sentence`/`paragraph`
  updated to describe both concurrency models.
- Platform-specific APIs are now selected at compile time via
  `ARDUINO_ARCH_ESP32` — no user configuration required, board selection in the
  IDE is enough. This covers the ESP-NOW headers (`esp_now.h` vs `espnow.h`),
  the WiFi header (`WiFi.h` vs `ESP8266WiFi.h`), channel pinning
  (`esp_wifi_set_channel()` vs `wifi_set_channel()`), peer registration, the
  receive-callback signature, and the RNG (`esp_random()` vs `os_random()`).
- Internal mutex access moved behind `_lock()`/`_unlock()` helpers: a real
  FreeRTOS mutex on ESP32, no-ops on ESP8266 (safe there, because the ESP-NOW
  receive callback only fires when the system yields, never mid-instruction).
- I2C bus clock raised to 400kHz for faster OLED frame pushes.
- Active-session display refresh raised to 50Hz (was ~6.7Hz).

### Fixed
- **False disconnects immediately after a burst of valid packets.** Elapsed-time
  checks compared `millis()` snapshots against timestamps written by the
  ESP-NOW receive callback, which runs in a different task context and can land
  a few milliseconds *ahead* of the snapshot. The unsigned subtraction then
  wrapped to ~4.29 billion and always exceeded the timeout. All elapsed-time
  comparisons are now cast to `long` before comparison, and the timestamps
  shared with the callback are declared `volatile`.
- Receiver command-silence watchdog raised to 12s so the transmitter (10s) is
  always the side that notices a dropped link first and surfaces it to the user.

### Display
- Removed the divider line from the active session screen.
- Analog gauges moved up and shortened; numeric values now shown beneath each.
- The paired transmitter's name is shown on the active screen (previously the
  receiver's own name).

## [1.0.0]

### Added
- Initial release: ESP32 ESP-NOW receiver with two pairing modes — plain
  `ESP_NOW` and PIN-authenticated `Smart Connect`.
- SSD1306 128x64 OLED UI: advertising, handshake, PIN entry, connected splash,
  live control panel, and disconnect screens.
- Handshake and confirmation packets retransmitted every ~500ms so a single
  dropped packet does not stall pairing.
- Telemetry replies sent back to the transmitter on every received command.
