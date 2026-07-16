#include "Smart_Connect.h"

static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ESP8266 esp_now_send takes non-const pointers; wrap to keep call sites clean
#ifndef ARDUINO_ARCH_ESP32
static inline int sc_espnow_send(const uint8_t* peer, const uint8_t* data, int len) {
  return esp_now_send((uint8_t*)peer, (uint8_t*)data, len);
}
#define esp_now_send(peer, data, len) sc_espnow_send(peer, data, len)
#endif

// Cooperative-safe short sleep: real task yield on ESP32, plain delay() (which
// pumps the ESP8266 SDK background tasks) on ESP8266.
static inline void sc_delay(uint32_t ms) {
#ifdef ARDUINO_ARCH_ESP32
  vTaskDelay(pdMS_TO_TICKS(ms));
#else
  delay(ms);
#endif
}

Smart_Connect* Smart_Connect::_instance = nullptr;

// =============================================================================
// CONSTRUCTOR
// =============================================================================
Smart_Connect::Smart_Connect()
  : _sda(0), _scl(0), _smartConnect(false), _priority(1),
    _state(RX_IDLE), _running(false),
    _stateTimestamp(0), _lastAdvTime(0), _lastDisplayTime(0),
    _connectedSince(0), _lastCmdTime(0), _firstCmdReceived(false),
    _lastConnOkSent(0), _lastPartialOkSent(0), _lastConnectOkSent(0),
    _ackPending(false), _connectResultPending(0),
    _telemetryPending(false), _removePeerPending(false), _addPeerPending(false),
    _display(nullptr),
    _buttons(0), _analog1(0), _analog2(0),
    _pin(0), _peerAdded(false)
#ifdef ARDUINO_ARCH_ESP32
    , _taskHandle(nullptr), _mutex(nullptr)
#endif
{
  memset(_telemetryBuf, 0, sizeof(_telemetryBuf));
  memset(_peerMac, 0, sizeof(_peerMac));
  memset(_txName, 0, sizeof(_txName));
  memset(_macStr, 0, sizeof(_macStr));
}

// =============================================================================
// SETUP
// =============================================================================
void Smart_Connect::setup(String name, int oledSda, int oledScl, bool smartConnect, int priority) {
  _name = name;
  _sda = oledSda;
  _scl = oledScl;
  _smartConnect = smartConnect;
  _priority = priority;
  _instance = this;

#ifdef ARDUINO_ARCH_ESP32
  _mutex = xSemaphoreCreateMutex();
#endif

  // I2C + OLED
  Wire.begin(_sda, _scl);
  Wire.setClock(400000);  // 400kHz for faster display updates
  delay(50);
  _display = new Adafruit_SSD1306(128, 64, &Wire, -1);
  if (!_display->begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("[SC] SSD1306 init failed"));
  }
  _display->clearDisplay();
  _display->display();

  // WiFi off for ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);

  // Pin to channel 1 — without this, the TX and RX may end up on different
  // default channels and unicast packets drop silently while broadcasts (the
  // advertise/scan pair) still propagate. Channel 1 is the ESP-NOW default.
#ifdef ARDUINO_ARCH_ESP32
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
#else
  wifi_set_channel(1);
#endif

  // Own MAC
  String mac = WiFi.macAddress();
  mac.toCharArray(_macStr, sizeof(_macStr));

  // ESP-NOW (esp_now_init returns 0 on success on both cores; ESP_OK is ESP32-only)
  if (esp_now_init() != 0) {
    Serial.println(F("[SC] esp_now_init failed"));
    return;
  }
#ifndef ARDUINO_ARCH_ESP32
  // ESP8266 refuses to send unless a self-role is set. This node both receives
  // CMDs and sends replies (ACK/telemetry), so it must be COMBO.
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
#endif
  esp_now_register_recv_cb(_recvCB);

  // Broadcast peer for advertising
#ifdef ARDUINO_ARCH_ESP32
  esp_now_peer_info_t bcast = {};
  memcpy(bcast.peer_addr, BROADCAST_ADDR, 6);
  bcast.channel = 0;
  bcast.encrypt = false;
  esp_now_add_peer(&bcast);
#else
  esp_now_add_peer((uint8_t*)BROADCAST_ADDR, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
#endif

  // Default telemetry
  strncpy(_telemetryBuf, "NO DATA", sizeof(_telemetryBuf));

  _state = RX_ADVERTISING;
  _stateTimestamp = millis();

  Serial.println(F("[SC] Setup complete"));
}

