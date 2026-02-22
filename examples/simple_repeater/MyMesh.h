#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <RTClib.h>
#include <target.h>

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

#ifdef WITH_RS232_BRIDGE
#include "helpers/bridges/RS232Bridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_ESPNOW_BRIDGE
#include "helpers/bridges/ESPNowBridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_MQTT_BRIDGE
#include "helpers/bridges/MQTTBridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_WEB_INTERFACE
// Forward-declare to break the include cycle: WebInterface.h includes MyMesh.h
class WebInterface;

// Message entry stored in the ring buffer for the web UI.
struct WebMsg {
  uint32_t seq;
  uint32_t timestamp;
  char     sender_hex[9];   // sender name or pub-key prefix (max 8 chars + null)
  char     channel_tag[16]; // e.g. "Public [8B]", "Direct", "Flood", "CH8B"
  char     text[161];       // MAX_TEXT_LEN = 160 chars + null
  bool     outbound;
  int8_t   snr;             // receive SNR x4 (quarter-dB; 0 for outbound/unknown)
  uint8_t  hops;            // path_len at receive time (0 = zero-hop / direct)
};

// Per-channel-hash activity counters maintained regardless of whether a PSK is configured.
struct ChannelStat {
  char     hash_hex[3];   // uppercase hex of the 1-byte channel hash, e.g. "5B"
  char     name[16];      // channel name (empty if not configured)
  uint32_t pkt_count;     // total packets observed this session
  uint32_t last_millis;   // millis() of most recent packet
  int32_t  snr_sum;       // cumulative SNR x4 for averaging
  int      snr_count;     // number of SNR samples
  bool     has_psk;       // true if a matching PSK is configured
};
#define MAX_CHANNEL_STATS 16
#ifndef WEB_MSG_BUF
  #define WEB_MSG_BUF 200  // allocated from PSRAM at runtime; override via build flags
#endif

// Contact entry (known ACL client) for the web UI compose dropdown.
struct WebContact {
  char     id_hex[9];      // 4-byte pub key prefix as uppercase hex + null
  uint32_t last_activity;  // RTC timestamp of last received message (0 = never)
};
#endif // WITH_WEB_INTERFACE

#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/ClientACL.h>
#include <helpers/CommonCLI.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/RegionMap.h>
#include "RateLimiter.h"

#ifdef WITH_BRIDGE
extern AbstractBridge* bridge;
#endif

struct RepeaterStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;                // was 'n_full_events'
  int16_t  last_snr;   // x 4
  uint16_t n_direct_dups, n_flood_dups;
  uint32_t total_rx_air_time_secs;
  uint32_t n_recv_errors;
};

#ifndef MAX_CLIENTS
  #define MAX_CLIENTS           32
#endif

struct NeighbourInfo {
  mesh::Identity id;
  uint32_t advert_timestamp;
  uint32_t heard_timestamp;
  int8_t snr; // multiplied by 4, user should divide to get float value
  uint32_t heard_millis; // millis() at time of hearing (always valid, RTC-independent)
};

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE   "15 Feb 2026"
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.13.0"
#endif

#define FIRMWARE_ROLE "repeater"

#define PACKET_LOG_FILE  "/packet_log"

class MyMesh : public mesh::Mesh, public CommonCLICallbacks {
  FILESYSTEM* _fs;
  uint32_t last_millis;
  uint64_t uptime_millis;
  unsigned long next_local_advert, next_flood_advert;
  bool _logging;
  NodePrefs _prefs;
  ClientACL  acl;
  CommonCLI _cli;
  uint8_t reply_data[MAX_PACKET_PAYLOAD];
  uint8_t reply_path[MAX_PATH_SIZE];
  int8_t  reply_path_len;
  TransportKeyStore key_store;
  RegionMap region_map, temp_map;
  RegionEntry* load_stack[8];
  RegionEntry* recv_pkt_region;
  RateLimiter discover_limiter, anon_limiter;
  bool region_load_active;
  unsigned long dirty_contacts_expiry;
#if MAX_NEIGHBOURS
  NeighbourInfo neighbours[MAX_NEIGHBOURS];
#endif
  CayenneLPP telemetry;
  unsigned long set_radio_at, revert_radio_at;
  float pending_freq;
  float pending_bw;
  uint8_t pending_sf;
  uint8_t pending_cr;
  int  matching_peer_indexes[MAX_CLIENTS];
  #define MAX_LISTEN_CHANNELS 4
  mesh::GroupChannel _listen_channels[MAX_LISTEN_CHANNELS];  // up to 4 group-channel PSKs (indexed by slot)
  int                _num_listen_channels;                   // highest configured slot + 1
  char               _listen_channel_names[MAX_LISTEN_CHANNELS][32];
#if defined(WITH_RS232_BRIDGE)
  RS232Bridge bridge;
#elif defined(WITH_ESPNOW_BRIDGE)
  ESPNowBridge bridge;
#elif defined(WITH_MQTT_BRIDGE)
  MQTTBridge bridge;
#endif

#ifdef WITH_WEB_INTERFACE
  // Message ring buffer (written from main task, read from AsyncWebServer task).
  // Allocated from PSRAM in begin() so it doesn't consume internal SRAM.
  WebMsg*      _web_msgs;
  uint32_t     _web_msg_seq;
  int          _web_msg_head;   // next write slot (0..WEB_MSG_BUF-1)
  int          _web_msg_count;  // number of valid entries
  portMUX_TYPE _web_msg_mux;

  // Channel activity tracker — updated for every GRP_TXT received (PSK or not).
  ChannelStat  _ch_stats[MAX_CHANNEL_STATS];
  int          _num_ch_stats;

