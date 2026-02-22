#pragma once

#ifdef WITH_WEB_INTERFACE

#include <ESPAsyncWebServer.h>

class MyMesh;

class WebInterface {
  AsyncWebServer _server;
  MyMesh*        _mesh;
  bool           _started;

  // CLI command queue (main loop processes, handler waits)
  volatile bool _cli_pending;
  volatile bool _cli_done;
  char          _cli_cmd[160];
  char          _cli_result[160];

  bool checkAuth(AsyncWebServerRequest* req);
  void registerRoutes();

  // Route handlers
  void handleRoot(AsyncWebServerRequest* req);
  void handleStatus(AsyncWebServerRequest* req);
  void handleNeighbors(AsyncWebServerRequest* req);
  void handleMessages(AsyncWebServerRequest* req);
  void handlePostMessage(AsyncWebServerRequest* req,
                         uint8_t* data, size_t len, size_t index, size_t total);
  void handleCLI(AsyncWebServerRequest* req,
                 uint8_t* data, size_t len, size_t index, size_t total);
  void handleChannels(AsyncWebServerRequest* req);
  void handleRegions(AsyncWebServerRequest* req);
  void handleChannelStats(AsyncWebServerRequest* req);
  void handleMQTTStart(AsyncWebServerRequest* req);
  void handleMQTTStop(AsyncWebServerRequest* req);
  void handleLog(AsyncWebServerRequest* req);

public:
  WebInterface(MyMesh* mesh, uint16_t port = 80);

  // Register routes. Call once from MyMesh::begin().
  void begin();

  // Start the HTTP server after WiFi is connected.
  void onWiFiConnected();

  // Call from MyMesh::loop() — drains the CLI command queue.
  void loop();
};

#endif // WITH_WEB_INTERFACE
