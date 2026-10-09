#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <functional>
#include <string>
#include <algorithm>
using String=std::string;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
using portMUX_TYPE=int;

#include <sys/time.h>
#include <vector>
struct SerialMock {
  std::string log;
  void println(const char* value) { log += value; log += '\n'; }
  void println(const String& value) { println(value.c_str()); }
  void print(const char* value) { log += value; }
  template<typename... Args> void printf(const char* format, Args... args) {
    char buffer[1024];
    int count=std::snprintf(buffer,sizeof(buffer),format,args...);
    assert(count>=0 && size_t(count)<sizeof(buffer)); log+=buffer;
  }
} Serial;
constexpr int WL_CONNECTED = 3, WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3;
constexpr int WIFI_POWER_ALWAYS_ON = 0, WIFI_POWER_SCHEDULED = 1, HTTP_POST = 1;
constexpr unsigned long WIFI_CONNECT_TIMEOUT = 30000;
constexpr unsigned long WIFI_BOOT_ON_DURATION_MS = 300000;
constexpr const char* DEVICENAME_PREFIX = "RadioStation";
constexpr uint32_t AP_START_RETRY_MS=5000;
constexpr int ESP_OK=0, ESP_MAC_WIFI_SOFTAP=1;
int macReadResult=ESP_OK, derivedMacReads=0;
uint8_t derivedApMac[6]={0x02,0x11,0x22,0xaa,0xbb,0xcc};
int esp_read_mac(uint8_t* mac,int kind) {
  assert(kind==ESP_MAC_WIFI_SOFTAP); ++derivedMacReads;
  if (macReadResult!=ESP_OK) return macReadResult;
  std::copy(derivedApMac,derivedApMac+6,mac); return ESP_OK;
}
struct IPAddress {
  uint32_t value=0xc0000201;
  explicit operator uint32_t() const { return value; }
  String toString() const { return value ? "192.0.2.1" : "0.0.0.0"; }
};
bool networkInitialized=true;
struct WiFiMock {
  int connection = 0, modeValue = WIFI_AP, begins = 0, reconnects = 0;
  int apCloses = 0, apStarts = 0, disconnects = 0, modes = 0;
  std::vector<int> modeAttempts;
  bool modeOk=true, offModeOk=true, apCloseOk=true, apConfigOk=true, sleep=false;
  bool apEventArrives=true;
  struct { bool ready=true; bool started() const { return ready; } } AP;
  uint32_t apIPValue=0xc0000201;
  bool macOk=true;
  int apMacReads=0, staMacReads=0;
  std::string apName;
  int status() const { return connection; }
  int getMode() const { return modeValue; }
  bool mode(int value) {
    ++modes;
    modeAttempts.push_back(value);
    if (!modeOk || (value==WIFI_OFF && !offModeOk)) return false;
    modeValue=value; AP.ready=false;
    if (value!=WIFI_OFF) networkInitialized=true;
    return true;
  }
  bool setSleep(bool value) { sleep=value; return true; }
  void begin(const char* ssid, const char*) { assert(std::strcmp(ssid,"Cloud")==0); ++begins; }
  void reconnect() { ++reconnects; }
  void disconnect(bool) { ++disconnects; connection=0; }
  bool softAPdisconnect(bool disable) {
    assert(disable); ++apCloses;
    if (!apCloseOk) return false;
    modeValue &= ~WIFI_AP; return true;
  }
  bool softAP(const char* name,const char* password,int channel=1,int hidden=0,int maximum=4) {
    assert(std::strcmp(password,"12345678")==0 && channel==1 && hidden==0 && maximum==4);
    ++apStarts; apName=name; AP.ready=apEventArrives; return apConfigOk;
  }
  uint8_t* macAddress(uint8_t*) { ++staMacReads; return nullptr; }
  uint8_t* softAPmacAddress(uint8_t* mac) {
    ++apMacReads;
    if (!macOk || !(modeValue&WIFI_AP)) return nullptr;
    if (!AP.ready) { std::fill(mac,mac+6,0); return mac; }
    std::copy(derivedApMac,derivedApMac+6,mac);return mac;
  }
  IPAddress localIP() const { return {}; }
  IPAddress softAPIP() const { return {apIPValue}; }
} WiFi;
struct ServerMock {
  std::map<std::string,std::function<void()>> routes;
  int code=0, stops=0, starts=0;
  String response;
  void on(const char* path,int,std::function<void()> fn) { routes[path]=fn; }
  void send(int value,const char*,String body) { code=value;response=body; }
  void stop() { ++stops; }
  void begin() { ++starts; }
  void post(const char* path) { code=0; routes.at(path)(); assert(code); }
} server;
bool webServerStarted=true;
void initWebServer() { server.begin(); webServerStarted=true; }
bool ap_mode=true, wifiConnectionPending=false, wifiRecoveryWindowActive=false;
bool apStartPending=false;
uint32_t apStartAttemptMillis=0;
bool ntpResumePending=false;
bool wifiRadioEnabled=true, scheduleWantsWifi=false, accessWantsWifi=false;
uint32_t wifiRecoveryStarted=0, wifi_connect_start=0, bootMillis=0;
uint32_t tick=100;
uint32_t millis() { return tick; }
char ssid[64]="Cloud", passwd[64]="not-a-secret";
String timezone_name="Australia/Brisbane";
int wifiPowerMode=WIFI_POWER_ALWAYS_ON;
std::atomic<bool> radioPaused{true};
void* radioTaskHandle=reinterpret_cast<void*>(1);
bool pauseOk=true, resumeOk=true;
int pauses=0, resumes=0, refreshes=0;
bool radioSetPaused(bool value) {
  if (value) { ++pauses; if (!pauseOk) return false; }
  else { ++resumes; if (!resumeOk) return false; }
  radioPaused=value; return true;
}
void radioRequestRefresh() { ++refreshes; }
String posixTzFor(const String& name) { assert(name=="Australia/Brisbane"); return "AEST-10"; }
bool shouldWifiBeOnForSchedule() { return scheduleWantsWifi; }
bool wifiAccessWindowOpen() { return accessWantsWifi; }
portMUX_TYPE clockMux=0;
uint32_t clockNtpSyncCount=0;
std::atomic<int> ntpsync{1};
std::atomic<uint32_t> ntpIntervalSec{3600};
int configs=0, ntpStops=0, restarts=0, callbackSets=0, localTimeReads=0;
bool ntpEnabled=false, restartOk=true, configOk=true, immediateReply=false;
uint32_t delayedReplyTick=0;
uint32_t intervalMs=0;
constexpr int SNTP_SYNC_MODE_IMMED=1;
void (*ntpCallback)(struct timeval*)=nullptr;
void onNtpSync(struct timeval*) { ++clockNtpSyncCount; ntpsync=1; }
void sntp_set_time_sync_notification_cb(void (*cb)(struct timeval*)) { ntpCallback=cb; ++callbackSets; }
void sntp_set_sync_mode(int mode) { assert(mode==SNTP_SYNC_MODE_IMMED); }
void sntp_set_sync_interval(uint32_t ms) { intervalMs=ms; }
bool esp_sntp_enabled() { return ntpEnabled; }
void esp_sntp_stop() { assert(networkInitialized && "Invalid mbox: stop before TCP/IP startup"); ++ntpStops; ntpEnabled=false; }
bool sntp_restart() { ++restarts; return restartOk && ntpEnabled; }
void configTzTime(const char* tz,const char* a,const char* b) {
  assert(std::strcmp(tz,"AEST-10")==0 && std::strcmp(a,"pool.ntp.org")==0 &&
         std::strcmp(b,"time.nist.gov")==0);
  ++configs; ntpEnabled=configOk;
  if (ntpEnabled && immediateReply) { struct timeval tv{}; ntpCallback(&tv); }
}
bool getLocalTime(struct tm*,unsigned long) { ++localTimeReads; return true; }
void delay(unsigned long value) {
  tick+=value;
  if (ntpEnabled && delayedReplyTick && tick>=delayedReplyTick) {
    delayedReplyTick=0; struct timeval tv{}; ntpCallback(&tv);
  }
}
static bool configureNtpClient();
static void beginWifiCredentialConnection();
void ntpstart();
void ntpstop();
void startAPMode();
void serviceAPStartup();
bool stopAPMode();
void reset() {
  Serial.log.clear(); WiFi=WiFiMock{}; networkInitialized=true;
  server.stops=server.starts=0; webServerStarted=true;
  ap_mode=true;wifiConnectionPending=false;wifiRecoveryWindowActive=false;ntpResumePending=false;
  apStartPending=false;apStartAttemptMillis=0;
  macReadResult=ESP_OK;derivedMacReads=0;
  const uint8_t defaultMac[6]={0x02,0x11,0x22,0xaa,0xbb,0xcc};
  std::copy(defaultMac,defaultMac+6,derivedApMac);
  wifiRecoveryStarted=0;wifi_connect_start=0;wifiRadioEnabled=true;
  wifiPowerMode=WIFI_POWER_ALWAYS_ON;bootMillis=tick;
  scheduleWantsWifi=accessWantsWifi=false;radioPaused=true;radioTaskHandle=reinterpret_cast<void*>(1);
  pauseOk=resumeOk=true;pauses=resumes=refreshes=0;
  configs=ntpStops=restarts=callbackSets=localTimeReads=0;
  clockNtpSyncCount=0;ntpsync=1;ntpEnabled=false;
  restartOk=configOk=true;immediateReply=false;delayedReplyTick=0;
  std::strcpy(ssid,"Cloud");
}
void updateWifiPowerManagement(void)
{
  static int appliedPowerMode = -1;
  if (WiFi.getMode() != WIFI_OFF && appliedPowerMode != wifiPowerMode) {
    if (WiFi.setSleep(wifiPowerMode != WIFI_POWER_ALWAYS_ON)) appliedPowerMode = wifiPowerMode;
  }
  if (ap_mode) return;
  if (wifiConnectionPending) return; // Do not cut power during a new credential attempt.
  if (wifiPowerMode != WIFI_POWER_SCHEDULED) {
    if (!wifiRadioEnabled) { wifiRadioEnabled=true; ntpstart(); }
    return;
  }

  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  if (now - lastCheck < 5000) return;
  lastCheck = now;

  if (wifiRecoveryWindowActive && now - wifiRecoveryStarted >= WIFI_BOOT_ON_DURATION_MS)
    wifiRecoveryWindowActive = false;
  bool wantOn = (now - bootMillis < WIFI_BOOT_ON_DURATION_MS) ||
                wifiRecoveryWindowActive || wifiAccessWindowOpen() || shouldWifiBeOnForSchedule();

  if (wantOn && !wifiRadioEnabled) {
    Serial.println("WiFi power-save: waking radio for sync or web-access window");
    wifiRadioEnabled = true;
    ntpstart();
    if (WiFi.status() == WL_CONNECTED) {
      initWebServer();
    }
  } else if (!wantOn && wifiRadioEnabled) {
    Serial.println("WiFi power-save: sync/web-access windows closed, powering down radio");
    wifiRadioEnabled = false;
    ntpstop(); // stops SNTP cleanly, then disconnects and powers off the radio
  }
}
void ntpstart(){++WiFi.begins;} void ntpstop(){++ntpStops;}
int main(){reset();ap_mode=false;WiFi.modeValue=WIFI_STA;wifiPowerMode=WIFI_POWER_SCHEDULED;
bootMillis=1000;tick=1000+600000;wifiRadioEnabled=false;updateWifiPowerManagement();assert(!wifiRadioEnabled);
// Physical uptime now reaches one complete millis cycle + 1s; stored tick wraps to 2000.
tick=2000;updateWifiPowerManagement();assert(wifiRadioEnabled && WiFi.begins==1);
printf("CONFIRMED: startup access window reopens after millis wraps at 49.71 days (bootMillis=1000, tick=2000)\n");
return 0;}