// =============================================================================
// RUN — ESP32: start the FreeRTOS receiver task (call once from setup()).
//       ESP8266: no FreeRTOS, so just arm cooperative mode — the sketch must
//       call tick() every loop().
// =============================================================================
void Smart_Connect::run() {
  if (_running) return;
  _running = true;
#ifdef ARDUINO_ARCH_ESP32
  xTaskCreatePinnedToCore(_taskFunc, "SC_RX", 8192, this, _priority, &_taskHandle, 0);
  Serial.println(F("[SC] Task started on core 0"));
#else
  Serial.println(F("[SC] Cooperative mode — call tick() every loop()"));
#endif
}

// =============================================================================
// TICK — ESP8266 cooperative driver. On ESP32 the task already runs _loop(),
//        so this is a harmless no-op (safe to leave in shared sketches).
// =============================================================================
void Smart_Connect::tick() {
#ifndef ARDUINO_ARCH_ESP32
  if (_running) _loop();
#endif
}

// =============================================================================
// readAll — thread-safe snapshot of last received command
// =============================================================================
String Smart_Connect::readAll() {
  uint16_t btn;
  uint8_t a1, a2;

  if (_lock(5)) {
    btn = _buttons;
    a1  = _analog1;
    a2  = _analog2;
    _unlock();
  } else {
    btn = 0; a1 = 0; a2 = 0;
  }

  char buf[64];
  snprintf(buf, sizeof(buf), "%d;%d;%d;%d;%d:%d;%d;%d;%d;%d:%d:%d",
    (btn >> 0) & 1, (btn >> 1) & 1, (btn >> 2) & 1, (btn >> 3) & 1, (btn >> 4) & 1,
    (btn >> 5) & 1, (btn >> 6) & 1, (btn >> 7) & 1, (btn >> 8) & 1, (btn >> 9) & 1,
    a1, a2);
  return String(buf);
}

bool Smart_Connect::isConnected() {
  return _state == RX_CONNECTED;
}

ReceiverState Smart_Connect::getState() {
  return _state;
}

void Smart_Connect::setTelemetry(const char* data) {
  if (_lock(5)) {
    strncpy(_telemetryBuf, data, sizeof(_telemetryBuf) - 1);
    _telemetryBuf[sizeof(_telemetryBuf) - 1] = '\0';
    _unlock();
  }
}

