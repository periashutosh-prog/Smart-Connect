#ifndef SMART_CONNECT_H
#define SMART_CONNECT_H

#include <Arduino.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =============================================================================
// PACKET IDS — ESP_NOW
// =============================================================================
#define PKT_ADVERTISE   10
#define PKT_HELLO       20
#define PKT_HELLO_ACK   21
#define PKT_CONN_OK     22
#define PKT_CMD         30
#define PKT_TELEMETRY   40
#define PKT_CLOSE       99

// =============================================================================
// PACKET IDS — SMART CONNECT
// =============================================================================
#define SC_PKT_ADVERTISE    50
#define SC_PKT_CONNECT_REQ  51
#define SC_PKT_PARTIAL_OK   52
#define SC_PKT_PIN_SUBMIT   53
#define SC_PKT_CONNECT_OK   54
#define SC_PKT_CONNECT_FAIL 55

// =============================================================================
// STRUCTS — ESP_NOW
// =============================================================================
struct __attribute__((packed)) AdvertisePacket {
  int  pkt_id;
  int  mode;         // 1 = ESP_NOW
  char mac[18];
  char name[20];
};

struct __attribute__((packed)) HelloPacket {
  int  pkt_id;
  char mac[18];
  char name[20];
};

struct __attribute__((packed)) AckPacket {
  int  pkt_id;       // PKT_HELLO_ACK (21) or PKT_CONN_OK (22)
};

struct __attribute__((packed)) CommandPacket {
  int      pkt_id;
  uint16_t buttons;  // bits 0-4: DPAD1, bits 5-9: DPAD2
  byte     analog1;  // 0-100
  byte     analog2;  // 0-100
};

struct __attribute__((packed)) TelemetryPacket {
  int  pkt_id;
  char data[64];
};

struct __attribute__((packed)) ClosePacket {
  int  pkt_id;
};

// =============================================================================
// STRUCTS — SMART CONNECT
// =============================================================================
struct __attribute__((packed)) ScAdvertisePacket {
  int  pkt_id;
  int  mode;         // 2 = SMART_CONNECT
  char mac[18];
  char name[20];
};

struct __attribute__((packed)) ScConnectReqPacket {
  int  pkt_id;
  char mac[18];
  char name[20];
};

struct __attribute__((packed)) ScPartialOkPacket {
  int  pkt_id;
};

struct __attribute__((packed)) ScPinSubmitPacket {
  int  pkt_id;
  int  pin;          // 4-digit code 0000-9999
};

struct __attribute__((packed)) ScConnectOkPacket {
  int  pkt_id;
};

struct __attribute__((packed)) ScConnectFailPacket {
  int  pkt_id;
};

// =============================================================================
// RECEIVER STATE MACHINE
// =============================================================================
enum ReceiverState {
  RX_IDLE,
  RX_ADVERTISING,
  RX_HANDSHAKE,       // ESP_NOW: received Hello, sent ACK, sending CONN_OK
  RX_PIN_WAIT,        // Smart Connect: showing PIN, waiting for submission
  RX_CONNECTED,
  RX_DISCONNECTED
};

// =============================================================================
// Smart_Connect CLASS
// =============================================================================
class Smart_Connect {
public:
  Smart_Connect();

  void    setup(String name, int oledSda, int oledScl, bool smartConnect, int priority);
  void    run();
  String  readAll();
  bool    isConnected();
  ReceiverState getState();
  void    setTelemetry(const char* data);

private:
  // Config
  String  _name;
  int     _sda, _scl;
  bool    _smartConnect;
  int     _priority;

  // State
  volatile ReceiverState _state;
  volatile bool _running;
  unsigned long _stateTimestamp;
  unsigned long _lastAdvTime;
  unsigned long _lastDisplayTime;
  volatile unsigned long _connectedSince;  // 5s splash anchor, then grace anchor for CMD timeout
  volatile unsigned long _lastCmdTime;     // updated on every received CMD
  volatile bool _firstCmdReceived;     // true once TX has actually started transmitting commands

  // Retransmit timestamps — over-the-air reply packets get retransmitted from the
  // FreeRTOS loop (every 500ms while in the relevant state) so a single dropped
  // packet doesn't kill the handshake. State transition stops the retransmission.
  unsigned long _lastConnOkSent;       // ESP_NOW: while RX_HANDSHAKE
  unsigned long _lastPartialOkSent;    // SC:      while RX_PIN_WAIT
  unsigned long _lastConnectOkSent;    // SC:      while RX_CONNECTED until first CMD arrives

  // Deferred ESP-NOW sends — _onReceive runs in the WiFi driver's callback context,
  // and calling esp_now_send() from inside that callback is unreliable (can stall
  // or silently drop). These flags are set in _onReceive and drained in _loop(),
  // which runs in our own FreeRTOS task.
  volatile bool _ackPending;
  volatile int  _connectResultPending; // 0 = none, else SC_PKT_CONNECT_OK/FAIL
  volatile bool _telemetryPending;
  volatile bool _removePeerPending;
  // Peer registration must ALSO be deferred out of the callback: esp_now_add_peer()
  // called from the recv-callback context can fail to actually register, after which
  // every esp_now_send() to that peer silently returns ESP_ERR_ESPNOW_NOT_FOUND —
  // which is why RX->TX replies (ACK, PARTIAL_OK, CONNECT_OK, telemetry) vanished
  // while TX->RX always worked. Flag it here, do the real add in _loop().
  volatile bool _addPeerPending;

  // Display
  Adafruit_SSD1306* _display;

  // Received command data (guarded by _mutex)
  volatile uint16_t _buttons;
  volatile uint8_t  _analog1;
  volatile uint8_t  _analog2;

  // Telemetry outbound
  char _telemetryBuf[64];

  // Smart Connect PIN
  int _pin;

  // Paired transmitter
  uint8_t _peerMac[6];
  char    _txName[20];
  bool    _peerAdded;

  // Own MAC
  char _macStr[18];

  // FreeRTOS
  TaskHandle_t      _taskHandle;
  SemaphoreHandle_t _mutex;

  // Internal
  static void _taskFunc(void* param);
  void _loop();
  void _advertise();
  void _sendTelemetry();
  void _addPeer(const uint8_t* mac);
  void _removePeer();

  // OLED screens
  void _drawAdvertising();
  void _drawConnecting();
  void _drawPinScreen();
  void _drawAuthenticating();
  void _drawConnected();
  void _drawActive();
  void _drawDisconnected();
  void _drawCentered(const char* text, int y, int size = 1);

  // Packet handler
  void _onReceive(const uint8_t* mac, const uint8_t* data, int len);

  // ESP-NOW callback trampoline
  static Smart_Connect* _instance;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 1)
  static void _recvCB(const esp_now_recv_info_t* info, const uint8_t* data, int len);
#else
  static void _recvCB(const uint8_t* mac, const uint8_t* data, int len);
#endif
};

#endif
