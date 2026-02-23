#pragma once

#ifdef WITH_MQTT_BRIDGE

#include "helpers/bridges/BridgeBase.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

/**
 * @brief Bridge implementation that carries MeshCore packets over MQTT,
 *        enabling two (or more) geographically separate mesh networks to
 *        behave as a single logical mesh.
 *
 * Each site subscribes AND publishes to the same MQTT topic.  The inherited
 * BridgeBase::_seen_packets table (SimpleMeshTables) handles loop prevention:
 *   - Packets forwarded TO MQTT are marked seen, so if the broker echoes them
 *     back they are dropped rather than re-injected into the local mesh.
 *   - Packets received FROM MQTT are marked seen, so the mesh will not
 *     re-publish them back to MQTT after retransmitting over RF.
 *
 * Required build defines:
 *   WITH_MQTT_BRIDGE=1
 *   WITH_MQTT_BRIDGE_SSID="your-ssid"
 *   WITH_MQTT_BRIDGE_WIFI_PASS="your-wifi-password"
 *   WITH_MQTT_BRIDGE_SERVER="broker.example.com"
 *
 * Optional build defines:
 *   WITH_MQTT_BRIDGE_PORT=1883        (default 1883; use 8883 for TLS)
 *   WITH_MQTT_BRIDGE_TOPIC="mc/net"  (default "meshcore/bridge")
 *   WITH_MQTT_BRIDGE_USER=""          (default: no auth)
 *   WITH_MQTT_BRIDGE_PASS=""          (default: no auth)
 *   WITH_MQTT_BRIDGE_TLS=1            (default 0; enables TLS without cert verification)
 */

#ifndef WITH_MQTT_BRIDGE_PORT
  #define WITH_MQTT_BRIDGE_PORT 1883
#endif

#ifndef WITH_MQTT_BRIDGE_TOPIC
  #define WITH_MQTT_BRIDGE_TOPIC "meshcore/bridge"
#endif

#ifndef WITH_MQTT_BRIDGE_TLS
  #define WITH_MQTT_BRIDGE_TLS 0
#endif

#ifndef WITH_MQTT_BRIDGE_USER
  #define WITH_MQTT_BRIDGE_USER ""
#endif

#ifndef WITH_MQTT_BRIDGE_PASS
  #define WITH_MQTT_BRIDGE_PASS ""
#endif

class MQTTBridge : public BridgeBase {
public:
  MQTTBridge(NodePrefs* prefs, mesh::PacketManager* mgr, mesh::RTCClock* rtc);

  /**
   * Connect WiFi. If bridge.autostart is enabled, also start the MQTT bridge.
   * Safe to call repeatedly — connectWiFi() returns early if already connected.
   */
  void begin() override;

  /**
   * Stop the MQTT bridge only. WiFi stays connected so the device can be
   * used as a plain WiFi repeater without forwarding mesh packets.
   */
  void end() override;

  /** Stop the MQTT bridge AND disconnect WiFi entirely. */
  void endAll();

  /** Start (or restart) the MQTT bridge. Brings up WiFi first if needed. */
  void startMQTT();

  /** Stop the MQTT bridge without touching WiFi. */
  void stopMQTT();

  void loop() override;

  /**
   * Called by MyMesh::logTx (or logRx depending on bridge_pkt_src).
   * Serialises the packet and publishes raw bytes to the MQTT topic,
   * unless it was already seen (i.e. arrived via MQTT itself).
   */
  void sendPacket(mesh::Packet* packet) override;

  /**
   * Called once a received MQTT payload has been parsed into a mesh::Packet.
   * Hands off to BridgeBase::handleReceivedPacket which marks it seen and
   * queues it for the mesh.
   */
  void onPacketReceived(mesh::Packet* packet) override;

  /** Fills buf with a one-line WiFi/MQTT status string for the CLI. */
  void getStatusStr(char* buf, int len);

  struct Stats {
    uint32_t tx_packets;   // packets successfully published to MQTT
    uint32_t rx_packets;   // packets received from MQTT and injected into mesh
    uint32_t tx_filtered;  // packets dropped by shouldBridgePacket()
    uint32_t rx_banned;    // packets dropped because source is banned
    uint32_t reconnects;   // successful MQTT (re)connections
  };
  const Stats& getStats() const { return _stats; }

  bool banNode(const uint8_t prefix[4]);

  bool unbanNode(const uint8_t prefix[4]);

  void getBanListStr(char* buf, int len) const;

  void setAppCallbacks(CommonCLICallbacks* cb) { _app_cb = cb; }

  static constexpr int MQTT_BAN_LIST_SIZE = 16;

  static constexpr uint8_t BAN_CMD_MAGIC[3] = {0xBA, 0x4E, 0xED};
  static constexpr uint8_t BAN_CMD_LEN = 7; // 3 magic + 4 pubkey prefix bytes

private:
  static MQTTBridge* _instance; // for the static PubSubClient callback
  bool _mqtt_running = false;   // MQTT bridge active (independent of WiFi)
  Stats _stats = {};

  uint8_t _ban_prefixes[MQTT_BAN_LIST_SIZE][4]; 
  uint8_t _ban_count = 0;

  CommonCLICallbacks* _app_cb = nullptr;
  bool _deferred_self_ban = false; // set in MQTT callback; handled in loop()

  void sendBanCommand(const uint8_t prefix[4]);

  void executeSelfBan();

#if WITH_MQTT_BRIDGE_TLS
  WiFiClientSecure _wifiClient;
#else
  WiFiClient _wifiClient;
#endif
  PubSubClient _mqttClient;
  unsigned long _lastReconnectAttempt;

  // Scratch buffer large enough for any serialised mesh packet
  static constexpr size_t MAX_MQTT_PAYLOAD = MAX_TRANS_UNIT + 1;
  uint8_t _tx_buffer[MAX_MQTT_PAYLOAD];

  bool connectWiFi();
  bool connectMQTT();

  static void mqttCallback(char* topic, byte* payload, unsigned int length);
  void handleMQTTMessage(const byte* payload, unsigned int length);
};

#endif // WITH_MQTT_BRIDGE