// =============================================================================
// FreeRTOS TASK (ESP32 only — ESP8266 drives _loop() cooperatively via tick())
// =============================================================================
#ifdef ARDUINO_ARCH_ESP32
void Smart_Connect::_taskFunc(void* param) {
  Smart_Connect* self = (Smart_Connect*)param;
  for (;;) {
    self->_loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
#endif

// =============================================================================
// MAIN LOOP — state machine + display refresh
// =============================================================================
void Smart_Connect::_loop() {
  unsigned long now = millis();

  // ---- Register the peer FIRST (before any send is drained) ----
  // _peerMac was already copied in _onReceive; do the actual esp_now_add_peer
  // here in task context so the registration reliably takes effect.
  if (_addPeerPending) {
    _addPeerPending = false;
    _addPeer(_peerMac);
  }

  // ---- Drain deferred ESP-NOW sends queued by _onReceive() ----
  // (never call esp_now_send from inside the recv callback itself — see header note)
  if (_ackPending) {
    _ackPending = false;
    AckPacket ack;
    ack.pkt_id = PKT_HELLO_ACK;
    esp_now_send(_peerMac, (const uint8_t*)&ack, sizeof(ack));
  }
  if (_connectResultPending != 0) {
    int result = _connectResultPending;
    _connectResultPending = 0;
    if (result == SC_PKT_CONNECT_OK) {
      ScConnectOkPacket ok;
      ok.pkt_id = SC_PKT_CONNECT_OK;
      esp_now_send(_peerMac, (const uint8_t*)&ok, sizeof(ok));
      _lastConnectOkSent = now;
    } else {
      // FAIL has nowhere to retransmit from (we drop the peer immediately),
      // so burst-send 3 copies with small gaps to make a single drop unlikely.
      ScConnectFailPacket fail;
      fail.pkt_id = SC_PKT_CONNECT_FAIL;
      for (int i = 0; i < 3; i++) {
        esp_now_send(_peerMac, (const uint8_t*)&fail, sizeof(fail));
        sc_delay(30);
      }
    }
  }
  if (_telemetryPending) {
    _telemetryPending = false;
    _sendTelemetry();
  }
  if (_removePeerPending) {
    _removePeerPending = false;
    _removePeer();
  }

  switch (_state) {

    case RX_ADVERTISING: {
      if (now - _lastAdvTime >= 500) {
        _lastAdvTime = now;
        _advertise();
      }
      if (now - _lastDisplayTime >= 200) {
        _lastDisplayTime = now;
        _drawAdvertising();
      }
      break;
    }

    case RX_HANDSHAKE: {
      // Retransmit CONN_OK every 500ms after a 500ms initial settle.
      // _lastConnOkSent is reset to 0 when we enter this state, so the gating
      // condition is purely "are we 500ms past entry AND 500ms past last send".
      if (now - _stateTimestamp >= 500 &&
          (_lastConnOkSent == 0 || now - _lastConnOkSent >= 500)) {
        _lastConnOkSent = now;
        AckPacket pkt;
        pkt.pkt_id = PKT_CONN_OK;
        esp_now_send(_peerMac, (const uint8_t*)&pkt, sizeof(pkt));
      }
      if ((long)(now - _stateTimestamp) > 10000) {
        Serial.println(F("[SC] Handshake timeout"));
        _removePeer();
        _state = RX_ADVERTISING;
        _stateTimestamp = now;
      }
      if (now - _lastDisplayTime >= 200) {
        _lastDisplayTime = now;
        _drawConnecting();
      }
      break;
    }

    case RX_PIN_WAIT: {
      // Retransmit PARTIAL_OK every 500ms while the TX is on the PIN entry
      // screen, in case the first one (queued from _onReceive) dropped.
      if (_lastPartialOkSent == 0 || now - _lastPartialOkSent >= 500) {
        bool first = (_lastPartialOkSent == 0);
        _lastPartialOkSent = now;
        ScPartialOkPacket ok;
        ok.pkt_id = SC_PKT_PARTIAL_OK;
        int r = esp_now_send(_peerMac, (const uint8_t*)&ok, sizeof(ok));
        if (first) Serial.printf("[SC] PARTIAL_OK send result=%d\n", (int)r);
      }
      if ((long)(now - _stateTimestamp) > 90000) {
        Serial.println(F("[SC] PIN wait timeout"));
        _removePeer();
        _state = RX_ADVERTISING;
        _stateTimestamp = now;
      }
      if (now - _lastDisplayTime >= 200) {
        _lastDisplayTime = now;
        _drawPinScreen();
      }
      break;
    }

    case RX_CONNECTED: {
      // SC path: retransmit CONNECT_OK every 500ms until the TX confirms
      // by sending its first CMD. Same drop-tolerance idea as CONN_OK above.
      if (_smartConnect && !_firstCmdReceived &&
          now - _lastConnectOkSent >= 500) {
        _lastConnectOkSent = now;
        ScConnectOkPacket ok;
        ok.pkt_id = SC_PKT_CONNECT_OK;
        esp_now_send(_peerMac, (const uint8_t*)&ok, sizeof(ok));
      }

      bool showSplash = ((long)(now - _connectedSince) < 5000);
      if (showSplash) {
        if (now - _lastDisplayTime >= 500) {
          _lastDisplayTime = now;
          _drawConnected();
        }
      } else {
        if (now - _lastDisplayTime >= 20) {  // 50 Hz active display refresh
          _lastDisplayTime = now;
          _drawActive();
        }
      }

      // CMD timeout — enforced only after the 5s splash, and gated on
      // _firstCmdReceived so we don't false-disconnect while the TX is still
      // sitting on its own CONNECTED splash (5s ESP_NOW / 2.5s SC).
      //
      // `now` was snapshotted once at the top of _loop(); _lastCmdTime gets
      // written by the ESP-NOW recv callback, which runs in a *different*
      // task context and can fire mid-_loop() (e.g. while _drawActive() is
      // pushing a frame over I2C, which alone can take 10-30ms). If a real
      // CMD lands in that window, _lastCmdTime ends up a few ms AHEAD of our
      // stale `now`. `now - _lastCmdTime` then underflows (both are unsigned
      // long) to ~4.29 billion, which is always > the threshold — an instant
      // false "TX silent" disconnect right after a burst of real traffic.
      // Confirmed via diagnostic: now=20546 lastCmd=20554 (lastCmd 8ms in the
      // "future"). Fix: cast the difference to signed so a small negative
      // result from this race reads as a small negative number instead of
      // wrapping — the standard safe pattern for comparing against a
      // concurrently-updated monotonic clock.
      if ((long)(now - _connectedSince) > 5000) {
        bool timedOut;
        if (_firstCmdReceived) {
          // Set deliberately longer than the TX's own 10000ms no-telemetry
          // watchdog so the TX always notices and tears down first (and
          // shows the user the RX-lost screen) — this is just a backstop in
          // case the TX hangs without ever giving up on its own.
          timedOut = ((long)(now - _lastCmdTime) > 12000);
        } else {
          // No CMD ever — give the TX 12s total from connect to start streaming
          timedOut = ((long)(now - _connectedSince) > 12000);
        }
        if (timedOut) {
          Serial.println(F("[SC] TX silent — disconnecting"));
          _state = RX_DISCONNECTED;
          _stateTimestamp = now;
          _lastDisplayTime = 0;
        }
      }
      break;
    }

    case RX_DISCONNECTED: {
      if (now - _stateTimestamp < 2000) {
        if (now - _lastDisplayTime >= 200) {
          _lastDisplayTime = now;
          _drawDisconnected();
        }
      } else {
        _removePeer();
        // Reset command state
        if (_lock(10)) {
          _buttons = 0;
          _analog1 = 0;
          _analog2 = 0;
          _unlock();
        }
        _state = RX_ADVERTISING;
        _stateTimestamp = now;
        Serial.println(F("[SC] Back to advertising"));
      }
      break;
    }

    default:
      break;
  }
}

// =============================================================================
// ADVERTISE — broadcast at 2Hz
// =============================================================================
void Smart_Connect::_advertise() {
  if (_smartConnect) {
    ScAdvertisePacket pkt;
    pkt.pkt_id = SC_PKT_ADVERTISE;
    pkt.mode = 2;
    strncpy(pkt.mac, _macStr, sizeof(pkt.mac));
    _name.toCharArray(pkt.name, sizeof(pkt.name));
    esp_now_send(BROADCAST_ADDR, (const uint8_t*)&pkt, sizeof(pkt));
  } else {
    AdvertisePacket pkt;
    pkt.pkt_id = PKT_ADVERTISE;
    pkt.mode = 1;
    strncpy(pkt.mac, _macStr, sizeof(pkt.mac));
    _name.toCharArray(pkt.name, sizeof(pkt.name));
    esp_now_send(BROADCAST_ADDR, (const uint8_t*)&pkt, sizeof(pkt));
  }
}

// =============================================================================
// SEND TELEMETRY — reply to command with user-set telemetry string
// =============================================================================
void Smart_Connect::_sendTelemetry() {
  TelemetryPacket pkt;
  pkt.pkt_id = PKT_TELEMETRY;
  memset(pkt.data, 0, sizeof(pkt.data));
  if (_lock(5)) {
    strncpy(pkt.data, _telemetryBuf, sizeof(pkt.data) - 1);
    _unlock();
  }
  esp_now_send(_peerMac, (const uint8_t*)&pkt, sizeof(pkt));
}

// =============================================================================
// PEER MANAGEMENT
// =============================================================================
void Smart_Connect::_addPeer(const uint8_t* mac) {
  if (_peerAdded) return;
  memcpy(_peerMac, mac, 6);
#ifdef ARDUINO_ARCH_ESP32
  esp_now_del_peer(mac);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_err_t r = esp_now_add_peer(&peer);
#else
  esp_now_del_peer((uint8_t*)mac);
  int r = esp_now_add_peer((uint8_t*)mac, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
#endif
  Serial.printf("[SC] add_peer %02X:%02X:%02X:%02X:%02X:%02X result=%d\n",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], (int)r);
  _peerAdded = true;
}

void Smart_Connect::_removePeer() {
  if (!_peerAdded) return;
#ifdef ARDUINO_ARCH_ESP32
  esp_now_del_peer(_peerMac);
#else
  esp_now_del_peer((uint8_t*)_peerMac);
#endif
  memset(_peerMac, 0, 6);
  memset(_txName, 0, sizeof(_txName));
  _peerAdded = false;
  _lastConnOkSent = 0;
  _lastPartialOkSent = 0;
  _lastConnectOkSent = 0;
  _firstCmdReceived = false;
}

// =============================================================================
// PACKET HANDLER
// =============================================================================
void Smart_Connect::_onReceive(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < (int)sizeof(int)) return;
  int pkt_id;
  memcpy(&pkt_id, data, sizeof(int));

  // ----- ESP_NOW: HelloPacket(20) -----
  // Also accept a retransmitted Hello while already in RX_HANDSHAKE with the
  // same peer — covers the case where our first HELLO_ACK got lost in the air;
  // the TX will keep retrying Hello up to 3x, so just re-queue the ACK.
  if (pkt_id == PKT_HELLO && !_smartConnect &&
      (_state == RX_ADVERTISING || (_state == RX_HANDSHAKE && memcmp(mac, _peerMac, 6) == 0))) {
    if (len < (int)sizeof(HelloPacket)) return;
    HelloPacket hello;
    memcpy(&hello, data, sizeof(HelloPacket));

    if (_state == RX_ADVERTISING) {
      memcpy(_peerMac, mac, 6);        // set immediately; real registration deferred
      _addPeerPending = true;
      strncpy(_txName, hello.name, sizeof(_txName) - 1);
      _txName[sizeof(_txName) - 1] = '\0';
      _lastConnOkSent = 0;             // arm CONN_OK retransmission
      _state = RX_HANDSHAKE;
      _stateTimestamp = millis();
    }
    _ackPending = true;
    Serial.printf("[SC] Hello from %s, ACK queued\n", _txName);
  }

  // ----- ESP_NOW: TX echo of CONN_OK(22) — handshake complete -----
  else if (pkt_id == PKT_CONN_OK && _state == RX_HANDSHAKE) {
    _state = RX_CONNECTED;
    _stateTimestamp = millis();
    _connectedSince = millis();
    _lastCmdTime = millis();
    _firstCmdReceived = false;
    _lastDisplayTime = 0;
    Serial.println(F("[SC] Connected (ESP_NOW)"));
  }

  // ----- SMART CONNECT: ConnectReq(51) -----
  else if (pkt_id == SC_PKT_CONNECT_REQ && _state == RX_ADVERTISING && _smartConnect) {
    if (len < (int)sizeof(ScConnectReqPacket)) return;
    ScConnectReqPacket req;
    memcpy(&req, data, sizeof(ScConnectReqPacket));

    memcpy(_peerMac, mac, 6);          // set immediately; real registration deferred
    _addPeerPending = true;
    strncpy(_txName, req.name, sizeof(_txName) - 1);
    _txName[sizeof(_txName) - 1] = '\0';

    // Generate random 4-digit PIN via hardware RNG
#ifdef ARDUINO_ARCH_ESP32
    _pin = (int)(esp_random() % 10000);
#else
    _pin = (int)(os_random() % 10000);
#endif

    _state = RX_PIN_WAIT;
    _stateTimestamp = millis();
    _lastPartialOkSent = 0;            // arm PARTIAL_OK retransmission (loop fires immediately)
    _lastDisplayTime = 0;
    Serial.printf("[SC] ConnectReq from %s, PIN=%04d\n", _txName, _pin);
  }

  // ----- SMART CONNECT: PinSubmit(53) -----
  else if (pkt_id == SC_PKT_PIN_SUBMIT && _state == RX_PIN_WAIT) {
    if (len < (int)sizeof(ScPinSubmitPacket)) return;
    ScPinSubmitPacket sub;
    memcpy(&sub, data, sizeof(ScPinSubmitPacket));

    if (sub.pin == _pin) {
      _connectResultPending = SC_PKT_CONNECT_OK;
      _state = RX_CONNECTED;
      _stateTimestamp = millis();
      _connectedSince = millis();
      _lastCmdTime = millis();
      _firstCmdReceived = false;
      _lastConnectOkSent = 0;          // arm CONNECT_OK retransmission
      _lastDisplayTime = 0;
      Serial.println(F("[SC] PIN correct — connected"));
    } else {
      _connectResultPending = SC_PKT_CONNECT_FAIL;
      _removePeerPending = true;
      _state = RX_ADVERTISING;
      _stateTimestamp = millis();
      Serial.printf("[SC] Wrong PIN (got %04d, expected %04d)\n", sub.pin, _pin);
    }
  }

  // ----- COMMAND(30) — active session. Also accepted while in RX_HANDSHAKE
  //       because that means our CONN_OK echo from the TX got lost in the air;
  //       a CMD arriving is itself proof the TX considers the session live, so
  //       we implicitly promote to RX_CONNECTED rather than waiting out the
  //       10s handshake timeout while the TX hammers us with commands. -----
  else if (pkt_id == PKT_CMD && (_state == RX_CONNECTED || (_state == RX_HANDSHAKE && memcmp(mac, _peerMac, 6) == 0))) {
    if (len < (int)sizeof(CommandPacket)) return;
    CommandPacket cmd;
    memcpy(&cmd, data, sizeof(CommandPacket));

    if (_state == RX_HANDSHAKE) {
      _state = RX_CONNECTED;
      _stateTimestamp = millis();
      _connectedSince = millis();
      _firstCmdReceived = false;
      _lastDisplayTime = 0;
      Serial.println(F("[SC] CMD arrived during handshake — promoting to CONNECTED"));
    }

    if (_lock(2)) {
      _buttons = cmd.buttons;
      _analog1 = cmd.analog1;
      _analog2 = cmd.analog2;
      _unlock();
    }

    static uint16_t lastLoggedButtons = 0xFFFF;
    if (cmd.buttons != lastLoggedButtons) {
      lastLoggedButtons = cmd.buttons;
      Serial.printf("[SC] CMD recv buttons=0x%04X a1=%d a2=%d\n", cmd.buttons, cmd.analog1, cmd.analog2);
    }

    _lastCmdTime = millis();
    _firstCmdReceived = true;
    _telemetryPending = true;
  }

  // ----- CLOSE(99) — session teardown -----
  else if (pkt_id == PKT_CLOSE && _state == RX_CONNECTED) {
    _state = RX_DISCONNECTED;
    _stateTimestamp = millis();
    _lastDisplayTime = 0;
    Serial.println(F("[SC] Close received"));
  }
}

// =============================================================================
// ESP-NOW RECEIVE CALLBACK TRAMPOLINE
// =============================================================================
#ifdef ARDUINO_ARCH_ESP32
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 1)
  void Smart_Connect::_recvCB(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (_instance) _instance->_onReceive(info->src_addr, data, len);
  }
  #else
  void Smart_Connect::_recvCB(const uint8_t* mac, const uint8_t* data, int len) {
    if (_instance) _instance->_onReceive(mac, data, len);
  }
  #endif
