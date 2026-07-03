#pragma once
// Off-target stub for ESPAsyncWebServer. The core declares an AsyncWebServer
// global (webServer) constructed with a port; the HTTP API lives in excluded
// TUs and is not exercised natively.
#include <Arduino.h>

class AsyncWebServerRequest {};
class AsyncWebServerResponse {};

class AsyncWebServer {
public:
  AsyncWebServer(int = 80) {}
  void begin() {}
  void end() {}
};
