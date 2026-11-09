#pragma once
// Stub minimo do Arduino/ESP32 para rodar a logica do firmware no PC (tempo simulado).
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <cctype>
#include <functional>
#include <cstdint>
#include <algorithm>

#ifndef MILLIS_T
#define MILLIS_T unsigned long
#endif

struct String {
  std::string s;
  String() {}
  String(const char *c) : s(c) {}
  String(const std::string &x) : s(x) {}
  size_t length() const { return s.size(); }
  char operator[](size_t i) const { return s[i]; }
  const char *c_str() const { return s.c_str(); }
  int indexOf(const String &o) const { auto p = s.find(o.s); return p == std::string::npos ? -1 : (int)p; }
  void trim() {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
  }
  String operator+(const char *o) const { return String(s + o); }
  String operator+(const String &o) const { return String(s + o.s); }
  String &operator+=(char c) { s += c; return *this; }
  String &operator+=(const char *o) { s += o; return *this; }
  String &operator+=(const String &o) { s += o.s; return *this; }
};

// ---- tempo simulado ----
extern uint64_t g_ms;
extern std::function<void()> g_delayHook;  // usado para simular a varredura continua do codigo antigo
inline MILLIS_T millis() { return (MILLIS_T)g_ms; }
inline void delay(unsigned long ms) { g_ms += ms; if (g_delayHook) g_delayHook(); }

// ---- Serial ----
struct Cmd { uint64_t t; std::string text; };
extern std::vector<Cmd> g_cmds;
extern size_t g_nextCmd;
extern bool g_atLineStart;
extern bool g_quiet;
extern std::vector<std::string> g_serialLog;

struct HardwareSerial {
  std::string line;
  void emit(const char *s) {
    for (const char *p = s; *p; ++p) {
      line += *p;
      if (*p == '\n') {
        g_serialLog.push_back(line);
        if (!g_quiet) ::printf("[%9.3fs] %s", g_ms / 1000.0, line.c_str());
        line.clear();
      }
    }
  }
  void begin(long) {}
  void print(const char *s) { emit(s); }
  void println(const char *s) { emit(s); emit("\n"); }
  void println(const String &s) { emit(s.c_str()); emit("\n"); }
  void println() { emit("\n"); }
  int printf(const char *f, ...) __attribute__((format(printf, 2, 3))) {
    char buf[1024];
    va_list ap; va_start(ap, f);
    int n = vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    emit(buf);
    return n;
  }
  int available() { return (g_nextCmd < g_cmds.size() && g_cmds[g_nextCmd].t <= g_ms) ? 1 : 0; }
  String readStringUntil(char) { return String(g_cmds[g_nextCmd++].text); }
};
extern HardwareSerial Serial;

// ---- BLE ----
struct BLEUUID { std::string u; String toString() const { return String(u); } };
struct BLEAddress { std::string a; String toString() const { return String(a); } };
struct BLEAdvertisedDevice {
  std::string mac; int rssi = 0; std::string uuid;
  int getServiceDataCount() { return uuid.empty() ? 0 : 1; }
  BLEUUID getServiceDataUUID(int) { return BLEUUID{uuid}; }
  BLEAddress getAddress() { return BLEAddress{mac}; }
  int getRSSI() { return rssi; }
};
class BLEAdvertisedDeviceCallbacks {
 public:
  virtual ~BLEAdvertisedDeviceCallbacks() {}
  virtual void onResult(BLEAdvertisedDevice advertisedDevice) = 0;
};
struct BLEScanResults {};

struct Pkt { uint64_t t; std::string mac; int rssi; std::string uuid; };
extern std::vector<Pkt> g_pkts;
extern size_t g_nextPkt;
extern std::function<void()> g_pumpNet;
extern int g_scanCalls, g_clearCalls;

struct BLEScan {
  BLEAdvertisedDeviceCallbacks *cb = nullptr;
  BLEScanResults res;
  void setAdvertisedDeviceCallbacks(BLEAdvertisedDeviceCallbacks *c, bool = false, bool = true) { cb = c; }
  void setActiveScan(bool) {}
  void setInterval(int) {}
  void setWindow(int) {}
  // varredura continua (start(0, nullptr, false)): os pacotes chegam durante delay()
  bool start(uint32_t, void (*)(BLEScanResults), bool = false) { return true; }
  BLEScanResults *start(uint32_t duration, bool = false) {
    g_scanCalls++;
    uint64_t end = g_ms + duration * 1000ULL;
    int passo = 0;
    while (g_ms < end) {
      uint64_t alvo = std::min<uint64_t>(end, g_ms + 100);
      while (g_nextPkt < g_pkts.size() && g_pkts[g_nextPkt].t <= alvo) {
        Pkt &p = g_pkts[g_nextPkt++];
        if (p.t + 30 < g_ms) continue;  // chegou com a varredura parada: perdido
        if (p.t > g_ms) g_ms = p.t;
        BLEAdvertisedDevice d; d.mac = p.mac; d.rssi = p.rssi; d.uuid = p.uuid;
        cb->onResult(d);
      }
      g_ms = alvo;
      if ((++passo % 2) == 0 && g_pumpNet) g_pumpNet();  // a tarefa de rede acorda a cada ~200 ms
    }
    g_ms += 15;  // tempo para reiniciar a varredura
    return &res;
  }
  void clearResults() { g_clearCalls++; }
};
struct BLEDevice {
  static void init(const char *) {}
  static BLEScan *getScan() { static BLEScan s; return &s; }
};

