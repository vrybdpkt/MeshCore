#include "WebInterface.h"

#ifdef WITH_WEB_INTERFACE

#include "MyMesh.h"
#include "web_ui.h"
#include <WiFi.h>

#ifndef MAX_NEIGHBOURS
  #define MAX_NEIGHBOURS 0
#endif
#ifndef MAX_CLIENTS
  #define MAX_CLIENTS 32
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Escape a C string for embedding in a JSON string value.
static int jsonEscape(const char* src, char* dst, int maxLen) {
  int i = 0;
  while (*src && i < maxLen - 2) {
    unsigned char c = (unsigned char)*src++;
    if (c == '"')       { if (i+2 >= maxLen) break; dst[i++]='\\'; dst[i++]='"'; }
    else if (c == '\\') { if (i+2 >= maxLen) break; dst[i++]='\\'; dst[i++]='\\'; }
    else if (c == '\n') { if (i+2 >= maxLen) break; dst[i++]='\\'; dst[i++]='n'; }
    else if (c == '\r') { if (i+2 >= maxLen) break; dst[i++]='\\'; dst[i++]='r'; }
    else if (c < 0x20)  { /* skip other control chars */ }
    else                { dst[i++] = (char)c; }
  }
  dst[i] = 0;
  return i;
}

// Very small JSON string-field extractor.  Finds "key":"value" and copies
// the unescaped value into 'out' (maxLen including null terminator).
static bool jsonGetStr(const char* json, const char* key, char* out, int maxLen) {
  char needle[80];
  snprintf(needle, sizeof(needle), "\"%s\":\"", key);
  const char* p = strstr(json, needle);
  if (!p) return false;
  p += strlen(needle);
  int i = 0;
  while (*p && *p != '"' && i < maxLen - 1) {
    if (*p == '\\' && *(p + 1)) {
      p++;
      if (*p == 'n') out[i++] = '\n';
      else if (*p == 'r') out[i++] = '\r';
      else if (*p == 't') out[i++] = '\t';
      else out[i++] = *p;
    } else {
      out[i++] = *p;
    }
    p++;
  }
  out[i] = 0;
  return true;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

WebInterface::WebInterface(MyMesh* mesh, uint16_t port)
  : _server(port), _mesh(mesh), _started(false),
    _cli_pending(false), _cli_done(false) {
  _cli_cmd[0] = 0;
  _cli_result[0] = 0;
}

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------

bool WebInterface::checkAuth(AsyncWebServerRequest* req) {
  const char* pw = _mesh->getNodePrefs()->password;
  if (pw[0] == 0) return true;  // no password set, open access
  if (!req->authenticate("admin", pw)) {
    req->requestAuthentication("MeshCore");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

void WebInterface::registerRoutes() {
  // Serve the SPA
  _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* r) { handleRoot(r); });

  // Status / stats
  _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* r) { handleStatus(r); });

  // Neighbors
  _server.on("/api/neighbors", HTTP_GET, [this](AsyncWebServerRequest* r) { handleNeighbors(r); });

  // Messages (GET with ?since=N, POST to send)
  _server.on("/api/messages", HTTP_GET, [this](AsyncWebServerRequest* r) { handleMessages(r); });
  _server.on("/api/messages", HTTP_POST,
    [](AsyncWebServerRequest*) {},  // onRequest (body arrives separately)
    nullptr,                        // onUpload
    [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t idx, size_t tot) {
      handlePostMessage(r, d, l, idx, tot);
    }
  );

  // CLI
  _server.on("/api/cli", HTTP_POST,
    [](AsyncWebServerRequest*) {},
    nullptr,
    [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t idx, size_t tot) {
      handleCLI(r, d, l, idx, tot);
    }
  );

  // Channel list (configured channels with names)
  _server.on("/api/channels", HTTP_GET, [this](AsyncWebServerRequest* r) { handleChannels(r); });

  // Region list (for scoped-flood scope dropdown)
  _server.on("/api/regions", HTTP_GET, [this](AsyncWebServerRequest* r) { handleRegions(r); });

  // Per-channel activity stats (for Dashboard channel activity card)
  _server.on("/api/channel_stats", HTTP_GET, [this](AsyncWebServerRequest* r) { handleChannelStats(r); });

  // MQTT bridge control
  _server.on("/api/mqtt/start", HTTP_POST, [this](AsyncWebServerRequest* r) { handleMQTTStart(r); });
  _server.on("/api/mqtt/stop",  HTTP_POST, [this](AsyncWebServerRequest* r) { handleMQTTStop(r); });

  // Packet log
  _server.on("/api/log", HTTP_GET, [this](AsyncWebServerRequest* r) { handleLog(r); });

  // 404
  _server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found");
  });
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WebInterface::begin() {
  registerRoutes();
}

