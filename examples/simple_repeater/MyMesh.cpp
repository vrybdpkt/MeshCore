#include "MyMesh.h"
#include <algorithm>

#ifdef WITH_WEB_INTERFACE
#include "WebInterface.h"
#include <WiFi.h>
#define WEB_MAX_TEXT_LEN  160   // same as MAX_TEXT_LEN in BaseChatMesh.h
// Forward declaration — used in allowPacketForward and the group-channel section.
static void updateChannelStat(ChannelStat stats[], int& num_stats, const char* hash_hex,
                               const char* name, bool has_psk, int8_t snr_x4);
#endif

/* ------------------------------ Config -------------------------------- */

#ifndef LORA_FREQ
  #define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
  #define LORA_BW 250
#endif
#ifndef LORA_SF
  #define LORA_SF 10
#endif
#ifndef LORA_CR
  #define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER 20
#endif

#ifndef ADVERT_NAME
  #define ADVERT_NAME "repeater"
#endif
#ifndef ADVERT_LAT
  #define ADVERT_LAT 0.0
#endif
#ifndef ADVERT_LON
  #define ADVERT_LON 0.0
#endif

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY 300
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY 200
#endif

#define FIRMWARE_VER_LEVEL       2

#define REQ_TYPE_GET_STATUS         0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05
#define REQ_TYPE_GET_NEIGHBOURS     0x06
#define REQ_TYPE_GET_OWNER_INFO     0x07     // FIRMWARE_VER_LEVEL >= 2

#define RESP_SERVER_LOGIN_OK        0 // response to ANON_REQ

#define ANON_REQ_TYPE_REGIONS      0x01
#define ANON_REQ_TYPE_OWNER        0x02
#define ANON_REQ_TYPE_BASIC        0x03   // just remote clock

#define CLI_REPLY_DELAY_MILLIS      600

#define LAZY_CONTACTS_WRITE_DELAY    5000

void MyMesh::putNeighbour(const mesh::Identity &id, uint32_t timestamp, float snr) {
#if MAX_NEIGHBOURS // check if neighbours enabled
  // find existing neighbour, else use least recently updated
  uint32_t oldest_timestamp = 0xFFFFFFFF;
  NeighbourInfo *neighbour = &neighbours[0];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    // if neighbour already known, we should update it
    if (id.matches(neighbours[i].id)) {
      neighbour = &neighbours[i];
      break;
    }

    // otherwise we should update the least recently updated neighbour
    if (neighbours[i].heard_timestamp < oldest_timestamp) {
      neighbour = &neighbours[i];
      oldest_timestamp = neighbour->heard_timestamp;
    }
  }

  // update neighbour info
  neighbour->id = id;
  neighbour->advert_timestamp = timestamp;
  neighbour->heard_timestamp = getRTCClock()->getCurrentTime();
  neighbour->heard_millis = (uint32_t)millis();
  neighbour->snr = (int8_t)(snr * 4);
#endif
}

uint8_t MyMesh::handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood) {
  ClientInfo* client = NULL;
  if (data[0] == 0) {   // blank password, just check if sender is in ACL
    client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    if (client == NULL) {
    #if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Login, sender not in ACL");
    #endif
    }
  }
  if (client == NULL) {
    uint8_t perms;
    if (strcmp((char *)data, _prefs.password) == 0) { // check for valid admin password
      perms = PERM_ACL_ADMIN;
    } else if (strcmp((char *)data, _prefs.guest_password) == 0) { // check guest password
      perms = PERM_ACL_GUEST;
    } else {
#if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Invalid password: %s", data);
#endif
      return 0;
    }

    client = acl.putClient(sender, 0);  // add to contacts (if not already known)
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("Possible login replay attack!");
      return 0;  // FATAL: client table is full -OR- replay attack
    }

    MESH_DEBUG_PRINTLN("Login success!");
    client->last_timestamp = sender_timestamp;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~0x03;
    client->permissions |= perms;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    if (perms != PERM_ACL_GUEST) {   // keep number of FS writes to a minimum
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }
  }

  if (is_flood) {
    client->out_path_len = -1;  // need to rediscover out_path
  }

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;  // Legacy: was recommended keep-alive interval (secs / 16)
  reply_data[6] = client->isAdmin() ? 1 : 0;
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);   // random blob to help packet-hash uniqueness
  reply_data[12] = FIRMWARE_VER_LEVEL;  // New field

  return 13;  // reply length
}

uint8_t MyMesh::handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++ & 0x3F;
    memcpy(reply_path, data, reply_path_len);
    // data += reply_path_len;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)

    return 8 + region_map.exportNamesTo((char *) &reply_data[8], sizeof(reply_data) - 12, REGION_DENY_FLOOD);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++ & 0x3F;
    memcpy(reply_path, data, reply_path_len);
    // data += reply_path_len;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    sprintf((char *) &reply_data[8], "%s\n%s", _prefs.node_name, _prefs.owner_info);

    return 8 + strlen((char *) &reply_data[8]);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++ & 0x3F;
    memcpy(reply_path, data, reply_path_len);
    // data += reply_path_len;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    reply_data[8] = 0;  // features
#ifdef WITH_RS232_BRIDGE
    reply_data[8] |= 0x01;  // is bridge, type UART
#elif WITH_ESPNOW_BRIDGE
    reply_data[8] |= 0x03;  // is bridge, type ESP-NOW
