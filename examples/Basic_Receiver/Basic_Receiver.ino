#include <Smart_Connect.h>

Smart_Connect receiver;

void setup() {
  Serial.begin(115200);
  // name, SDA, SCL, smartConnect (true=SC / false=ESP_NOW), FreeRTOS priority
  receiver.setup("RC-Car", 27, 26, true, 1);
  receiver.run();
}

void loop() {
  if (receiver.isConnected()) {
    String input = receiver.readAll();
    // "D1U;D1D;D1L;D1R;D1C:D2U;D2D;D2L;D2R;D2C:A1:A2"
    Serial.println(input);

    receiver.setTelemetry("BAT:87% SPD:42 DIR:FWD");
  }

  delay(20);
}