void WebInterface::onWiFiConnected() {
  if (_started) return;
  _server.begin();
  _started = true;
}

void WebInterface::loop() {
  // Start the server the first time WiFi comes up, regardless of when that happens.
  if (!_started && WiFi.status() == WL_CONNECTED) {
    onWiFiConnected();
  }

  if (!_cli_pending) return;

  char reply[160] = {};
  _mesh->handleCommand(0, _cli_cmd, reply);
  strncpy(_cli_result, reply, sizeof(_cli_result) - 1);
  _cli_result[sizeof(_cli_result) - 1] = 0;
  _cli_done = true;
  _cli_pending = false;
}

// ---------------------------------------------------------------------------
// Route handlers
// ---------------------------------------------------------------------------

void WebInterface::handleRoot(AsyncWebServerRequest* req) {
  if (!checkAuth(req)) return;
  req->send_P(200, "text/html", WEB_UI_HTML);
}

void WebInterface::handleStatus(AsyncWebServerRequest* req) {
  if (!checkAuth(req)) return;

  NodePrefs* prefs = _mesh->getNodePrefs();

  // WiFi IP
  char wifi_ip[24] = "";
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    snprintf(wifi_ip, sizeof(wifi_ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  }

  // MQTT status
  char mqtt_status[160] = "";
  _mesh->getBridgeStatus(mqtt_status);
  bool mqtt_connected = strstr(mqtt_status, "MQTT:connected") != nullptr;

  // Stats from radio/mesh
  int packets_recv = 0, packets_sent = 0, last_snr = 0, last_rssi = 0, battery_mv = 0;
  uint32_t uptime_secs = 0;
  _mesh->fillRepeaterStats(&packets_recv, &packets_sent, &last_snr, &last_rssi, &battery_mv, &uptime_secs);

#ifdef WITH_MQTT_BRIDGE
  const MQTTBridge::Stats& ms = _mesh->getMQTTStats();
  uint32_t mqtt_tx = ms.tx_packets;
  uint32_t mqtt_rx = ms.rx_packets;
#else
  uint32_t mqtt_tx = 0, mqtt_rx = 0;
#endif

  // Node ID (first 4 bytes of pub key as hex)
  char node_id[9] = "00000000";
  {
    const uint8_t* pk = _mesh->getSelfId().pub_key;
    snprintf(node_id, sizeof(node_id), "%02X%02X%02X%02X", pk[0], pk[1], pk[2], pk[3]);
  }

  // Escape strings that could contain special characters
  char name_esc[68] = {};
  jsonEscape(prefs->node_name, name_esc, sizeof(name_esc));

  char sender_esc[68] = {};
  jsonEscape(prefs->sender_name[0] ? prefs->sender_name : prefs->node_name, sender_esc, sizeof(sender_esc));

  char mqtt_esc[200] = {};
  jsonEscape(mqtt_status, mqtt_esc, sizeof(mqtt_esc));

  char json[768];
  snprintf(json, sizeof(json),
    "{\"name\":\"%s\","
    "\"sender_name\":\"%s\","
    "\"node_id\":\"%s\","
    "\"firmware\":\"%s\","
    "\"build_date\":\"%s\","
    "\"role\":\"%s\","
    "\"uptime_secs\":%lu,"
    "\"freq_mhz\":%.3f,"
    "\"tx_power_dbm\":%d,"
    "\"packets_recv\":%d,"
    "\"packets_sent\":%d,"
    "\"last_snr\":%d,"
    "\"last_rssi\":%d,"
    "\"battery_mv\":%d,"
    "\"mqtt_tx\":%lu,"
    "\"mqtt_rx\":%lu,"
    "\"mqtt_connected\":%s,"
    "\"wifi_ip\":\"%s\","
    "\"mqtt_status\":\"%s\"}",
    name_esc,
    sender_esc,
    node_id,
    _mesh->getFirmwareVer(),
    _mesh->getBuildDate(),
    _mesh->getRole(),
    (unsigned long)uptime_secs,
    (double)prefs->freq,
    (int)prefs->tx_power_dbm,
    packets_recv,
    packets_sent,
    last_snr,
    last_rssi,
    battery_mv,
    (unsigned long)mqtt_tx,
    (unsigned long)mqtt_rx,
    mqtt_connected ? "true" : "false",
    wifi_ip,
    mqtt_esc
  );

  req->send(200, "application/json", json);
}

void WebInterface::handleNeighbors(AsyncWebServerRequest* req) {
  if (!checkAuth(req)) return;

#if MAX_NEIGHBOURS > 0
  NeighbourInfo nbrs[MAX_NEIGHBOURS];
  int count = _mesh->getNeighborsCopy(nbrs, MAX_NEIGHBOURS);
#else
  NeighbourInfo* nbrs = nullptr;
  int count = 0;
#endif

  // Also include logged-in ACL clients so the user has visibility even before
  // any radio advertisements arrive.
  WebContact clients[MAX_CLIENTS];
  int nClients = _mesh->getWebContacts(clients, MAX_CLIENTS);

  unsigned long now_ms = millis();

  // Build JSON
  String json = "{\"neighbors\":[";
  for (int i = 0; i < count; i++) {
    if (i) json += ',';
    char id_hex[9];
    snprintf(id_hex, sizeof(id_hex), "%02X%02X%02X%02X",
             nbrs[i].id.pub_key[0], nbrs[i].id.pub_key[1],
             nbrs[i].id.pub_key[2], nbrs[i].id.pub_key[3]);
    // secs_ago is millis-based (always valid regardless of RTC sync state).
    // heard_timestamp (RTC) is included for reference but not used for display.
    unsigned long secs_ago = (uint32_t)(now_ms - nbrs[i].heard_millis) / 1000UL;
    char entry[100];
    snprintf(entry, sizeof(entry), "{\"id\":\"%s\",\"snr\":%d,\"secs_ago\":%lu}",
             id_hex, (int)nbrs[i].snr, secs_ago);
    json += entry;
  }
  json += "],\"clients\":[";
  for (int i = 0; i < nClients; i++) {
    if (i) json += ',';
    char entry[80];
    snprintf(entry, sizeof(entry), "{\"id\":\"%s\",\"last_activity\":%lu}",
             clients[i].id_hex, (unsigned long)clients[i].last_activity);
    json += entry;
  }
  json += "]}";
  req->send(200, "application/json", json);
}

void WebInterface::handleMessages(AsyncWebServerRequest* req) {
  if (!checkAuth(req)) return;

  uint32_t since = 0;
  if (req->hasParam("since")) {
    since = (uint32_t)req->getParam("since")->value().toInt();
  }

  WebMsg msgs[20];
  int count = _mesh->getWebMsgsSince(since, msgs, 20);

  // Contacts (known ACL clients)
  WebContact contacts[MAX_CLIENTS];
  int nContacts = _mesh->getWebContacts(contacts, MAX_CLIENTS);

  String json = "{\"messages\":[";
  for (int i = 0; i < count; i++) {
    if (i) json += ',';
    char text_esc[340] = {};
    jsonEscape(msgs[i].text, text_esc, sizeof(text_esc));
    char from_esc[20] = {};
    jsonEscape(msgs[i].sender_hex, from_esc, sizeof(from_esc));
    char ch_esc[32] = {};
    jsonEscape(msgs[i].channel_tag, ch_esc, sizeof(ch_esc));
    char entry[520];
    snprintf(entry, sizeof(entry),
      "{\"seq\":%lu,\"ts\":%lu,\"from\":\"%s\",\"channel\":\"%s\",\"text\":\"%s\",\"outbound\":%s,\"snr\":%d,\"hops\":%u}",
      (unsigned long)msgs[i].seq, (unsigned long)msgs[i].timestamp,
      from_esc, ch_esc, text_esc,
      msgs[i].outbound ? "true" : "false",
      (int)msgs[i].snr, (unsigned)msgs[i].hops);
    json += entry;
  }
  json += "],\"contacts\":[";
  for (int i = 0; i < nContacts; i++) {
    if (i) json += ',';
    char entry[32];
    snprintf(entry, sizeof(entry), "{\"id\":\"%s\"}", contacts[i].id_hex);
    json += entry;
  }
  json += "]}";
  req->send(200, "application/json", json);
}

void WebInterface::handlePostMessage(AsyncWebServerRequest* req,
                                     uint8_t* data, size_t len,
                                     size_t index, size_t total) {
  if (index + len < total) return;  // wait for all body chunks
  if (!checkAuth(req)) return;

  char body[350] = {};
  size_t copyLen = len < sizeof(body) - 1 ? len : sizeof(body) - 1;
  memcpy(body, data, copyLen);

  char to[12] = {};
  char text[161] = {};
  char region[32] = {};
  jsonGetStr(body, "to", to, sizeof(to));
  jsonGetStr(body, "text", text, sizeof(text));
  jsonGetStr(body, "region", region, sizeof(region));

  if (text[0] == 0) {
    req->send(400, "application/json", "{\"error\":\"missing text\"}");
    return;
  }

  bool to_all = strcmp(to, "flood") == 0;
  // Map "channelN" form values to "CHANNELN" sentinels recognised by MyMesh::loop().
  const char* target = to;
  if (strncmp(to, "channel", 7) == 0) {
    if      (to[7] == '1') target = "CHANNEL1";
    else if (to[7] == '2') target = "CHANNEL2";
    else if (to[7] == '3') target = "CHANNEL3";
    else                   target = "CHANNEL0";  // "channel0" or legacy "channel"
  }
  _mesh->queueSendText(to_all, target, text, region[0] ? region : nullptr);
  req->send(202, "application/json", "{\"status\":\"queued\"}");
}

void WebInterface::handleCLI(AsyncWebServerRequest* req,
                              uint8_t* data, size_t len,
                              size_t index, size_t total) {
  if (index + len < total) return;  // wait for all chunks
  if (!checkAuth(req)) return;

  char body[200] = {};
  size_t copyLen = len < sizeof(body) - 1 ? len : sizeof(body) - 1;
  memcpy(body, data, copyLen);

  char cmd[160] = {};
  jsonGetStr(body, "command", cmd, sizeof(cmd));

  if (cmd[0] == 0) {
    req->send(400, "application/json", "{\"error\":\"missing command\"}");
    return;
  }

  if (_cli_pending) {
    req->send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  }

  strncpy(_cli_cmd, cmd, sizeof(_cli_cmd) - 1);
  _cli_cmd[sizeof(_cli_cmd) - 1] = 0;
  _cli_result[0] = 0;
  _cli_done = false;
  _cli_pending = true;  // signal main loop

  // Busy-wait up to 2 s for the main loop to process the command.
  // The AsyncWebServer handler runs on a separate FreeRTOS task, so yielding
  // is fine here.
  unsigned long start = millis();
  while (!_cli_done && (millis() - start < 2000UL)) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  char result_esc[340] = {};
  jsonEscape(_cli_result, result_esc, sizeof(result_esc));

  char json[380];
  snprintf(json, sizeof(json), "{\"result\":\"%s\"}", result_esc);
  req->send(200, "application/json", json);
}

void WebInterface::handleChannels(AsyncWebServerRequest* req) {
  if (!checkAuth(req)) return;
  char json[512];
  int pos = 0;
  pos += snprintf(json + pos, sizeof(json) - pos, "[");
  bool first = true;
  for (int i = 0; i < MAX_LISTEN_CHANNELS; i++) {
    char hash_hex[3] = {};
    char name[32]    = {};
    if (!_mesh->getChannelInfo(i, hash_hex, name)) continue;
    // Escape name for JSON (replace " with \")
    char esc_name[64] = {};
    int ei = 0;
    for (int j = 0; name[j] && ei < 62; j++) {
      if (name[j] == '"' || name[j] == '\\') esc_name[ei++] = '\\';
      esc_name[ei++] = name[j];
    }
    pos += snprintf(json + pos, sizeof(json) - pos,
                    "%s{\"idx\":%d,\"hash\":\"%s\",\"name\":\"%s\"}",
                    first ? "" : ",", i, hash_hex, esc_name);
    first = false;
  }
  snprintf(json + pos, sizeof(json) - pos, "]");
  req->send(200, "application/json", json);
}

void WebInterface::handleRegions(AsyncWebServerRequest* req) {
  if (!checkAuth(req)) return;
  // getRegionsList calls RegionMap::exportNamesTo which already strips any
  // leading '#' prefix, so names arrive as bare strings like "Home,Work,Net".
  // Skip the wildcard '*' entry — it isn't a meaningful scope choice.
  char names[256] = {};
  _mesh->getRegionsList(names, sizeof(names));

  char json[512];
  int pos = 0;
  pos += snprintf(json + pos, sizeof(json) - pos, "[");
  bool first = true;
  char* p = names;
  while (*p) {
    char* comma = strchr(p, ',');
    size_t len = comma ? (size_t)(comma - p) : strlen(p);
    if (len > 0 && len < 32 && !(len == 1 && *p == '*')) {
      pos += snprintf(json + pos, sizeof(json) - pos,
                      "%s\"%.*s\"", first ? "" : ",", (int)len, p);
      first = false;
    }
    if (!comma) break;
    p = comma + 1;
  }
  snprintf(json + pos, sizeof(json) - pos, "]");
  req->send(200, "application/json", json);
}

void WebInterface::handleChannelStats(AsyncWebServerRequest* req) {
  if (!checkAuth(req)) return;

  ChannelStat stats[MAX_CHANNEL_STATS];
  int n = _mesh->getChannelStats(stats, MAX_CHANNEL_STATS);
  unsigned long now_ms = millis();

  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i) json += ',';
    float avg_snr = stats[i].snr_count > 0
      ? (float)stats[i].snr_sum / (4.0f * stats[i].snr_count)
      : 0.0f;
    unsigned long secs_ago = (uint32_t)(now_ms - stats[i].last_millis) / 1000UL;
    char name_esc[32] = {};
    jsonEscape(stats[i].name, name_esc, sizeof(name_esc));
    char entry[128];
    snprintf(entry, sizeof(entry),
      "{\"hash\":\"%s\",\"name\":\"%s\",\"pkts\":%lu,\"secs_ago\":%lu,\"avg_snr\":%.1f,\"has_psk\":%s}",
      stats[i].hash_hex, name_esc,
      (unsigned long)stats[i].pkt_count, secs_ago, avg_snr,
      stats[i].has_psk ? "true" : "false");
    json += entry;
  }
  json += "]";
  req->send(200, "application/json", json);
}

void WebInterface::handleMQTTStart(AsyncWebServerRequest* req) {
  if (!checkAuth(req)) return;
  _mesh->setBridgeState(true);
  req->send(200, "application/json", "{\"status\":\"started\"}");
}

void WebInterface::handleMQTTStop(AsyncWebServerRequest* req) {
  if (!checkAuth(req)) return;
  _mesh->setBridgeState(false);
  req->send(200, "application/json", "{\"status\":\"stopped\"}");
}

void WebInterface::handleLog(AsyncWebServerRequest* req) {
  if (!checkAuth(req)) return;
  // Stream the packet log file from SPIFFS
  if (SPIFFS.exists(PACKET_LOG_FILE)) {
    req->send(SPIFFS, PACKET_LOG_FILE, "text/plain");
  } else {
    req->send(200, "text/plain", "(packet log is empty or logging is disabled)");
  }
}

#endif // WITH_WEB_INTERFACE