#endif
    if (_prefs.disable_fwd) {   // is this repeater currently disabled
      reply_data[8] |= 0x80;  // is disabled
    }
    // TODO:  add some kind of moving-window utilisation metric, so can query 'how busy' is this repeater
    return 9;   // reply length
  }
  return 0;
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload, size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {  // guests can also access this now
    RepeaterStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundCount(0xFFFFFFFF);
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables *)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables *)getTables())->getNumFloodDups();
    stats.total_rx_air_time_secs = getReceiveAirTime() / 1000;
    stats.n_recv_errors = radio_driver.getPacketsRecvErrors();
    memcpy(&reply_data[4], &stats, sizeof(stats));

    return 4 + sizeof(stats); //  reply_len
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]); // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions

    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);

    // query other sensors -- target specific
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
      perm_mask = 0x00;  // just base telemetry allowed
    }
    sensors.querySensors(perm_mask, telemetry);

	// This default temperature will be overridden by external sensors (if any)
    float temperature = board.getMCUTemperature();
    if(!isnan(temperature)) { // Supported boards with built-in temperature sensor. ESP32-C3 may return NAN
      telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature); // Built-in MCU Temperature
    }

    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen; // reply_len
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    uint8_t res1 = payload[1];   // reserved for future  (extra query params)
    uint8_t res2 = payload[2];
    if (res1 == 0 && res2 == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients() && ofs + 7 <= sizeof(reply_data) - 4; i++) {
        auto c = acl.getClientByIdx(i);
        if (c->permissions == 0) continue;  // skip deleted entries
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;  // just 6-byte pub_key prefix
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  if (payload[0] == REQ_TYPE_GET_NEIGHBOURS) {
    uint8_t request_version = payload[1];
    if (request_version == 0) {

      // reply data offset (after response sender_timestamp/tag)
      int reply_offset = 4;

      // get request params
      uint8_t count = payload[2]; // how many neighbours to fetch (0-255)
      uint16_t offset;
      memcpy(&offset, &payload[3], 2); // offset from start of neighbours list (0-65535)
      uint8_t order_by = payload[5]; // how to order neighbours. 0=newest_to_oldest, 1=oldest_to_newest, 2=strongest_to_weakest, 3=weakest_to_strongest
      uint8_t pubkey_prefix_length = payload[6]; // how many bytes of neighbour pub key we want
      // we also send a 4 byte random blob in payload[7...10] to help packet uniqueness

      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS count=%d, offset=%d, order_by=%d, pubkey_prefix_length=%d", count, offset, order_by, pubkey_prefix_length);

      // clamp pub key prefix length to max pub key length
      if(pubkey_prefix_length > PUB_KEY_SIZE){
        pubkey_prefix_length = PUB_KEY_SIZE;
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS invalid pubkey_prefix_length=%d clamping to %d", pubkey_prefix_length, PUB_KEY_SIZE);
      }

      // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
      int16_t neighbours_count = 0;
      NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
      for (int i = 0; i < MAX_NEIGHBOURS; i++) {
        auto neighbour = &neighbours[i];
        const uint8_t* pk = neighbour->id.pub_key;
        if (pk[0] || pk[1] || pk[2] || pk[3]) {
          sorted_neighbours[neighbours_count] = neighbour;
          neighbours_count++;
        }
      }

      // sort neighbours based on order
      if (order_by == 0) {
        // sort by newest to oldest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting newest to oldest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp > b->heard_timestamp; // desc
        });
      } else if (order_by == 1) {
        // sort by oldest to newest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting oldest to newest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp < b->heard_timestamp; // asc
        });
      } else if (order_by == 2) {
        // sort by strongest to weakest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting strongest to weakest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr > b->snr; // desc
        });
      } else if (order_by == 3) {
        // sort by weakest to strongest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting weakest to strongest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr < b->snr; // asc
        });
      }

      // build results buffer
      int results_count = 0;
      int results_offset = 0;
      uint8_t results_buffer[130];
      for(int index = 0; index < count && index + offset < neighbours_count; index++){
        
        // stop if we can't fit another entry in results
        int entry_size = pubkey_prefix_length + 4 + 1;
        if(results_offset + entry_size > sizeof(results_buffer)){
          MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS no more entries can fit in results buffer");
          break;
        }

        // add next neighbour to results
        auto neighbour = sorted_neighbours[index + offset];
        uint32_t heard_seconds_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
        memcpy(&results_buffer[results_offset], neighbour->id.pub_key, pubkey_prefix_length); results_offset += pubkey_prefix_length;
        memcpy(&results_buffer[results_offset], &heard_seconds_ago, 4); results_offset += 4;
        memcpy(&results_buffer[results_offset], &neighbour->snr, 1); results_offset += 1;
        results_count++;

      }

      // build reply
      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS neighbours_count=%d results_count=%d", neighbours_count, results_count);
      memcpy(&reply_data[reply_offset], &neighbours_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_buffer, results_offset); reply_offset += results_offset;

      return reply_offset;
    }
  } else if (payload[0] == REQ_TYPE_GET_OWNER_INFO) {
    sprintf((char *) &reply_data[4], "%s\n%s\n%s", FIRMWARE_VERSION, _prefs.node_name, _prefs.owner_info);
    return 4 + strlen((char *) &reply_data[4]);
  }
  return 0; // unknown command
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_REPEATER, app_data);

  return createAdvert(self_id, app_data, app_data_len);
}

File MyMesh::openAppend(const char *fname) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood() && packet->path_len >= _prefs.flood_max) return false;
  if (packet->isRouteFlood() && recv_pkt_region == NULL) {
    if (packet->getRouteType() != ROUTE_TYPE_FLOOD) {
      // TRANSPORT_FLOOD with an unknown/unmatched transport code — don't forward.
      MESH_DEBUG_PRINTLN("allowPacketForward: unknown transport code for TRANSPORT_FLOOD packet");
      return false;
    }
    // Plain ROUTE_TYPE_FLOOD: filterRecvFloodPacket may not have been called
    // (e.g., for bridge-injected packets).  Consult the wildcard region directly.
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      return false;
    }
    recv_pkt_region = &region_map.getWildcard();
  }
#ifdef WITH_WEB_INTERFACE
  // Count forwarded direct (TXT_MSG) packets for the Dashboard channel-activity card
  // and push a placeholder into the message ring buffer so they appear in the All tab.
  if (packet->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
    int8_t snr_x4 = (int8_t)(radio_driver.getLastSNR() * 4.0f);
    uint8_t hops  = (uint8_t)packet->path_len;
    updateChannelStat(_ch_stats, _num_ch_stats, "DM", "Direct fwd", false, snr_x4);
    // payload[1] = source 1-byte hash, payload[0] = destination 1-byte hash (same
    // layout logged by logRx/logTx: [src -> dest]).
    char src_hex[3], dst_tag[8];
    snprintf(src_hex, sizeof(src_hex), "%02X", (uint8_t)packet->payload[1]);
    snprintf(dst_tag, sizeof(dst_tag), "\xe2\x86\x92%02X", (uint8_t)packet->payload[0]); // →XX
    pushWebMsg(src_hex, dst_tag, "(private \xe2\x80\x94 forwarded)", false, snr_x4, hops);
  }
#endif
  return true;
}

const char *MyMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
#if MESH_PACKET_LOGGING
  Serial.print(getLogDateTime());
  Serial.print(" RAW: ");
  mesh::Utils::printHex(Serial, raw, len);
  Serial.println();
#endif
}

void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 1) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": RX, len=%d (type=%d, route=%s, payload_len=%d) SNR=%d RSSI=%d score=%d", len,
               pkt->getPayloadType(), pkt->isRouteDirect() ? "D" : "F", pkt->payload_len,
               (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(), (int)(score * 1000));

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTx(mesh::Packet *pkt, int len) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 0) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX, len=%d (type=%d, route=%s, payload_len=%d)", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTxFail(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX FAIL!, len=%d (type=%d, route=%s, payload_len=%d)\n", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
      f.close();
    }
  }
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint32_t MyMesh::getRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->path_len + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->path_len + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}

bool MyMesh::filterRecvFloodPacket(mesh::Packet* pkt) {
  // just try to determine region for packet (apply later in allowPacketForward())
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = NULL;
    } else {
      recv_pkt_region =  &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = NULL;
  }
  // do normal processing
  return false;
}

void MyMesh::onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                            uint8_t *data, size_t len) {
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) { // received an initial request by a possible admin
                                                           // client (unknown at this stage)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    data[len] = 0;  // ensure null terminator
    uint8_t reply_len;

    reply_path_len = -1;
    if (data[4] == 0 || data[4] >= ' ') {   // is password, ie. a login request
      reply_len = handleLoginReq(sender, secret, timestamp, &data[4], packet->isRouteFlood());
    } else if (data[4] == ANON_REQ_TYPE_REGIONS && packet->isRouteDirect()) {
      reply_len = handleAnonRegionsReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_OWNER && packet->isRouteDirect()) {
      reply_len = handleAnonOwnerReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_BASIC && packet->isRouteDirect()) {
      reply_len = handleAnonClockReq(sender, timestamp, &data[5]);
    } else {
      reply_len = 0;  // unknown/invalid request type
    }

    if (reply_len == 0) return;   // invalid request

    if (packet->isRouteFlood()) {
      // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
      mesh::Packet* path = createPathReturn(sender, secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
      if (path) sendFlood(path, SERVER_RESPONSE_DELAY);
    } else if (reply_path_len < 0) {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
      if (reply) sendFlood(reply, SERVER_RESPONSE_DELAY);
    } else {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
      if (reply) sendDirect(reply, reply_path, reply_path_len, SERVER_RESPONSE_DELAY);
    }
  }
}

int MyMesh::searchPeersByHash(const uint8_t *hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i; // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

void MyMesh::getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

static bool isShare(const mesh::Packet *packet) {
  if (packet->hasTransportCodes()) {
    return packet->transport_codes[0] == 0 && packet->transport_codes[1] == 0;  // codes { 0, 0 } means 'send to nowhere'
  }
  return false;
}

void MyMesh::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                          const uint8_t *app_data, size_t app_data_len) {
  mesh::Mesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len); // chain to super impl

  // if this a zero hop advert (and not via 'Share'), add it to neighbours
  if (packet->path_len == 0 && !isShare(packet)) {
    AdvertDataParser parser(app_data, app_data_len);
    if (parser.isValid()) {   // keep all direct neighbours (repeaters, companions, sensors, etc.)
      putNeighbour(id, timestamp, packet->getSNR());
    }
  }
}