#else
void Smart_Connect::_recvCB(uint8_t* mac, uint8_t* data, uint8_t len) {
  if (_instance) _instance->_onReceive(mac, data, (int)len);
}
#endif

// =============================================================================
// OLED DRAW HELPERS
// =============================================================================
void Smart_Connect::_drawCentered(const char* text, int y, int size) {
  _display->setTextSize(size);
  int16_t x1, y1; uint16_t w, h;
  _display->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  _display->setCursor((128 - w) / 2 - x1, y);
  _display->print(text);
}

// =============================================================================
// OLED SCREENS
// =============================================================================

// ---- Advertising: "Advertising as" / name / animated dots ----
void Smart_Connect::_drawAdvertising() {
  _display->clearDisplay();
  _display->setTextColor(SSD1306_WHITE);

  _drawCentered("Advertising as", 8);

  _display->setTextSize(1);
  int16_t x1, y1; uint16_t w, h;
  char nameBuf[21];
  _name.toCharArray(nameBuf, sizeof(nameBuf));

  // Name in fake-bold (double print offset by 1px)
  _display->getTextBounds(nameBuf, 0, 0, &x1, &y1, &w, &h);
  int nx = (128 - w) / 2 - x1;
  _display->setCursor(nx, 28);
  _display->print(nameBuf);
  _display->setCursor(nx + 1, 28);
  _display->print(nameBuf);

  // Mode label
  const char* modeLabel = _smartConnect ? "[SMART CONNECT]" : "[ESP_NOW]";
  _drawCentered(modeLabel, 44);

  // Animated dots
  int dots = ((millis() / 500) % 4);
  char dotStr[5] = "";
  for (int i = 0; i < dots; i++) strcat(dotStr, ".");
  _drawCentered(dotStr, 56);

  _display->display();
}

