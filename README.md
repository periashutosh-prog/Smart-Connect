# Smart Connect

An ESP-NOW receiver library for **ESP32 and ESP8266** that pairs with an [AIO Transmitter](https://github.com/periashutosh-prog/AIO-Transmitter) (or any compatible transmitter speaking the same protocol) and drives a 128x64 SSD1306 OLED with a full connection UI — advertising, handshake/PIN authentication, live control display, and telemetry — out of the box.

It supports two pairing modes over the same ESP-NOW radio:

- **ESP_NOW** — direct broadcast advertise + unicast handshake, no authentication.
- **Smart Connect** — PIN-authenticated pairing. The receiver generates a random 4-digit PIN, displays it on its OLED, and only connects once the transmitter submits the matching PIN.

On **ESP32**, everything (advertising, handshake retransmission, watchdog/disconnect handling, telemetry replies, and OLED rendering) runs on its own FreeRTOS task pinned to core 0, so it doesn't compete with your sketch's `loop()`. On **ESP8266** (which has no FreeRTOS in the Arduino core) the same logic runs cooperatively — you call `receiver.tick()` once per `loop()` iteration and everything else is identical. See [Platform differences](#platform-differences).

## Part of the AIO Transmitter / StrawberryOS ecosystem

This library is the receiver-side counterpart to the **[AIO Transmitter](https://github.com/periashutosh-prog/AIO-Transmitter)** — an open-source ESP32-C6 wireless transmitter platform built for robotics, RC vehicles, and experimental wireless control. The AIO Transmitter firmware implements the same packet protocol (ESP_NOW and Smart Connect) directly in its own codebase, so any receiver built with this library will pair with an AIO Transmitter out of the box, with no extra glue code.

The AIO Transmitter is itself a core pillar of **[StrawberryOS](https://github.com/periashutosh-prog/StrawberryOS)**, an open ecosystem of modular ESP32-based devices (transmitters, wearables, cyberdecks) sharing firmware logic, UI design language, and wireless protocols. If you're building a receiver — an RC car, a robot, a sensor node — that needs to talk to an AIO Transmitter, this library is the fastest path there.

Links:
- AIO Transmitter firmware & hardware: https://github.com/periashutosh-prog/AIO-Transmitter
- StrawberryOS ecosystem: https://github.com/periashutosh-prog/StrawberryOS

## Hardware requirements

- An **ESP32** board (tested on ESP32 Dev Module; works on any ESP32/ESP32-S3/ESP32-C variant with ESP-NOW + I2C support), **or** an **ESP8266** board (tested on NodeMCU 1.0 / ESP-12E; also Wemos D1 mini, etc.)
- A 128x64 SSD1306 OLED display wired over I2C
- [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library)
- [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306)

## Installation

1. Download/clone this repository.
2. Place the folder in your Arduino `libraries/` directory (or install via *Sketch > Include Library > Add .ZIP Library*).
3. Restart the Arduino IDE.

## Usage

```cpp
#include <Smart_Connect.h>

Smart_Connect receiver;

void setup() {
  Serial.begin(115200);
  // name, SDA pin, SCL pin, smartConnect (true = PIN-authenticated, false = ESP_NOW), task priority (ESP32 only)
  receiver.setup("RC-Car", 27, 26, false, 1);
  receiver.run();
}

void loop() {
  receiver.tick();  // Required on ESP8266; harmless no-op on ESP32.

  if (receiver.isConnected()) {
    String input = receiver.readAll();
    // Format: "D1U;D1D;D1L;D1R;D1C:D2U;D2D;D2L;D2R;D2C:A1:A2"
    // D1/D2 = two DPADs (Up/Down/Left/Right/Center, 1 = pressed)
    // A1/A2 = two analog channels, 0-100
    Serial.println(input);

    receiver.setTelemetry("BAT:87% SPD:42 DIR:FWD");
  }

  delay(20);
}
```

> Calling `receiver.tick()` in `loop()` works on both platforms — leave it in and the same sketch compiles and runs on ESP32 and ESP8266 unchanged.

Set the 4th argument of `setup()` to `true` to run in **Smart Connect** (PIN-authenticated) mode instead of plain ESP_NOW. The OLED automatically shows the advertising screen, generated PIN, connecting/authenticating animation, a 5-second "Connected!" splash (showing the paired transmitter's name), and then a live control panel mirroring DPAD/analog state.

## API

| Method | Description |
|---|---|
| `setup(name, sda, scl, smartConnect, priority)` | Initializes I2C, OLED, ESP-NOW, and channel pinning (channel 1). Call once in `setup()`. On ESP8266 `priority` is ignored. |
| `run()` | ESP32: starts the background FreeRTOS task (core 0). ESP8266: arms cooperative mode. Call once, after `setup()`. |
| `tick()` | ESP8266: drives the state machine — **call every `loop()`**. ESP32: no-op (the task drives it), safe to leave in cross-platform sketches. |
| `isConnected()` | Returns `true` once paired and actively receiving commands. |
| `readAll()` | Returns the latest DPAD + analog state as a formatted string (see above). Thread-safe. |
| `setTelemetry(const char*)` | Sets the telemetry string sent back to the transmitter on every command. Max 63 chars. |
| `getState()` | Returns the raw `ReceiverState` enum value for advanced use. |

## Platform differences

| | ESP32 | ESP8266 |
|---|---|---|
| Concurrency | Dedicated FreeRTOS task pinned to core 0 | Cooperative — `tick()` in `loop()` |
| `tick()` required? | No (no-op) | **Yes, every loop** |
| ESP-NOW self-role | Handled by IDF | Set to `COMBO` automatically |
| WiFi header | `WiFi.h` | `ESP8266WiFi.h` |
| `priority` arg | Task priority | Ignored |

The public API is otherwise identical, and a sketch that calls `tick()` compiles and runs on both. On ESP8266, keep your `loop()` free of long blocking calls so the receiver's state machine and the ESP-NOW callback get serviced promptly.

## Protocol notes

- Both ends pin to **WiFi channel 1** before starting ESP-NOW — required for unicast handshake reliability.
- Handshake and PIN-confirmation packets are retransmitted every ~500ms until acknowledged, so a single dropped packet won't stall pairing.
- A connected session is monitored by a command-silence watchdog; if the transmitter goes quiet for too long, the receiver tears down and returns to advertising automatically.
- Disconnect and the "no command received" timeouts are deliberately tuned to be more patient than the transmitter's own telemetry watchdog, so the transmitter is always the side that notices and surfaces a lost link first.

## Changelog

See [CHANGELOG.md](CHANGELOG.md). Current version: **1.5.0** (adds ESP8266 support).

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

See the [AIO Transmitter](https://github.com/periashutosh-prog/AIO-Transmitter) / [StrawberryOS](https://github.com/periashutosh-prog/StrawberryOS) repositories for the wider project's licensing (hardware is CERN OHL).