void MyMesh::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret,
                            uint8_t *data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("onPeerDataRecv: invalid peer idx: %d", i);
    return;
  }
  ClientInfo* client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_REQ) { // request (from a Known admin client!)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    if (timestamp > client->last_timestamp) { // prevent replay attacks
      int reply_len = handleRequest(client, timestamp, &data[4], len - 4);
      if (reply_len == 0) return; // invalid command

      client->last_timestamp = timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      if (packet->isRouteFlood()) {
        // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
        mesh::Packet *path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                              PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
        if (path) sendFlood(path, SERVER_RESPONSE_DELAY);
      } else {
        mesh::Packet *reply =
            createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
        if (reply) {
          if (client->out_path_len >= 0) { // we have an out_path, so send DIRECT
            sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
          } else {
            sendFlood(reply, SERVER_RESPONSE_DELAY);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  } else if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && client->isAdmin()) { // a CLI command
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    uint8_t flags = (data[4] >> 2);        // message attempt number, and other flags

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported text type received: flags=%02x", (uint32_t)flags);
    } else if (sender_timestamp >= client->last_timestamp) { // prevent replay attacks
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      // len can be > original length, but 'text' will be padded with zeroes
      data[len] = 0; // need to make a C string again, with null terminator

#ifdef WITH_WEB_INTERFACE
      // Capture the incoming message for the web interface inbox.
      {
        char sender_hex[9];
        snprintf(sender_hex, sizeof(sender_hex), "%02X%02X%02X%02X",
                 client->id.pub_key[0], client->id.pub_key[1],
                 client->id.pub_key[2], client->id.pub_key[3]);
        pushWebMsg(sender_hex, "Direct", (const char*)&data[5], false, packet->_snr, (uint8_t)packet->path_len);
      }
#endif

      if (flags == TXT_TYPE_PLAIN) { // for legacy CLI, send Acks
        uint32_t ack_hash; // calc truncated hash of the message timestamp + text + sender pub_key, to prove
                           // to sender that we got it
        mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                            PUB_KEY_SIZE);

        mesh::Packet *ack = createAck(ack_hash);
        if (ack) {
          if (client->out_path_len < 0) {
            sendFlood(ack, TXT_ACK_DELAY);
          } else {
            sendDirect(ack, client->out_path, client->out_path_len, TXT_ACK_DELAY);
          }
        }
      }

      uint8_t temp[166];
      char *command = (char *)&data[5];
      char *reply = (char *)&temp[5];
      if (is_retry) {
        *reply = 0;
      } else {
        handleCommand(sender_timestamp, command, reply);
      }
      int text_len = strlen(reply);
      if (text_len > 0) {
        uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
        if (timestamp == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          timestamp++;
        }
        memcpy(temp, &timestamp, 4);        // mostly an extra blob to help make packet_hash unique
        temp[4] = (TXT_TYPE_CLI_DATA << 2); // NOTE: legacy was: TXT_TYPE_PLAIN

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        if (reply) {
          if (client->out_path_len < 0) {
            sendFlood(reply, CLI_REPLY_DELAY_MILLIS);
          } else {
            sendDirect(reply, client->out_path, client->out_path_len, CLI_REPLY_DELAY_MILLIS);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  }
}

bool MyMesh::onPeerPathRecv(mesh::Packet *packet, int sender_idx, const uint8_t *secret, uint8_t *path,
                            uint8_t path_len, uint8_t extra_type, uint8_t *extra, uint8_t extra_len) {
  // TODO: prevent replay attacks
  int i = matching_peer_indexes[sender_idx];

  if (i >= 0 && i < acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("PATH to client, path_len=%d", (uint32_t)path_len);
    auto client = acl.getClientByIdx(i);

    memcpy(client->out_path, path, client->out_path_len = path_len); // store a copy of path, for sendDirect()
    client->last_activity = getRTCClock()->getCurrentTime();
  } else {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: invalid peer idx: %d", i);
  }

  // NOTE: no reciprocal path send!!
  return false;
}

#define CTL_TYPE_NODE_DISCOVER_REQ   0x80
#define CTL_TYPE_NODE_DISCOVER_RESP  0x90

void MyMesh::onControlDataRecv(mesh::Packet* packet) {
  uint8_t type = packet->payload[0] & 0xF0;    // just test upper 4 bits
  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6
      && !_prefs.disable_fwd && discover_limiter.allow(rtc_clock.getCurrentTime())
  ) {
    int i = 1;
    uint8_t  filter = packet->payload[i++];
    uint32_t tag;
    memcpy(&tag, &packet->payload[i], 4); i += 4;
    uint32_t since;
    if (packet->payload_len >= i+4) {   // optional since field
      memcpy(&since, &packet->payload[i], 4); i += 4;
    } else {
      since = 0;
    }

    if ((filter & (1 << ADV_TYPE_REPEATER)) != 0 && _prefs.discovery_mod_timestamp >= since) {
      bool prefix_only = packet->payload[0] & 1;
      uint8_t data[6 + PUB_KEY_SIZE];
      data[0] = CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_REPEATER;   // low 4-bits for node type
      data[1] = packet->_snr;   // let sender know the inbound SNR ( x 4)
      memcpy(&data[2], &tag, 4);     // include tag from request, for client to match to
      memcpy(&data[6], self_id.pub_key, PUB_KEY_SIZE);
      auto resp = createControlData(data, prefix_only ? 6 + 8 : 6 + PUB_KEY_SIZE);
      if (resp) {
        sendZeroHop(resp, getRetransmitDelay(resp)*4);  // apply random delay (widened x4), as multiple nodes can respond to this
      }
    }
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      _cli(board, rtc, sensors, acl, &_prefs, this), telemetry(MAX_PACKET_PAYLOAD - 4), region_map(key_store), temp_map(key_store),
      discover_limiter(4, 120),  // max 4 every 2 minutes
      anon_limiter(4, 180)   // max 4 every 3 minutes
#if defined(WITH_RS232_BRIDGE)
      , bridge(&_prefs, WITH_RS232_BRIDGE, _mgr, &rtc)
#endif
#if defined(WITH_ESPNOW_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
#if defined(WITH_MQTT_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
{
  last_millis = 0;
  uptime_millis = 0;
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  set_radio_at = revert_radio_at = 0;
  _logging = false;
  region_load_active = false;
#ifdef WITH_WEB_INTERFACE
  _web_msgs = nullptr;
  _web = nullptr;
#endif

#if MAX_NEIGHBOURS
  memset(neighbours, 0, sizeof(neighbours));
#endif

  // defaults
  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.airtime_factor = 1.0;   // one half
  _prefs.rx_delay_base = 0.0f;   // turn off by default, was 10.0;
  _prefs.tx_delay_factor = 0.5f; // was 0.25f
  _prefs.direct_tx_delay_factor = 0.2f; // was zero
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.advert_interval = 1;        // default to 2 minutes for NEW installs
  _prefs.flood_advert_interval = 12; // 12 hours
  _prefs.flood_max = 64;
  _prefs.interference_threshold = 0; // disabled

  // bridge defaults
  _prefs.bridge_enabled  = 1;    // enabled
  _prefs.bridge_delay    = 500;  // milliseconds
  _prefs.bridge_pkt_src  = 0;    // logTx
  _prefs.bridge_baud     = 115200; // baud rate
  _prefs.bridge_channel  = 1;    // channel 1
  _prefs.mqtt_autostart  = 1;    // start MQTT bridge on boot (old prefs files keep this default)

  StrHelper::strncpy(_prefs.bridge_secret, "LVSITANOS", sizeof(_prefs.bridge_secret));

  // GPS defaults
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;

  _prefs.adc_multiplier = 0.0f; // 0.0f means use default board multiplier
}

void MyMesh::begin(FILESYSTEM *fs) {
  mesh::Mesh::begin();
  _fs = fs;
  // load persisted prefs
  _cli.loadPrefs(_fs);
  acl.load(_fs, self_id);
  // TODO: key_store.begin();
  region_map.load(_fs);

  // Load up to MAX_LISTEN_CHANNELS group-channel PSKs for GRP_TXT decryption.
  // Files: /chN.psk (hex key), /chN.name (optional friendly name).
  // Channel N is stored at index N — sparse; empty slots have all-zero secret/hash.
  // Legacy /channel_psk is migrated to /ch0.psk on first boot.
  _num_listen_channels = 0;
  memset(_listen_channels, 0, sizeof(_listen_channels));
  memset(_listen_channel_names, 0, sizeof(_listen_channel_names));
  {
    // Migrate old single-channel file to /ch0.psk if new slot file doesn't exist yet
    if (_fs->exists("/channel_psk") && !_fs->exists("/ch0.psk")) {
      File src = _fs->open("/channel_psk");
      File dst = _fs->open("/ch0.psk", "w", true);
      if (src && dst) {
        while (src.available()) dst.write(src.read());
      }
      if (src) src.close();
      if (dst) dst.close();
      _fs->remove("/channel_psk");
    }
    for (int i = 0; i < MAX_LISTEN_CHANNELS; i++) {
      char fname[12];
      snprintf(fname, sizeof(fname), "/ch%d.psk", i);
      File f = _fs->open(fname);
      if (!f) continue;
      char hex[65] = {};
      int n = f.read((uint8_t*)hex, 64);
      f.close();
      hex[64] = 0;
      while (n > 0 && (hex[n-1] == ' ' || hex[n-1] == '\r' || hex[n-1] == '\n')) n--;
      hex[n] = 0;
      int key_bytes = (n == 32) ? 16 : (n == 64) ? 32 : 0;
      mesh::GroupChannel& ch = _listen_channels[i];  // store at indexed position
      if (key_bytes > 0 && mesh::Utils::fromHex(ch.secret, key_bytes, hex)) {
        memset(ch.secret + key_bytes, 0, 32 - key_bytes);
        mesh::Utils::sha256(ch.hash, sizeof(ch.hash), ch.secret, key_bytes);
        if (i + 1 > _num_listen_channels) _num_listen_channels = i + 1;
        // Load optional friendly name
        char nfname[14];
        snprintf(nfname, sizeof(nfname), "/ch%d.name", i);
        File nf = _fs->open(nfname);
        if (nf) {
          int nn = nf.read((uint8_t*)_listen_channel_names[i], 31);
          nf.close();
          while (nn > 0 && (_listen_channel_names[i][nn-1] == ' ' ||
                            _listen_channel_names[i][nn-1] == '\r' ||
                            _listen_channel_names[i][nn-1] == '\n')) nn--;
          _listen_channel_names[i][nn] = 0;
        }
      }
    }
  }

#if defined(WITH_BRIDGE)
#if defined(WITH_MQTT_BRIDGE)
  bridge.begin();  // WiFi always auto-connects; MQTT follows mqtt_autostart
#else
  if (_prefs.bridge_enabled) {
    bridge.begin();
  }
#endif
#endif

#ifdef WITH_WEB_INTERFACE
  // Allocate message ring buffer from PSRAM if available, internal heap otherwise.
  _web_msgs = (WebMsg*)(psramFound()
    ? ps_malloc(WEB_MSG_BUF * sizeof(WebMsg))
    :    malloc(WEB_MSG_BUF * sizeof(WebMsg)));
  if (_web_msgs) memset(_web_msgs, 0, WEB_MSG_BUF * sizeof(WebMsg));

  _web_msg_seq   = 0;
  _web_msg_head  = 0;
  _web_msg_count = 0;
  _num_ch_stats  = 0;
  _web_msg_mux   = portMUX_INITIALIZER_UNLOCKED;
  memset(&_pending_send, 0, sizeof(_pending_send));
  _web = new WebInterface(this);
  _web->begin();
  // Start the HTTP server immediately if WiFi is already up (MQTT bridge brings
  // it up synchronously inside bridge.begin() above).
  if (WiFi.status() == WL_CONNECTED) {
    _web->onWiFiConnected();
  }
#endif

  radio_set_params(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_set_tx_power(_prefs.tx_power_dbm);

  updateAdvertTimer();
  updateFloodAdvertTimer();

  board.setAdcMultiplier(_prefs.adc_multiplier);

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000); // give CLI reply some time to be sent back, before applying temp radio params
  pending_freq = freq;
  pending_bw = bw;
  pending_sf = sf;
  pending_cr = cr;

  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000); // schedule when to revert radio params
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  mesh::Packet *pkt = createSelfAdvert();
  if (pkt) {
    if (flood) {
      sendFlood(pkt, delay_millis);
    } else {
      sendZeroHop(pkt, delay_millis);
    }
  } else {
    MESH_DEBUG_PRINTLN("ERROR: unable to create advertisement packet!");
  }
}

void MyMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) { // schedule local advert timer
    next_local_advert = futureMillis(((uint32_t)_prefs.advert_interval) * 2 * 60 * 1000);
  } else {
    next_local_advert = 0; // stop the timer
  }
}

void MyMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) { // schedule flood advert timer
    next_flood_advert = futureMillis(((uint32_t)_prefs.flood_advert_interval) * 60 * 60 * 1000);
  } else {
    next_flood_advert = 0; // stop the timer
  }
}