// ---- FreeRTOS / ESP ----
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)0)
#define portEXIT_CRITICAL(m) ((void)0)
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }
extern TaskFunction_t g_taskFn;
extern void *g_taskArg;
extern int g_taskStack;
struct TaskYield {};
inline int xTaskCreatePinnedToCore(TaskFunction_t fn, const char *, uint32_t stack, void *arg, int, void *, int) {
  g_taskFn = fn; g_taskArg = arg; g_taskStack = (int)stack; return 1;
}
inline void vTaskDelay(unsigned long) { throw TaskYield(); }  // devolve o controle ao teste
#define pdMS_TO_TICKS(x) (x)

struct EspClass {
  uint64_t getEfuseMac() { return 0xAABBCCDDEEFFULL; }
  uint32_t getFreeHeap() { return 180000; }
  uint32_t getMinFreeHeap() { return 150000; }
  uint32_t getMaxAllocHeap() { return 90000; }
};
inline EspClass ESP;

// ---- Wi-Fi / HTTP / MQTT ----
extern std::function<bool(uint64_t)> g_wifiUp, g_mqttUp;
extern std::function<int(uint64_t)> g_httpCode;
struct SentMsg { uint64_t t; std::string text; int code; };
extern std::vector<SentMsg> g_sent;
extern uint64_t g_postMs;
extern std::vector<std::string> g_mqttPub;

enum wl_status_t { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
#define WIFI_STA 1
struct IPAddress { String toString() const { return String("192.168.0.50"); } };
struct WiFiClass {
  int beginCalls = 0;
  void mode(int) {}
  wl_status_t begin(const char *, const char * = nullptr) { beginCalls++; return WL_DISCONNECTED; }
  bool disconnect(bool = false, bool = false) { return true; }
  wl_status_t status() { return g_wifiUp(g_ms) ? WL_CONNECTED : WL_DISCONNECTED; }
  IPAddress localIP() { return IPAddress(); }
};
extern WiFiClass WiFi;

struct Client {};
struct WiFiClient : Client {};
struct WiFiClientSecure : WiFiClient { void setInsecure() {} };

inline std::string urlDecode(const std::string &in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '%' && i + 2 < in.size()) { out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
    else out += in[i];
  }
  return out;
}
struct HTTPClient {
  String url_;
  bool begin(WiFiClient &, String url) { url_ = url; return true; }
  void setConnectTimeout(int) {}
  void setTimeout(unsigned short) {}
  void addHeader(const String &, const String &, bool = false, bool = true) {}
  int POST(String body) {
    g_ms += g_postMs;
    int code = g_httpCode(g_ms);
    size_t p = body.s.find("&text=");
    std::string txt = p == std::string::npos ? body.s : urlDecode(body.s.substr(p + 6));
    g_sent.push_back({g_ms, txt, code});
    Serial.printf("[HTTP] POST -> %d | %s\n", code, txt.c_str());
    return code;
  }
  void end() {}
};

struct PubSubClient {
  Client *c; bool conn = false;
  PubSubClient(Client &cl) : c(&cl) {}
  PubSubClient &setServer(const char *, uint16_t) { return *this; }
  PubSubClient &setSocketTimeout(uint16_t) { return *this; }
  bool connect(const char *id) {
    if (g_mqttUp(g_ms)) { conn = true; Serial.printf("[MQTT] connect %s\n", id); return true; }
    Serial.printf("[MQTT] connect FALHOU\n"); return false;
  }
  bool connected() { if (conn && !g_mqttUp(g_ms)) conn = false; return conn; }
  bool publish(const char *t, const char *p) { Serial.printf("[MQTT] publish %s %s\n", t, p); g_mqttPub.push_back(std::string(t) + " " + p); return conn; }
  bool loop() { return conn; }
};

enum { ESP_RST_POWERON = 1, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT };
inline int esp_reset_reason() { return ESP_RST_POWERON; }