// ---- Connecting (ESP_NOW handshake) ----
void Smart_Connect::_drawConnecting() {
  _display->clearDisplay();
  _display->setTextColor(SSD1306_WHITE);

  _drawCentered("Connecting", 10);
  _drawCentered(_txName, 28);

  int dots = ((millis() / 500) % 4);
  char dotStr[5] = "";
  for (int i = 0; i < dots; i++) strcat(dotStr, ".");
  _drawCentered(dotStr, 42);

  // Progress bar
  unsigned long elapsed = millis() - _stateTimestamp;
  int barW = (int)((elapsed * 88) / 10000);
  if (barW > 88) barW = 88;
  _display->drawRect(20, 52, 88, 8, SSD1306_WHITE);
  if (barW > 0) _display->fillRect(22, 54, barW - 4, 4, SSD1306_WHITE);

  _display->display();
}

// ---- PIN screen (Smart Connect) — show the 4 big digits ----
void Smart_Connect::_drawPinScreen() {
  _display->clearDisplay();
  _display->setTextColor(SSD1306_WHITE);

  _drawCentered("ENTER PIN", 2);
  _display->drawFastHLine(0, 12, 128, SSD1306_WHITE);

  // 4 digit boxes
  const int boxW = 22, boxH = 28, gap = 6;
  const int totalW = boxW * 4 + gap * 3;
  const int startX = (128 - totalW) / 2;
  const int startY = 20;

  char pinStr[5];
  snprintf(pinStr, sizeof(pinStr), "%04d", _pin);

  for (int i = 0; i < 4; i++) {
    int bx = startX + i * (boxW + gap);
    _display->drawRect(bx, startY, boxW, boxH, SSD1306_WHITE);

    char digit[2] = { pinStr[i], '\0' };
    _display->setTextSize(2);
    int16_t x1, y1; uint16_t w, h;
    _display->getTextBounds(digit, 0, 0, &x1, &y1, &w, &h);
    _display->setCursor(bx + (boxW - w) / 2 - x1, startY + (boxH - h) / 2 - y1);
    _display->print(digit);
  }

  _display->setTextSize(1);
  _drawCentered("on transmitter", 54);

  _display->display();
}