void MyMesh::dumpLogFile() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PACKET_LOG_FILE, "r");
#else
  File f = _fs->open(PACKET_LOG_FILE);
#endif
  if (f) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      Serial.print((char)c);
    }
    f.close();
  }
}

void MyMesh::setTxPower(int8_t power_dbm) {
  radio_set_tx_power(power_dbm);
}

void MyMesh::formatNeighborsReply(char *reply) {
  char *dp = reply;

#if MAX_NEIGHBOURS
  // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    auto neighbour = &neighbours[i];
    // Use pub_key check rather than heard_timestamp > 0 so neighbors show even when RTC is unsynced.
    const uint8_t* pk = neighbour->id.pub_key;
    if (pk[0] || pk[1] || pk[2] || pk[3]) {
      sorted_neighbours[neighbours_count] = neighbour;
      neighbours_count++;
    }
  }

  // sort neighbours newest to oldest
  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp; // desc
  });

  for (int i = 0; i < neighbours_count && dp - reply < 134; i++) {
    NeighbourInfo *neighbour = sorted_neighbours[i];

    // add new line if not first item
    if (i > 0) *dp++ = '\n';

    char hex[10];
    // get 4 bytes of neighbour id as hex
    mesh::Utils::toHex(hex, neighbour->id.pub_key, 4);

    // add next neighbour
    uint32_t secs_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
    sprintf(dp, "%s:%d:%d", hex, secs_ago, neighbour->snr);
    while (*dp)
      dp++; // find end of string
  }
#endif
  if (dp == reply) { // no neighbours, need empty response
    strcpy(dp, "-none-");
    dp += 6;
  }
  *dp = 0; // null terminator
}

void MyMesh::removeNeighbor(const uint8_t *pubkey, int key_len) {
#if MAX_NEIGHBOURS
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    NeighbourInfo *neighbour = &neighbours[i];
    if (memcmp(neighbour->id.pub_key, pubkey, key_len) == 0) {
      neighbours[i] = NeighbourInfo(); // clear neighbour entry
    }
  }
#endif
}

void MyMesh::formatStatsReply(char *reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}