  // Pending send request from the web UI (web task writes, main loop reads)
  struct {
    volatile bool pending;
    bool          to_all;
    char          target_hex[9];  // 4-byte pub key prefix hex, or ignored when to_all
    char          text[161];
    char          region[32];     // region name for scoped flood (empty = unscoped)
  } _pending_send;

  WebInterface* _web;
#endif

  void putNeighbour(const mesh::Identity& id, uint32_t timestamp, float snr);
  uint8_t handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood);
  uint8_t handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  int handleRequest(ClientInfo* sender, uint32_t sender_timestamp, uint8_t* payload, size_t payload_len);
  mesh::Packet* createSelfAdvert();

  File openAppend(const char* fname);

protected:
  float getAirtimeBudgetFactor() const override {
    return _prefs.airtime_factor;
  }

  bool allowPacketForward(const mesh::Packet* packet) override;
  const char* getLogDateTime() override;
  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;

  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void logTxFail(mesh::Packet* pkt, int len) override;
  int calcRxDelay(float score, uint32_t air_time) const override;

  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;

  int getInterferenceThreshold() const override {
    return _prefs.interference_threshold;
  }
  int getAGCResetInterval() const override {
    return ((int)_prefs.agc_reset_interval) * 4000;   // milliseconds
  }
  uint8_t getExtraAckTransmitCount() const override {
    return _prefs.multi_acks;
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    sensors.setSettingValue("gps", _prefs.gps_enabled?"1":"0");
  }
#endif

  bool filterRecvFloodPacket(mesh::Packet* pkt) override;

  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data, size_t len) override;
  int  searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) override;
  void onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) override;
  int searchPeersByHash(const uint8_t* hash) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len);
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onControlDataRecv(mesh::Packet* packet) override;

public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);

  void begin(FILESYSTEM* fs);

  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
  const char* getNodeName() { return _prefs.node_name; }
  NodePrefs* getNodePrefs() {
    return &_prefs;
  }

  void savePrefs() override {
    _cli.savePrefs(_fs);
  }

  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;
  bool formatFileSystem() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;

  void setLoggingOn(bool enable) override { _logging = enable; }

  void eraseLogFile() override {
    _fs->remove(PACKET_LOG_FILE);
  }

  void dumpLogFile() override;
  void setTxPower(int8_t power_dbm) override;
  void formatNeighborsReply(char *reply) override;
  void removeNeighbor(const uint8_t* pubkey, int key_len) override;
  void formatStatsReply(char *reply) override;
  void formatRadioStatsReply(char *reply) override;
  void formatPacketStatsReply(char *reply) override;

  mesh::LocalIdentity& getSelfId() override { return self_id; }

  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);
  void loop();

#if defined(WITH_BRIDGE)
  void setBridgeState(bool enable) override {
#if defined(WITH_MQTT_BRIDGE)
    // MQTT build: controls MQTT only — WiFi is managed independently.
    if (enable) bridge.startMQTT();
    else        bridge.stopMQTT();
#else
    if (enable == bridge.isRunning()) return;
    if (enable) bridge.begin();
    else        bridge.end();
#endif
  }

#if defined(WITH_MQTT_BRIDGE)
  void setWifiState(bool enable) override {
    if (enable) bridge.begin();   // (re)connect WiFi; MQTT follows mqtt_autostart
    else        bridge.endAll();  // disconnect MQTT then WiFi
  }
#endif

  void restartBridge() override {
#if defined(WITH_MQTT_BRIDGE)
    // Restart MQTT only; WiFi stays up.
    bridge.stopMQTT();
    bridge.startMQTT();
#else
    bridge.end();
    bridge.begin();
#endif
  }

#if defined(WITH_MQTT_BRIDGE)
  void getBridgeStatus(char* buf) override {
    bridge.getStatusStr(buf, 159);
  }
  const MQTTBridge::Stats& getMQTTStats() const {
    return bridge.getStats();
  }
#endif
#endif

  // To check if there is pending work
  bool hasPendingWork() const;

#ifdef WITH_WEB_INTERFACE
  // Web interface API — called from WebInterface handlers (separate FreeRTOS task).
  void pushWebMsg(const char* sender_hex, const char* channel_tag, const char* text, bool outbound, int8_t snr = 0, uint8_t hops = 0);
  int  getWebMsgsSince(uint32_t since, WebMsg* out, int maxCount);
  int  getWebContacts(WebContact* out, int maxCount);
  int  getNeighborsCopy(NeighbourInfo* out, int maxCount);
  void fillRepeaterStats(int* packets_recv, int* packets_sent,
                         int* last_snr,     int* last_rssi,
                         int* battery_mv,   uint32_t* uptime_secs);
  // Queue a text message to send (processed by MyMesh::loop on the main task).
  // region: region name for transport-code scoping, or "" for unscoped flood.
  void queueSendText(bool to_all, const char* target_hex, const char* text, const char* region = "");
  // Send helpers — called from loop() on the main Arduino task.
  bool sendTextToClient(const uint8_t* pubkey4, const char* text);
  bool sendTextToAllClients(const char* text);
  bool sendChannelText(int ch_idx, const char* text, const char* region = nullptr);  // send GRP_TXT on channel ch_idx (0..3)
  // Returns comma-separated region names configured on this repeater (for web UI dropdown).
  void getRegionsList(char* buf, int maxLen);
  // Returns true if slot idx has a PSK configured; fills hash_hex (2 upper-hex chars + null)
  // and name (empty string if not named).
  bool getChannelInfo(int idx, char hash_hex_out[3], char name_out[32]) const;
  int  getNumListenChannels() const { return _num_listen_channels; }
  int  getChannelStats(ChannelStat* out, int maxCount);
#endif
};
