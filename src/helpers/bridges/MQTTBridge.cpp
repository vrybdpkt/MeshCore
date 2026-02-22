#include "MQTTBridge.h"

#ifdef WITH_MQTT_BRIDGE

#ifndef WITH_MQTT_BRIDGE_SSID
  #define WITH_MQTT_BRIDGE_SSID ""      // override at runtime: set bridge.mqtt.ssid <value>
#endif
#ifndef WITH_MQTT_BRIDGE_WIFI_PASS
  #define WITH_MQTT_BRIDGE_WIFI_PASS "" // override at runtime: set bridge.mqtt.wifi_pass <value>
#endif
#ifndef WITH_MQTT_BRIDGE_SERVER
  #define WITH_MQTT_BRIDGE_SERVER ""    // override at runtime: set bridge.mqtt.server <value>
#endif

MQTTBridge* MQTTBridge::_instance = nullptr;

MQTTBridge::MQTTBridge(NodePrefs* prefs, mesh::PacketManager* mgr, mesh::RTCClock* rtc)
    : BridgeBase(prefs, mgr, rtc), _mqttClient(_wifiClient), _lastReconnectAttempt(0) {
  _instance = this;
}

// ── WiFi ──────────────────────────────────────────────────────────────────────

bool MQTTBridge::connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  const char* ssid = _prefs->mqtt_ssid[0] ? _prefs->mqtt_ssid : WITH_MQTT_BRIDGE_SSID;
  const char* pass = _prefs->mqtt_wifi_pass[0] ? _prefs->mqtt_wifi_pass : WITH_MQTT_BRIDGE_WIFI_PASS;

  if (ssid[0] == '\0') {
    BRIDGE_DEBUG_PRINTLN("No WiFi SSID configured — set bridge.mqtt.ssid\n");
    return false;
  }

  BRIDGE_DEBUG_PRINTLN("WiFi connecting to %s...\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    BRIDGE_DEBUG_PRINTLN("WiFi connected\n");
    return true;
  }

  int wl = WiFi.status();
  const char* reason =
    (wl == 1) ? "SSID not found" :
    (wl == 4) ? "wrong password" : "unknown";
  BRIDGE_DEBUG_PRINTLN("WiFi connection failed, status=%d (%s)\n", wl, reason);
  return false;
}

// ── MQTT ──────────────────────────────────────────────────────────────────────

bool MQTTBridge::connectMQTT() {
  if (_mqttClient.connected()) return true;

  const char* server = _prefs->mqtt_server[0] ? _prefs->mqtt_server : WITH_MQTT_BRIDGE_SERVER;
  uint16_t    port   = _prefs->mqtt_port ? _prefs->mqtt_port : (uint16_t)WITH_MQTT_BRIDGE_PORT;
  const char* topic  = _prefs->mqtt_topic[0] ? _prefs->mqtt_topic : WITH_MQTT_BRIDGE_TOPIC;
  const char* user   = _prefs->mqtt_user[0]  ? _prefs->mqtt_user  : WITH_MQTT_BRIDGE_USER;
  const char* pass   = _prefs->mqtt_pass[0]  ? _prefs->mqtt_pass  : WITH_MQTT_BRIDGE_PASS;

  if (server[0] == '\0') {
    BRIDGE_DEBUG_PRINTLN("No MQTT server configured — set bridge.mqtt.server\n");
    return false;
  }

  BRIDGE_DEBUG_PRINTLN("MQTT connecting to %s:%d...\n", server, (int)port);
  _mqttClient.setServer(server, port);

  // Derive a stable client ID from the node name so multiple bridges on the
  // same broker don't collide.
  char clientId[40];
  snprintf(clientId, sizeof(clientId), "mc-bridge-%.32s", _prefs->node_name);

  bool ok = (user[0] != '\0')
    ? _mqttClient.connect(clientId, user, pass)
    : _mqttClient.connect(clientId);

  if (ok) {
    BRIDGE_DEBUG_PRINTLN("MQTT connected\n");
    _mqttClient.subscribe(topic);
    BRIDGE_DEBUG_PRINTLN("Subscribed to %s\n", topic);
    _stats.reconnects++;
    return true;
  }

  BRIDGE_DEBUG_PRINTLN("MQTT connection failed, rc=%d\n", _mqttClient.state());
  return false;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void MQTTBridge::begin() {
  BRIDGE_DEBUG_PRINTLN("WiFi starting...\n");

#if WITH_MQTT_BRIDGE_TLS
  _wifiClient.setInsecure(); // TLS without certificate verification
#endif

  _mqttClient.setCallback(mqttCallback);
  // Buffer must fit the full serialised packet plus PubSubClient's own header
  _mqttClient.setBufferSize(MAX_MQTT_PAYLOAD + 64);
  _mqttClient.setKeepAlive(60);

  _mqtt_running = false;

  if (!connectWiFi()) {
    BRIDGE_DEBUG_PRINTLN("WiFi unavailable\n");
    return;
  }
  _initialized = true;

  if (_prefs->mqtt_autostart) {
    _mqtt_running = true;
    connectMQTT(); // best-effort; loop() will retry
  }
}

void MQTTBridge::end() {
  // Stop MQTT only — WiFi stays up so the device keeps LAN connectivity.
  stopMQTT();
}

void MQTTBridge::endAll() {
  BRIDGE_DEBUG_PRINTLN("Stopping WiFi+MQTT...\n");
  stopMQTT();
  _initialized = false;
  WiFi.disconnect(true);
}

void MQTTBridge::startMQTT() {
  if (!_initialized) {
    // WiFi was never brought up — do full init first.
#if WITH_MQTT_BRIDGE_TLS
    _wifiClient.setInsecure();
#endif
    _mqttClient.setCallback(mqttCallback);
    _mqttClient.setBufferSize(MAX_MQTT_PAYLOAD + 64);
    _mqttClient.setKeepAlive(60);

    if (!connectWiFi()) {
      BRIDGE_DEBUG_PRINTLN("WiFi unavailable — can't start MQTT\n");
      return;
    }
    _initialized = true;
  }
  BRIDGE_DEBUG_PRINTLN("MQTT bridge starting...\n");
  _mqtt_running = true;
  connectMQTT(); // best-effort; loop() will retry
}

void MQTTBridge::stopMQTT() {
  BRIDGE_DEBUG_PRINTLN("MQTT bridge stopping...\n");
  _mqtt_running = false;
  _mqttClient.disconnect();
}

void MQTTBridge::loop() {
  if (!_initialized) return;

  // Keep WiFi alive regardless of whether the MQTT bridge is active.
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    return;
  }

  if (!_mqtt_running) return;

  // Reconnect MQTT with 5-second back-off
  if (!_mqttClient.connected()) {
    unsigned long now = millis();
    if (now - _lastReconnectAttempt > 5000UL) {
      _lastReconnectAttempt = now;
      connectMQTT();
    }
    return;
  }

  _mqttClient.loop();
}