void MyMesh::formatRadioStatsReply(char *reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char *reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(), 
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply) {
  if (region_load_active) {
    if (StrHelper::isBlank(command)) {  // empty/blank line, signal to terminate 'load' operation
      region_map = temp_map;  // copy over the temp instance as new current map
      region_load_active = false;

      sprintf(reply, "OK - loaded %d regions", region_map.getCount());
    } else {
      char *np = command;
      while (*np == ' ') np++;   // skip indent
      int indent = np - command;

      char *ep = np;
      while (RegionMap::is_name_char(*ep)) ep++;
      if (*ep) { *ep++ = 0; }  // set null terminator for end of name

      while (*ep && *ep != 'F') ep++;  // look for (optional) flags

      if (indent > 0 && indent < 8 && strlen(np) > 0) {
        auto parent = load_stack[indent - 1];
        if (parent) {
          auto old = region_map.findByName(np);
          auto nw = temp_map.putRegion(np, parent->id, old ? old->id : 0);  // carry-over the current ID (if name already exists)
          if (nw) {
            nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);   // carry-over flags from curr

            load_stack[indent] = nw;  // keep pointers to parent regions, to resolve parent_id's
          }
        }
      }
      reply[0] = 0;
    }
    return;
  }

  while (*command == ' ') command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // handle ACL related commands
  if (memcmp(command, "setperm ", 8) == 0) {   // format:  setperm {pubkey-hex} {permissions-int8}
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');   // look for separator char
    if (sp == NULL) {
      strcpy(reply, "Err - bad params");
    } else {
      *sp++ = 0;   // replace space with null terminator

      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min(sp - hex, PUB_KEY_SIZE*2);
      if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
        uint8_t perms = atoi(sp);
        if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
          dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);   // trigger acl.save()
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - invalid params");
        }
      } else {
        strcpy(reply, "Err - bad pubkey");
      }
    }
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL:");
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;  // skip deleted (or guest) entries

      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else if (memcmp(command, "region", 6) == 0) {
    reply[0] = 0;

    const char* parts[4];
    int n = mesh::Utils::parseTextParts(command, parts, 4, ' ');
    if (n == 1) {
      region_map.exportTo(reply, 160);
    } else if (n >= 2 && strcmp(parts[1], "load") == 0) {
      temp_map.resetFrom(region_map);   // rebuild regions in a temp instance
      memset(load_stack, 0, sizeof(load_stack));
      load_stack[0] = &temp_map.getWildcard();
      region_load_active = true;
    } else if (n >= 2 && strcmp(parts[1], "save") == 0) {
      _prefs.discovery_mod_timestamp = rtc_clock.getCurrentTime();   // this node is now 'modified' (for discovery info)
      savePrefs();
      bool success = region_map.save(_fs);
      strcpy(reply, success ? "OK" : "Err - save failed");
    } else if (n >= 3 && strcmp(parts[1], "allowf") == 0) {
      auto region = region_map.findByNamePrefix(parts[2]);
      if (region) {
        region->flags &= ~REGION_DENY_FLOOD;
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n >= 3 && strcmp(parts[1], "denyf") == 0) {
      auto region = region_map.findByNamePrefix(parts[2]);
      if (region) {
        region->flags |= REGION_DENY_FLOOD;
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n >= 3 && strcmp(parts[1], "get") == 0) {
      auto region = region_map.findByNamePrefix(parts[2]);
      if (region) {
        auto parent = region_map.findById(region->parent);
        if (parent && parent->id != 0) {
          sprintf(reply, " %s (%s) %s", region->name, parent->name, (region->flags & REGION_DENY_FLOOD) ? "" : "F");
        } else {
          sprintf(reply, " %s %s", region->name, (region->flags & REGION_DENY_FLOOD) ? "" : "F");
        }
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n >= 3 && strcmp(parts[1], "home") == 0) {
      auto home = region_map.findByNamePrefix(parts[2]);
      if (home) {
        region_map.setHomeRegion(home);
        sprintf(reply, " home is now %s", home->name);
      } else {
        strcpy(reply, "Err - unknown region");
      }
    } else if (n == 2 && strcmp(parts[1], "home") == 0) {
      auto home = region_map.getHomeRegion();
      sprintf(reply, " home is %s", home ? home->name : "*");
    } else if (n >= 3 && strcmp(parts[1], "put") == 0) {
      auto parent = n >= 4 ? region_map.findByNamePrefix(parts[3]) : &region_map.getWildcard();
      if (parent == NULL) {
        strcpy(reply, "Err - unknown parent");
      } else {
        auto region = region_map.putRegion(parts[2], parent->id);
        if (region == NULL) {
          strcpy(reply, "Err - unable to put");
        } else {
          strcpy(reply, "OK");
        }
      }
    } else if (n >= 3 && strcmp(parts[1], "remove") == 0) {
      auto region = region_map.findByName(parts[2]);
      if (region) {
        if (region_map.removeRegion(*region)) {
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - not empty");
        }
      } else {
        strcpy(reply, "Err - not found");
      }
    } else if (n >= 3 && strcmp(parts[1], "list") == 0) {
      uint8_t mask = 0;
      bool invert = false;
      
      if (strcmp(parts[2], "allowed") == 0) {
        mask = REGION_DENY_FLOOD;
        invert = false;  // list regions that DON'T have DENY flag
      } else if (strcmp(parts[2], "denied") == 0) {
        mask = REGION_DENY_FLOOD;
        invert = true;   // list regions that DO have DENY flag
      } else {
        strcpy(reply, "Err - use 'allowed' or 'denied'");
        return;
      }
      
      int len = region_map.exportNamesTo(reply, 160, mask, invert);
      if (len == 0) {
        strcpy(reply, "-none-");
      }
    } else {
      strcpy(reply, "Err - ??");
    }
  } else if (strncmp(command, "set channel.", 12) == 0) {
    // set channel.psk <hex>        alias for channel.0.psk
    // set channel.N.psk <hex>      N = 0..3
    // set channel.N.name <text>    friendly name for channel N
    const char* rest = command + 12;
    int ch_idx = 0;
    const char* sub;
    if (rest[0] >= '0' && rest[0] <= '3' && rest[1] == '.') {
      ch_idx = rest[0] - '0';
      sub    = rest + 2;
    } else {
      ch_idx = 0;
      sub    = rest;
    }
    if (strncmp(sub, "psk ", 4) == 0) {
      char hex[65] = {};
      strncpy(hex, sub + 4, 64);
      int hlen = strlen(hex);
      while (hlen > 0 && (hex[hlen-1] == ' ' || hex[hlen-1] == '\r' || hex[hlen-1] == '\n'))
        hex[--hlen] = 0;
      int key_bytes = (hlen == 32) ? 16 : (hlen == 64) ? 32 : 0;
      if (key_bytes == 0) {
        strcpy(reply, "Err - need 32 hex chars (16-byte) or 64 hex chars (32-byte)");
      } else if (ch_idx >= MAX_LISTEN_CHANNELS) {
        strcpy(reply, "Err - channel index out of range");
      } else {
        mesh::GroupChannel& ch = _listen_channels[ch_idx];
        if (!mesh::Utils::fromHex(ch.secret, key_bytes, hex)) {
          strcpy(reply, "Err - invalid hex");
        } else {
          memset(ch.secret + key_bytes, 0, 32 - key_bytes);
          mesh::Utils::sha256(ch.hash, sizeof(ch.hash), ch.secret, key_bytes);
          if (ch_idx >= _num_listen_channels) _num_listen_channels = ch_idx + 1;
          char fname[12];
          snprintf(fname, sizeof(fname), "/ch%d.psk", ch_idx);
          File f = _fs->open(fname, "w", true);
          if (f) { f.write((const uint8_t*)hex, hlen); f.close(); strcpy(reply, "OK"); }
          else    { strcpy(reply, "Err - failed to save"); }
        }
      }
    } else if (strncmp(sub, "name ", 5) == 0) {
      const char* name = sub + 5;
      if (ch_idx >= MAX_LISTEN_CHANNELS || _listen_channels[ch_idx].hash[0] == 0) {
        strcpy(reply, "Err - channel not configured (set PSK first)");
      } else {
        strncpy(_listen_channel_names[ch_idx], name, 31);
        _listen_channel_names[ch_idx][31] = 0;
        char nfname[14];
        snprintf(nfname, sizeof(nfname), "/ch%d.name", ch_idx);
        File f = _fs->open(nfname, "w", true);
        if (f) { f.write((const uint8_t*)name, strlen(name)); f.close(); strcpy(reply, "OK"); }
        else    { strcpy(reply, "Err - failed to save"); }
      }
    } else {
      strcpy(reply, "Err - unknown channel subcommand (use psk or name)");
    }
  } else if (strncmp(command, "get channel.", 12) == 0) {
    // get channel.psk / get channel.N.psk / get channel.N.name
    const char* rest = command + 12;
    int ch_idx = 0;
    const char* sub;
    if (rest[0] >= '0' && rest[0] <= '3' && rest[1] == '.') {
      ch_idx = rest[0] - '0';
      sub    = rest + 2;
    } else {
      ch_idx = 0;
      sub    = rest;
    }
    if (strcmp(sub, "psk") == 0) {
      if (ch_idx >= MAX_LISTEN_CHANNELS || _listen_channels[ch_idx].hash[0] == 0) {
        strcpy(reply, "(not set)");
      } else {
        mesh::Utils::toHex(reply, _listen_channels[ch_idx].secret, 32);
      }
    } else if (strcmp(sub, "name") == 0) {
      if (ch_idx >= MAX_LISTEN_CHANNELS || _listen_channels[ch_idx].hash[0] == 0) {
        strcpy(reply, "(channel not configured)");
      } else if (_listen_channel_names[ch_idx][0] == 0) {
        strcpy(reply, "(not named)");
      } else {
        strcpy(reply, _listen_channel_names[ch_idx]);
      }
    } else {
      strcpy(reply, "Err - unknown channel subcommand (use psk or name)");
    }
  } else if (strncmp(command, "clear channel.", 14) == 0) {
    // clear channel.psk / clear channel.N.psk / clear channel.N.name
    const char* rest = command + 14;
    int ch_idx = 0;
    const char* sub;
    if (rest[0] >= '0' && rest[0] <= '3' && rest[1] == '.') {
      ch_idx = rest[0] - '0';
      sub    = rest + 2;
    } else {
      ch_idx = 0;
      sub    = rest;
    }
    if (strcmp(sub, "psk") == 0) {
      if (ch_idx >= MAX_LISTEN_CHANNELS) {
        strcpy(reply, "Err - index out of range");
      } else {
        memset(&_listen_channels[ch_idx], 0, sizeof(mesh::GroupChannel));
        char fname[12];
        snprintf(fname, sizeof(fname), "/ch%d.psk", ch_idx);
        _fs->remove(fname);
        while (_num_listen_channels > 0) {
          int top = _num_listen_channels - 1;
          bool zero = true;
          for (int b = 0; b < 32; b++) { if (_listen_channels[top].secret[b]) { zero = false; break; } }
          if (zero) _num_listen_channels--;
          else break;
        }
        strcpy(reply, "OK");
      }
    } else if (strcmp(sub, "name") == 0) {
      if (ch_idx < MAX_LISTEN_CHANNELS) {
        _listen_channel_names[ch_idx][0] = 0;
        char nfname[14];
        snprintf(nfname, sizeof(nfname), "/ch%d.name", ch_idx);
        _fs->remove(nfname);
      }
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - unknown channel subcommand (use psk or name)");
    }
  } else {
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif

#ifdef WITH_WEB_INTERFACE
  // Process a pending send-text request queued by the web interface.
  if (_pending_send.pending) {
    _pending_send.pending = false;  // clear first so the web handler can enqueue again
    if (_pending_send.to_all) {
      sendTextToAllClients(_pending_send.text);
    } else if (strncmp(_pending_send.target_hex, "CHANNEL", 7) == 0) {
      int ch = (_pending_send.target_hex[7] >= '0' && _pending_send.target_hex[7] <= '3')
               ? (_pending_send.target_hex[7] - '0') : 0;
      sendChannelText(ch, _pending_send.text, _pending_send.region[0] ? _pending_send.region : nullptr);
    } else {
      // Decode 4-byte pub key prefix from the stored 8-char hex string.
      uint8_t pubkey4[4] = {};
      for (int j = 0; j < 4; j++) {
        char bs[3] = { _pending_send.target_hex[j * 2], _pending_send.target_hex[j * 2 + 1], 0 };
        pubkey4[j] = (uint8_t)strtol(bs, nullptr, 16);
      }
      sendTextToClient(pubkey4, _pending_send.text);
    }
  }
  if (_web) _web->loop();
#endif

  mesh::Mesh::loop();

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendFlood(pkt);

    updateFloodAdvertTimer(); // schedule next flood advert
    updateAdvertTimer();      // also schedule local advert (so they don't overlap)
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);

    updateAdvertTimer(); // schedule next local advert
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) { // apply pending (temporary) radio params
    set_radio_at = 0;                                     // clear timer
    radio_set_params(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) { // revert radio params to orig
    revert_radio_at = 0;                                        // clear timer
    radio_set_params(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  // is pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs);
    dirty_contacts_expiry = 0;
  }

  // update uptime
  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;
}

// To check if there is pending work
bool MyMesh::hasPendingWork() const {
#if defined(WITH_BRIDGE)
  if (bridge.isRunning()) return true;  // bridge needs WiFi radio, can't sleep
#endif
  return _mgr->getOutboundCount(0xFFFFFFFF) > 0;
}

// =============================================================================
// Group-channel support — allows the repeater to decode GRP_TXT messages
// =============================================================================

int MyMesh::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels_out[], int max_matches) {
  int found = 0;
  for (int i = 0; i < _num_listen_channels && found < max_matches; i++) {
    // Skip empty slots (no PSK configured at this index)
    if (_listen_channels[i].hash[0] == 0 && _listen_channels[i].hash[1] == 0) continue;
    if (_listen_channels[i].hash[0] == hash[0]) {
      channels_out[found++] = _listen_channels[i];
    }
  }
#ifdef WITH_WEB_INTERFACE
  {
    // Update channel activity counters for every GRP_TXT, regardless of PSK.
    char hash_hex[3]; snprintf(hash_hex, sizeof(hash_hex), "%02X", hash[0]);
    int8_t snr_x4 = (int8_t)(radio_driver.getLastSNR() * 4.0f);
    updateChannelStat(_ch_stats, _num_ch_stats, hash_hex, "", found > 0, snr_x4);
    if (found == 0) {
      // Traffic visible in log but no matching PSK — show a placeholder.
      char ch_tag[9]; snprintf(ch_tag, sizeof(ch_tag), "CH%02X", hash[0]);
      pushWebMsg("?", ch_tag, "(encrypted - PSK not configured)", false, snr_x4, 0);
    }
  }
#endif
  return found;
}

void MyMesh::onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel,
                              uint8_t* data, size_t len) {
#ifdef WITH_WEB_INTERFACE
  if (type == PAYLOAD_TYPE_GRP_TXT && len > 5) {
    uint8_t txt_type = data[4];
    if ((txt_type >> 2) == 0) {  // plain text
      data[len] = 0;  // null-terminate; buffer has room (MAX_PACKET_PAYLOAD)

      // Build a channel tag: "Name [8B]" if named, else "CH8B"
      char ch_tag[16] = {};
      bool named = false;
      for (int i = 0; i < _num_listen_channels; i++) {
        if (_listen_channels[i].hash[0] == channel.hash[0] && _listen_channel_names[i][0]) {
          snprintf(ch_tag, sizeof(ch_tag), "%.11s[%02X]", _listen_channel_names[i], channel.hash[0]);
          named = true;
          break;
        }
      }
      if (!named) snprintf(ch_tag, sizeof(ch_tag), "CH%02X", channel.hash[0]);

      // Update channel activity stats with the name discovered at decode time.
      {
        char hash_hex[3]; snprintf(hash_hex, sizeof(hash_hex), "%02X", channel.hash[0]);
        const char* stat_name = named ? _listen_channel_names[0] : "";  // best-effort
        for (int i = 0; i < _num_listen_channels; i++) {
          if (_listen_channels[i].hash[0] == channel.hash[0] && _listen_channel_names[i][0]) {
            stat_name = _listen_channel_names[i]; break;
          }
        }
        updateChannelStat(_ch_stats, _num_ch_stats, hash_hex, stat_name, true, packet->_snr);
      }

      // Decoded payload is "SenderName: MessageText" starting at data[5].
      const char* full = (const char*)&data[5];
      const char* sep = strstr(full, ": ");
      char sender[9];
      const char* msg;
      if (sep && sep > full) {
        int nlen = (int)(sep - full);
        if (nlen > 8) nlen = 8;
        memcpy(sender, full, nlen);
        sender[nlen] = 0;
        msg = sep + 2;
      } else {
        snprintf(sender, sizeof(sender), "CH%02X", channel.hash[0]);
        msg = full;
      }
      pushWebMsg(sender, ch_tag, msg, false, packet->_snr, (uint8_t)packet->path_len);
    }
  }
#endif
}

// =============================================================================
// Web interface support methods (compiled only when WITH_WEB_INTERFACE=1)
// =============================================================================

#ifdef WITH_WEB_INTERFACE

// Return info about a channel slot for the web API.
// Returns false if the slot has no PSK configured.
bool MyMesh::getChannelInfo(int idx, char hash_hex_out[3], char name_out[32]) const {
  if (idx < 0 || idx >= MAX_LISTEN_CHANNELS) return false;
  if (_listen_channels[idx].hash[0] == 0 && _listen_channels[idx].hash[1] == 0) return false;
  snprintf(hash_hex_out, 3, "%02X", _listen_channels[idx].hash[0]);
  strncpy(name_out, _listen_channel_names[idx], 31);
  name_out[31] = 0;
  return true;
}

// Push a message into the ring buffer.  Safe to call from the main Arduino task;
// the buffer is protected by a portMUX spinlock so the AsyncWebServer task can
// read it concurrently.
void MyMesh::pushWebMsg(const char* sender_hex, const char* channel_tag, const char* text, bool outbound, int8_t snr, uint8_t hops) {
  if (!_web_msgs) return;
  portENTER_CRITICAL(&_web_msg_mux);
  WebMsg& m = _web_msgs[_web_msg_head];
  m.seq       = _web_msg_seq++;
  m.timestamp = getRTCClock()->getCurrentTime();
  strncpy(m.sender_hex,   sender_hex,   sizeof(m.sender_hex)   - 1); m.sender_hex[sizeof(m.sender_hex)-1]     = 0;
  strncpy(m.channel_tag,  channel_tag,  sizeof(m.channel_tag)  - 1); m.channel_tag[sizeof(m.channel_tag)-1]   = 0;
  strncpy(m.text,         text,         sizeof(m.text)         - 1); m.text[sizeof(m.text)-1]                 = 0;
  m.outbound  = outbound;
  m.snr       = snr;
  m.hops      = hops;
  _web_msg_head = (_web_msg_head + 1) % WEB_MSG_BUF;
  if (_web_msg_count < WEB_MSG_BUF) _web_msg_count++;
  portEXIT_CRITICAL(&_web_msg_mux);
}

// Update per-channel-hash activity counters.  Called from both the PSK-matched and
// no-PSK paths so the Dashboard always shows live traffic regardless of key config.
static void updateChannelStat(ChannelStat stats[], int& num_stats, const char* hash_hex,
                               const char* name, bool has_psk, int8_t snr_x4) {
  for (int i = 0; i < num_stats; i++) {
    if (strcmp(stats[i].hash_hex, hash_hex) == 0) {
      stats[i].pkt_count++;
      stats[i].last_millis = (uint32_t)millis();
      stats[i].snr_sum    += snr_x4;
      stats[i].snr_count++;
      if (has_psk) stats[i].has_psk = true;  // upgrade if PSK was later configured
      if (name[0] && !stats[i].name[0]) {    // fill in name once we have it (decoded)
        strncpy(stats[i].name, name, sizeof(stats[i].name) - 1);
        stats[i].name[sizeof(stats[i].name) - 1] = 0;
      }
      return;
    }
  }
  if (num_stats < MAX_CHANNEL_STATS) {
    ChannelStat& s = stats[num_stats++];
    strncpy(s.hash_hex, hash_hex, sizeof(s.hash_hex) - 1); s.hash_hex[2] = 0;
    strncpy(s.name,     name,     sizeof(s.name)     - 1); s.name[sizeof(s.name)-1] = 0;
    s.pkt_count  = 1;
    s.last_millis = (uint32_t)millis();
    s.snr_sum    = snr_x4;
    s.snr_count  = 1;
    s.has_psk    = has_psk;
  }
}

int MyMesh::getChannelStats(ChannelStat* out, int maxCount) {
  int n = _num_ch_stats < maxCount ? _num_ch_stats : maxCount;
  memcpy(out, _ch_stats, n * sizeof(ChannelStat));
  return n;
}

// Copy up to maxCount messages with seq >= since into 'out'.
// Called from the AsyncWebServer task — uses the same spinlock.
int MyMesh::getWebMsgsSince(uint32_t since, WebMsg* out, int maxCount) {
  if (!_web_msgs) return 0;
  portENTER_CRITICAL(&_web_msg_mux);
  // The oldest message is at (_web_msg_head - _web_msg_count) mod WEB_MSG_BUF.
  int start = (_web_msg_head - _web_msg_count + WEB_MSG_BUF * 2) % WEB_MSG_BUF;
  int found = 0;
  for (int i = 0; i < _web_msg_count && found < maxCount; i++) {
    int idx = (start + i) % WEB_MSG_BUF;
    if (_web_msgs[idx].seq >= since) {
      out[found++] = _web_msgs[idx];
    }
  }
  portEXIT_CRITICAL(&_web_msg_mux);
  return found;
}

// Return up to maxCount known ACL contacts.
int MyMesh::getWebContacts(WebContact* out, int maxCount) {
  int n = acl.getNumClients();
  if (n > maxCount) n = maxCount;
  for (int i = 0; i < n; i++) {
    ClientInfo* c = acl.getClientByIdx(i);
    snprintf(out[i].id_hex, sizeof(out[i].id_hex), "%02X%02X%02X%02X",
             c->id.pub_key[0], c->id.pub_key[1],
             c->id.pub_key[2], c->id.pub_key[3]);
    out[i].last_activity = c->last_activity;
  }
  return n;
}

// Copy neighbor table into caller-supplied buffer.
int MyMesh::getNeighborsCopy(NeighbourInfo* out, int maxCount) {
#if MAX_NEIGHBOURS
  int found = 0;
  for (int i = 0; i < MAX_NEIGHBOURS && found < maxCount; i++) {
    // Detect populated slot by checking pub_key instead of heard_timestamp,
    // so neighbors are still visible when the RTC is not yet synchronized (returns 0).
    const uint8_t* pk = neighbours[i].id.pub_key;
    if (pk[0] || pk[1] || pk[2] || pk[3]) {
      out[found++] = neighbours[i];
    }
  }
  return found;
#else
  return 0;
#endif
}

// Gather stats from radio/mesh drivers into simple int fields.
void MyMesh::fillRepeaterStats(int* packets_recv, int* packets_sent,
                                int* last_snr,     int* last_rssi,
                                int* battery_mv,   uint32_t* uptime_secs) {
  *packets_recv = (int)radio_driver.getPacketsRecv();
  *packets_sent = (int)radio_driver.getPacketsSent();
  *last_snr     = (int16_t)(radio_driver.getLastSNR() * 4);
  *last_rssi    = (int16_t)radio_driver.getLastRSSI();
  *battery_mv   = (int)board.getBattMilliVolts();
  *uptime_secs  = (uint32_t)(uptime_millis / 1000ULL);
}

// Queue a send-text request.  Called from the AsyncWebServer task; processed
// by loop() on the main Arduino task.
void MyMesh::queueSendText(bool to_all, const char* target_hex, const char* text, const char* region) {
  if (_pending_send.pending) return;  // drop if a send is already queued
  _pending_send.to_all = to_all;
  strncpy(_pending_send.target_hex, target_hex, sizeof(_pending_send.target_hex) - 1);
  _pending_send.target_hex[sizeof(_pending_send.target_hex) - 1] = 0;
  strncpy(_pending_send.text, text, sizeof(_pending_send.text) - 1);
  _pending_send.text[sizeof(_pending_send.text) - 1] = 0;
  if (region) {
    strncpy(_pending_send.region, region, sizeof(_pending_send.region) - 1);
    _pending_send.region[sizeof(_pending_send.region) - 1] = 0;
  } else {
    _pending_send.region[0] = 0;
  }
  _pending_send.pending = true;  // set last — acts as publish barrier
}

// Send a plain-text TXT_MSG to the ACL client whose pub key prefix matches
// pubkey4[0..3].  Pushes an outbound WebMsg on success.
bool MyMesh::sendTextToClient(const uint8_t* pubkey4, const char* text) {
  for (int i = 0; i < acl.getNumClients(); i++) {
    ClientInfo* c = acl.getClientByIdx(i);
    if (memcmp(c->id.pub_key, pubkey4, 4) != 0) continue;

    int text_len = (int)strlen(text);
    if (text_len > WEB_MAX_TEXT_LEN) text_len = WEB_MAX_TEXT_LEN;

    uint8_t temp[5 + WEB_MAX_TEXT_LEN + 1];
    uint32_t ts = getRTCClock()->getCurrentTimeUnique();
    memcpy(temp, &ts, 4);
    temp[4] = (TXT_TYPE_PLAIN << 2);  // plain text (0x00)
    memcpy(&temp[5], text, text_len);
    temp[5 + text_len] = 0;

    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, c->id, c->shared_secret,
                              temp, 5 + text_len);
    if (!pkt) return false;

    if (c->out_path_len >= 0) {
      sendDirect(pkt, c->out_path, c->out_path_len, 0);
    } else {
      sendFlood(pkt, (uint32_t)0);
    }

    const char* sender = _prefs.sender_name[0] ? _prefs.sender_name : _prefs.node_name;
    pushWebMsg(sender, "Direct", text, /*outbound=*/true);
    return true;
  }
  return false;  // contact not found
}

