#include <Smart_Connect.h>

Smart_Connect receiver;

// I2C pins for the SSD1306 — adjust to match your own wiring.
#ifdef ARDUINO_ARCH_ESP32
  #define OLED_SDA 27
  #define OLED_SCL 26
#else
  // NodeMCU 1.0 / ESP-12E: GPIO14 = D5 (silkscreen "SK"/SCLK),
  //                        GPIO12 = D6 (silkscreen "SO"/MISO).
  // (The core's stock I2C default is GPIO4/GPIO5 = D2/D1.)
  #define OLED_SDA 14
  #define OLED_SCL 12
#endif

void setup() {
  Serial.begin(115200);
  // name, SDA, SCL, smartConnect (true=SC / false=ESP_NOW), task priority
  receiver.setup("RC-Car", OLED_SDA, OLED_SCL, true, 1);
  receiver.run();
}

void loop() {
  // On ESP8266 (no FreeRTOS) this drives the receiver's state machine.
  // On ESP32 it's a harmless no-op — the background task handles everything.
  receiver.tick();

  if (receiver.isConnected()) {
    String input = receiver.readAll();
    // "D1U;D1D;D1L;D1R;D1C:D2U;D2D;D2L;D2R;D2C:A1:A2"
    Serial.println(input);

    receiver.setTelemetry("BAT:87% SPD:42 DIR:FWD");
  }

  delay(20);
}