// ---- Authenticating (Smart Connect, after PIN submit — not used in current
//      flow since _onReceive transitions directly, but kept for future use) ----
void Smart_Connect::_drawAuthenticating() {
  _display->clearDisplay();
  _display->setTextColor(SSD1306_WHITE);

  _drawCentered("Authenticating", 18);

  int dots = ((millis() / 500) % 4);
  char dotStr[5] = "";
  for (int i = 0; i < dots; i++) strcat(dotStr, ".");
  _drawCentered(dotStr, 34);

  // Ping-pong loader
  const int barX = 18, barW = 92, barH = 8, barY = 50;
  _display->drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  const int chunkW = 26;
  unsigned long t = millis() % 1800;
  float phase = t / 900.0f;
  if (phase > 1.0f) phase = 2.0f - phase;
  int travel = barW - 4 - chunkW;
  int chunkX = barX + 2 + (int)(phase * travel);
  _display->fillRect(chunkX, barY + 2, chunkW, barH - 4, SSD1306_WHITE);

  _display->display();
}

// ---- Connected! with checkmark ----
void Smart_Connect::_drawConnected() {
  _display->clearDisplay();
  _display->setTextColor(SSD1306_WHITE);

  _drawCentered("CONNECTED!", 4);
  _display->drawFastHLine(0, 14, 128, SSD1306_WHITE);

  // Checkmark circle
  _display->fillCircle(64, 38, 14, SSD1306_WHITE);
  for (int i = 0; i < 3; i++) {
    _display->drawLine(58, 38 + i, 62, 42 + i, SSD1306_BLACK);
    _display->drawLine(62, 42 + i, 70, 32 + i, SSD1306_BLACK);
  }

  // TX name below
  _drawCentered(_txName, 56);

  _display->display();
}