// Broadcast a plain-text message to every active ACL client.
bool MyMesh::sendTextToAllClients(const char* text) {
  bool any = false;
  for (int i = 0; i < acl.getNumClients(); i++) {
    ClientInfo* c = acl.getClientByIdx(i);
    if (c->last_activity == 0) continue;  // skip clients that never connected

    int text_len = (int)strlen(text);
    if (text_len > WEB_MAX_TEXT_LEN) text_len = WEB_MAX_TEXT_LEN;

    uint8_t temp[5 + WEB_MAX_TEXT_LEN + 1];
    uint32_t ts = getRTCClock()->getCurrentTimeUnique();
    memcpy(temp, &ts, 4);
    temp[4] = (TXT_TYPE_PLAIN << 2);
    memcpy(&temp[5], text, text_len);
    temp[5 + text_len] = 0;

    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, c->id, c->shared_secret,
                              temp, 5 + text_len);
    if (pkt) {
      sendFlood(pkt, (uint32_t)0);
      any = true;
    }
  }
  if (any) {
    const char* sender = _prefs.sender_name[0] ? _prefs.sender_name : _prefs.node_name;
    pushWebMsg(sender, "Flood", text, /*outbound=*/true);
  }
  return any;
}

// Return comma-separated list of all configured region names for the web UI.
void MyMesh::getRegionsList(char* buf, int maxLen) {
  region_map.exportNamesTo(buf, maxLen, 0);
}