// ── Bridge send / receive ─────────────────────────────────────────────────────

/**
 * Returns true if this packet should be forwarded over MQTT to remote sites.
 *
 * Excluded:
 *   PAYLOAD_TYPE_ADVERT with path_len==0 — zero-hop local advertisements.
 *                        They are only relevant to direct RF neighbours and
 *                        must not propagate beyond the local segment.
 *   PAYLOAD_TYPE_TRACE — diagnostic traceroute packets, local only.
 */
static bool shouldBridgePacket(const mesh::Packet* pkt) {
  uint8_t type = pkt->getPayloadType();
  if (type == PAYLOAD_TYPE_TRACE) return false;
  if (pkt->path_len == 0 && type == PAYLOAD_TYPE_ADVERT) return false;
  return true;
}

void MQTTBridge::sendPacket(mesh::Packet* packet) {
  if (!_mqtt_running || !_mqttClient.connected() || !packet) return;
  if (!shouldBridgePacket(packet)) {
    _stats.tx_filtered++;
    return;
  }

  // _seen_packets.hasSeen() both checks AND marks the hash:
  //   - If the packet came in via MQTT we already marked it then, so this
  //     returns true and we don't re-publish it (prevents echo loops).
  //   - If it's a fresh LoRa packet we mark it now; the broker echo will be
  //     caught when it arrives back via the subscription.
  if (!_seen_packets.hasSeen(packet)) {
    uint16_t len = packet->writeTo(_tx_buffer);
    if (len > 0 && len <= (uint16_t)MAX_MQTT_PAYLOAD) {
      const char* topic = _prefs->mqtt_topic[0] ? _prefs->mqtt_topic : WITH_MQTT_BRIDGE_TOPIC;
      if (_mqttClient.publish(topic, _tx_buffer, len, /*retain=*/false)) {
        _stats.tx_packets++;
        BRIDGE_DEBUG_PRINTLN("TX %d bytes\n", (int)len);
      }
    }
  }
}

void MQTTBridge::onPacketReceived(mesh::Packet* packet) {
  handleReceivedPacket(packet);
}

// ── Static MQTT callback ──────────────────────────────────────────────────────

void MQTTBridge::mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (_instance) {
    _instance->handleMQTTMessage(payload, length);
  }
}

void MQTTBridge::getStatusStr(char* buf, int len) {
  if (!_initialized) {
    snprintf(buf, len, "WiFi:down MQTT:down");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(buf, len, "WiFi:connecting MQTT:down");
    return;
  }
  IPAddress ip = WiFi.localIP();
  if (!_mqtt_running) {
    snprintf(buf, len, "WiFi:%d.%d.%d.%d MQTT:stopped",
      ip[0], ip[1], ip[2], ip[3]);
    return;
  }
  snprintf(buf, len, "WiFi:%d.%d.%d.%d MQTT:%s",
    ip[0], ip[1], ip[2], ip[3],
    _mqttClient.connected() ? "connected" : "disconnected");
}

void MQTTBridge::handleMQTTMessage(const byte* payload, unsigned int length) {
  if (length == 0 || length > MAX_MQTT_PAYLOAD) {
    BRIDGE_DEBUG_PRINTLN("RX invalid length %d, dropping\n", (int)length);
    return;
  }

  mesh::Packet* pkt = _mgr->allocNew();
  if (!pkt) {
    BRIDGE_DEBUG_PRINTLN("RX failed to allocate packet\n");
    return;
  }

  if (pkt->readFrom(payload, (uint8_t)length)) {
    BRIDGE_DEBUG_PRINTLN("RX %d bytes\n", (int)length);
    _stats.rx_packets++;
    onPacketReceived(pkt);
  } else {
    BRIDGE_DEBUG_PRINTLN("RX failed to parse packet, dropping\n");
    _mgr->free(pkt);
  }
}

#endif // WITH_MQTT_BRIDGE