// ---- Disconnected (shown briefly before returning to advertising) ----
void Smart_Connect::_drawDisconnected() {
  _display->clearDisplay();
  _display->setTextColor(SSD1306_WHITE);
  _drawCentered("Disconnected!", 20);
  _drawCentered("Re-Advertising...", 38);
  _display->display();
}

// ---- Active session (shown after the 5s "CONNECTED!" splash) — mirrors the
//      transmitter's control panel: shows the connected TX's name (so the
//      user sees who they're actually paired with, not their own receiver
//      name), live DPAD + analog state, no ping/loss figures since those are
//      only meaningful on the TX side ----
void Smart_Connect::_drawActive() {
  _display->clearDisplay();
  _display->setTextColor(SSD1306_WHITE);

  int16_t x1, y1; uint16_t w, h;
  _display->setTextSize(1);
  _display->getTextBounds(_txName, 0, 0, &x1, &y1, &w, &h);
  _display->setCursor((128 - w) / 2 - x1, 0);
  _display->print(_txName);

  _drawCentered("CONNECTED", 10);

  uint16_t btn; uint8_t a1, a2;
  if (_lock(5)) {
    btn = _buttons; a1 = _analog1; a2 = _analog2;
    _unlock();
  } else {
    btn = 0; a1 = 0; a2 = 0;
  }

  // Left gauge (analog1) — moved up to reclaim the space freed by the removed
  // divider line, shortened slightly to leave room for the value readout below.
  int bar1_x = 4;
  _display->drawRoundRect(bar1_x, 20, 8, 32, 4, SSD1306_WHITE);
  int fillL = (a1 * 28) / 100;
  _display->fillRoundRect(bar1_x + 2, 22 + (28 - fillL), 4, fillL, 2, SSD1306_WHITE);
  {
    char buf[5]; snprintf(buf, sizeof(buf), "%d", a1);
    int16_t bx1, by1; uint16_t bw, bh;
    _display->setTextSize(1);
    _display->getTextBounds(buf, 0, 0, &bx1, &by1, &bw, &bh);
    _display->setCursor(bar1_x + 4 - (int)bw / 2 - bx1, 54);
    _display->print(buf);
  }

  // Right gauge (analog2)
  int bar2_x = 116;
  _display->drawRoundRect(bar2_x, 20, 8, 32, 4, SSD1306_WHITE);
  int fillR = (a2 * 28) / 100;
  _display->fillRoundRect(bar2_x + 2, 22 + (28 - fillR), 4, fillR, 2, SSD1306_WHITE);
  {
    char buf[5]; snprintf(buf, sizeof(buf), "%d", a2);
    int16_t bx1, by1; uint16_t bw, bh;
    _display->setTextSize(1);
    _display->getTextBounds(buf, 0, 0, &bx1, &by1, &bw, &bh);
    _display->setCursor(bar2_x + 4 - (int)bw / 2 - bx1, 54);
    _display->print(buf);
  }

  // DPAD1 (left) — bits 0-4: up, down, left, right, center
  int dpad1_x = 24, dpad1_y = 46;
  bool u  = (btn >> 0) & 1, d  = (btn >> 1) & 1, l  = (btn >> 2) & 1, r  = (btn >> 3) & 1, c  = (btn >> 4) & 1;
  if (u) _display->fillRect(dpad1_x + 10, dpad1_y - 10, 8, 8, SSD1306_WHITE); else _display->drawRect(dpad1_x + 10, dpad1_y - 10, 8, 8, SSD1306_WHITE);
  if (d) _display->fillRect(dpad1_x + 10, dpad1_y + 10, 8, 8, SSD1306_WHITE); else _display->drawRect(dpad1_x + 10, dpad1_y + 10, 8, 8, SSD1306_WHITE);
  if (l) _display->fillRect(dpad1_x,      dpad1_y,      8, 8, SSD1306_WHITE); else _display->drawRect(dpad1_x,      dpad1_y,      8, 8, SSD1306_WHITE);
  if (r) _display->fillRect(dpad1_x + 20, dpad1_y,      8, 8, SSD1306_WHITE); else _display->drawRect(dpad1_x + 20, dpad1_y,      8, 8, SSD1306_WHITE);
  if (c) _display->fillRect(dpad1_x + 10, dpad1_y,      8, 8, SSD1306_WHITE); else _display->drawRect(dpad1_x + 10, dpad1_y,      8, 8, SSD1306_WHITE);

  // DPAD2 (right) — bits 5-9: up, down, left, right, center
  int dpad2_x = 76, dpad2_y = 46;
  bool u2 = (btn >> 5) & 1, d2 = (btn >> 6) & 1, l2 = (btn >> 7) & 1, r2 = (btn >> 8) & 1, c2 = (btn >> 9) & 1;
  if (u2) _display->fillRect(dpad2_x + 10, dpad2_y - 10, 8, 8, SSD1306_WHITE); else _display->drawRect(dpad2_x + 10, dpad2_y - 10, 8, 8, SSD1306_WHITE);
  if (d2) _display->fillRect(dpad2_x + 10, dpad2_y + 10, 8, 8, SSD1306_WHITE); else _display->drawRect(dpad2_x + 10, dpad2_y + 10, 8, 8, SSD1306_WHITE);
  if (l2) _display->fillRect(dpad2_x,      dpad2_y,      8, 8, SSD1306_WHITE); else _display->drawRect(dpad2_x,      dpad2_y,      8, 8, SSD1306_WHITE);
  if (r2) _display->fillRect(dpad2_x + 20, dpad2_y,      8, 8, SSD1306_WHITE); else _display->drawRect(dpad2_x + 20, dpad2_y,      8, 8, SSD1306_WHITE);
  if (c2) _display->fillRect(dpad2_x + 10, dpad2_y,      8, 8, SSD1306_WHITE); else _display->drawRect(dpad2_x + 10, dpad2_y,      8, 8, SSD1306_WHITE);

  _display->display();
}