// Internal helper: sends pkt as a flood, optionally scoped to a named region.
// If region_name is non-empty and found, computes the auto transport codes and
// calls the transport-code overload of sendFlood.  Falls back to plain flood on
// any failure (unknown region, null key).
static void doScopedFlood(mesh::Mesh* mesh, RegionMap& region_map,
                          TransportKeyStore& key_store,
                          mesh::Packet* pkt, const char* region_name, uint32_t delay) {
  if (!region_name || !region_name[0]) {
    mesh->sendFlood(pkt, delay);
    return;
  }
  RegionEntry* region = region_map.findByNamePrefix(region_name);
  if (!region) {
    mesh->sendFlood(pkt, delay);
    return;
  }
  TransportKey key;
  if (region->name[0] == '#') {
    key_store.getAutoKeyFor(region->id, region->name, key);
  } else {
    // Implicit hashtag region: the key is derived from "#name".
    char tmp[32] = {};
    tmp[0] = '#';
    strncpy(&tmp[1], region->name, 30);
    key_store.getAutoKeyFor(region->id, tmp, key);
  }
  if (key.isNull()) {
    mesh->sendFlood(pkt, delay);
    return;
  }
  uint16_t codes[2] = { key.calcTransportCode(pkt), 0 };
  mesh->sendFlood(pkt, codes, delay);
}

