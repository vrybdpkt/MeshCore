#pragma once

#include <helpers/ui/DisplayDriver.h>
#include <helpers/CommonCLI.h>

class UITask {
  DisplayDriver* _display;
  unsigned long _next_read, _next_refresh, _auto_off;
  int _prevBtnState;
  int _screen;           // 0 = home, 1 = MQTT stats (WITH_MQTT_BRIDGE only)
  NodePrefs* _node_prefs;
  char _version_info[32];
  char _bstat1[32];  // e.g. "WiFi:192.168.1.42"
  char _bstat2[32];  // e.g. "MQTT:connected"
#ifdef WITH_MQTT_BRIDGE
  uint32_t _stat_tx, _stat_rx, _stat_filtered, _stat_reconnects;
#endif

  void renderCurrScreen();
public:
  UITask(DisplayDriver& display) : _display(&display) {
    _next_read = _next_refresh = 0;
    _screen = 0;
    _bstat1[0] = _bstat2[0] = 0;
#ifdef WITH_MQTT_BRIDGE
    _stat_tx = _stat_rx = _stat_filtered = _stat_reconnects = 0;
#endif
  }
  void begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version);
  void setBridgeInfo(const char* s1, const char* s2);
#ifdef WITH_MQTT_BRIDGE
  void setMQTTStats(uint32_t tx, uint32_t rx, uint32_t filtered, uint32_t reconnects) {
    _stat_tx = tx; _stat_rx = rx; _stat_filtered = filtered; _stat_reconnects = reconnects;
  }
#endif

  void loop();
};