// Send a group text message on channel ch_idx (GRP_TXT flood).
// The on-air format is: timestamp(4) + txt_type(1) + "NodeName: text"
bool MyMesh::sendChannelText(int ch_idx, const char* text, const char* region) {
  if (ch_idx < 0 || ch_idx >= _num_listen_channels) return false;
  int text_len = (int)strlen(text);
  if (text_len == 0 || text_len > WEB_MAX_TEXT_LEN) return false;

  // Build: timestamp(4) + txt_type(1) + "SenderName: text"
  const char* sender = _prefs.sender_name[0] ? _prefs.sender_name : _prefs.node_name;
  uint8_t temp[5 + 32 + WEB_MAX_TEXT_LEN + 1];
  uint32_t ts = getRTCClock()->getCurrentTimeUnique();
  memcpy(temp, &ts, 4);
  temp[4] = 0;  // txt_type byte: attempt=0, plain text

  int prefix_len = snprintf((char*)&temp[5], 33, "%s: ", sender);
  if (prefix_len < 0 || prefix_len > 32) prefix_len = 0;
  memcpy(&temp[5 + prefix_len], text, text_len + 1);

  auto pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, _listen_channels[ch_idx],
                                 temp, 5 + prefix_len + text_len);
  if (!pkt) return false;
  doScopedFlood(this, region_map, key_store, pkt, region, (uint32_t)0);
  // Build channel tag for outbound echo
  char ch_tag[16] = {};
  bool named = _listen_channel_names[ch_idx][0] != 0;
  if (named) snprintf(ch_tag, sizeof(ch_tag), "%.11s[%02X]", _listen_channel_names[ch_idx], _listen_channels[ch_idx].hash[0]);
  else       snprintf(ch_tag, sizeof(ch_tag), "CH%02X", _listen_channels[ch_idx].hash[0]);
  pushWebMsg(sender, ch_tag, text, /*outbound=*/true);
  return true;
}

#endif // WITH_WEB_INTERFACE
