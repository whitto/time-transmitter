#!/usr/bin/env python3
"""Compile actual Wi-Fi/SNTP functions and exercise AP recovery without hardware."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

try:
    from . import test_bluetooth_workflow as workflow
except ImportError:
    import test_bluetooth_workflow as workflow

FIRMWARE = workflow.FIRMWARE

MOCKS = r'''
#include <sys/time.h>
struct SerialMock {
  std::string log;
  void println(const char* value) { log += value; log += '\n'; }
  void println(const String& value) { println(value.c_str()); }
  void print(const char* value) { log += value; }
  template<typename... Args> void printf(const char*, Args...) {}
} Serial;
constexpr int WL_CONNECTED = 3, WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3;
constexpr int WIFI_POWER_ALWAYS_ON = 0, WIFI_POWER_SCHEDULED = 1, HTTP_POST = 1;
constexpr unsigned long WIFI_CONNECT_TIMEOUT = 30000;
constexpr unsigned long WIFI_BOOT_ON_DURATION_MS = 300000;
constexpr const char* DEVICENAME_PREFIX = "RadioStation";
struct IPAddress { String toString() const { return "192.0.2.1"; } };
bool networkInitialized=true;
struct WiFiMock {
  int connection = 0, modeValue = WIFI_AP, begins = 0, reconnects = 0;
  int apCloses = 0, apStarts = 0, disconnects = 0, modes = 0;
  bool modeOk = true, apCloseOk = true, sleep = false;
  bool macOk=true;
  int apMacReads=0, staMacReads=0;
  std::string apName;
  int status() const { return connection; }
  int getMode() const { return modeValue; }
  bool mode(int value) { ++modes; if (!modeOk) return false; modeValue=value; if (value!=WIFI_OFF) networkInitialized=true; return true; }
  bool setSleep(bool value) { sleep=value; return true; }
  void begin(const char* ssid, const char*) { assert(std::strcmp(ssid,"Cloud")==0); ++begins; }
  void reconnect() { ++reconnects; }
  void disconnect(bool) { ++disconnects; connection=0; }
  bool softAPdisconnect(bool disable) {
    assert(disable); ++apCloses;
    if (!apCloseOk) return false;
    modeValue &= ~WIFI_AP; return true;
  }
  void softAP(const char* name, const char*) { ++apStarts; apName=name; }
  uint8_t* macAddress(uint8_t*) { ++staMacReads; return nullptr; }
  uint8_t* softAPmacAddress(uint8_t* mac) {
    ++apMacReads;
    if (!macOk || !(modeValue&WIFI_AP)) return nullptr;
    const uint8_t value[6]={0x02,0x11,0x22,0xaa,0xbb,0xcc};
    std::copy(value,value+6,mac);return mac;
  }
  IPAddress localIP() const { return {}; }
  IPAddress softAPIP() const { return {}; }
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
bool ntpResumePending=false;
bool wifiRadioEnabled=true, scheduleWantsWifi=false;
unsigned long wifiRecoveryStarted=0, wifi_connect_start=0, bootMillis=0;
unsigned long tick=100;
unsigned long millis() { return tick; }
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
portMUX_TYPE clockMux=0;
uint32_t clockNtpSyncCount=0;
std::atomic<int> ntpsync{1};
std::atomic<uint32_t> ntpIntervalSec{3600};
int configs=0, ntpStops=0, restarts=0, callbackSets=0, localTimeReads=0;
bool ntpEnabled=false, restartOk=true, configOk=true, immediateReply=false;
unsigned long delayedReplyTick=0;
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
bool stopAPMode();
void reset() {
  Serial.log.clear(); WiFi=WiFiMock{}; networkInitialized=true;
  server.stops=server.starts=0; webServerStarted=true;
  ap_mode=true;wifiConnectionPending=false;wifiRecoveryWindowActive=false;ntpResumePending=false;
  wifiRecoveryStarted=0;wifi_connect_start=0;wifiRadioEnabled=true;
  wifiPowerMode=WIFI_POWER_ALWAYS_ON;bootMillis=tick;
  scheduleWantsWifi=false;radioPaused=true;radioTaskHandle=reinterpret_cast<void*>(1);
  pauseOk=resumeOk=true;pauses=resumes=refreshes=0;
  configs=ntpStops=restarts=callbackSets=localTimeReads=0;
  clockNtpSyncCount=0;ntpsync=1;ntpEnabled=false;
  restartOk=configOk=true;immediateReply=false;delayedReplyTick=0;
  std::strcpy(ssid,"Cloud");
}
'''

CASES = r'''
int main() {
  // Model the reported no-credentials cold boot: no Wi-Fi or TCP/IP task
  // exists yet. The SDK stop wrapper posts to tcpip_callback even when
  // SNTP is not running, while its enabled query only reads static state.
  reset();networkInitialized=false;WiFi.modeValue=WIFI_OFF;
  webServerStarted=false;radioTaskHandle=nullptr;
  startAPMode();
  assert(networkInitialized && ap_mode && WiFi.modeValue==WIFI_AP);
  assert(webServerStarted && WiFi.apStarts==1 && ntpStops==0);
  reset();networkInitialized=false;WiFi.modeValue=WIFI_OFF;ap_mode=false;
  beginWifiCredentialConnection();
  assert(networkInitialized && wifiConnectionPending && ntpStops==0);
  reset();networkInitialized=false;WiFi.modeValue=WIFI_OFF;
  ntpstop();assert(ntpStops==0 && !networkInitialized);

  // The guards must still stop an active client during real shutdown and
  // transitions, rather than merely suppressing all NTP stop calls.
  reset();ntpEnabled=true;startAPMode();assert(ntpStops==1 && !ntpEnabled);
  reset();ntpEnabled=true;beginWifiCredentialConnection();assert(ntpStops==1 && !ntpEnabled);
  reset();ntpEnabled=true;ntpstop();assert(ntpStops==1 && !ntpEnabled);

  reset();
  // Provisioning keeps the AP usable until DHCP/got-IP, without repeatedly
  // restarting association or claiming an NTP reply before its callback.
  beginWifiCredentialConnection();
  assert(ap_mode && wifiConnectionPending && WiFi.modeValue==WIFI_AP_STA);
  assert(WiFi.begins==1 && configs==0 && radioPaused && resumes==0);
  tick+=5000;checkWiFiConnection();
  assert(WiFi.reconnects==0 && ap_mode && wifiConnectionPending && configs==0);
  WiFi.connection=WL_CONNECTED;checkWiFiConnection();
  assert(!ap_mode && !wifiConnectionPending && WiFi.modeValue==WIFI_STA);
  assert(configs==1 && ntpEnabled && ntpsync==0 && !radioPaused && webServerStarted);
  assert(WiFi.apCloses==1 && WiFi.begins==1 && callbackSets==1);
  for (int i=0;i<4;++i) { tick+=5000;checkWiFiConnection(); }
  assert(configs==1 && ntpsync==0 && clockNtpSyncCount==0);
  struct timeval reply{};ntpCallback(&reply);
  assert(ntpsync==1 && clockNtpSyncCount==1);
  Serial.log.clear();flushNtpLogs();
  assert(Serial.log=="NTP synchronized (confirmed server reply)\n");
  flushNtpLogs();assert(Serial.log=="NTP synchronized (confirmed server reply)\n");
  ntpCallback(&reply);flushNtpLogs();
  assert(Serial.log=="NTP synchronized (confirmed server reply)\nNTP synchronized (confirmed server reply)\n");

  // Wrong credentials return to the same configuration AP and leave RF
  // paused. Credentials and other saved settings are never changed here.
  reset();beginWifiCredentialConnection();tick+=WIFI_CONNECT_TIMEOUT+1;
  checkWiFiConnection();
  assert(ap_mode && !wifiConnectionPending && WiFi.modeValue==WIFI_AP);
  assert(radioPaused && resumes==0 && configs==0 && WiFi.apStarts==1);
  assert(std::strcmp(ssid,"Cloud")==0 && WiFi.reconnects==0 && webServerStarted);
  int calls=WiFi.begins;checkWiFiConnection();assert(WiFi.begins==calls);

  // An AP-close or STA-mode error cannot report a completed transition or
  // release the RF pause. Retrying succeeds when the driver recovers.
  reset();beginWifiCredentialConnection();WiFi.connection=WL_CONNECTED;
  WiFi.apCloseOk=false;checkWiFiConnection();
  assert(ap_mode && !wifiConnectionPending && radioPaused && configs==0 && WiFi.apStarts==1);
  assert(WiFi.modeValue==WIFI_AP && webServerStarted);
  WiFi.apCloseOk=true;beginWifiCredentialConnection();WiFi.modeOk=false;checkWiFiConnection();
  assert(ap_mode && !wifiConnectionPending && radioPaused && configs==0 && WiFi.apStarts==2);
  WiFi.modeOk=true;beginWifiCredentialConnection();checkWiFiConnection();assert(!ap_mode && ntpEnabled);
  reset();WiFi.modeOk=false;beginWifiCredentialConnection();
  assert(ap_mode && !wifiConnectionPending && radioPaused && WiFi.begins==0);
  // Core 3.3.12's disabled STA MAC query returns null without filling its
  // buffer. AP naming uses the active AP MAC and a defined error fallback.
  reset();startAPMode();
  assert(WiFi.apName=="RadioStation_AABBCC" && WiFi.apMacReads==1 && WiFi.staMacReads==0);
  startAPMode();assert(WiFi.apName=="RadioStation_AABBCC");
  WiFi.macOk=false;startAPMode();
  assert(WiFi.apName=="RadioStation_000000" && WiFi.staMacReads==0);

  // If association finishes after a bounded boot/wake attempt, checking the
  // connection initializes the stopped SNTP client, exactly once.
  reset();ap_mode=false;radioPaused=false;WiFi.modeValue=WIFI_STA;
  ntpstart();assert(!ntpEnabled && ntpsync==0 && WiFi.begins==1);
  WiFi.connection=WL_CONNECTED;checkWiFiConnection();
  assert(ntpEnabled && configs==1 && !radioPaused && ntpsync==0);
  checkWiFiConnection();assert(configs==1);

  // A plausible old clock never impersonates a new server reply. Immediate
  // and delayed callbacks both produce confirmation; getLocalTime isn't used.
  reset();ap_mode=false;WiFi.connection=WL_CONNECTED;radioPaused=false;
  clockNtpSyncCount=7;ntpsync=1;ntpstart();
  assert(ntpsync==0 && clockNtpSyncCount==7 && localTimeReads==0);
  assert(Serial.log.find("confirmed server reply)")==std::string::npos);
  assert(Serial.log.find("reply pending")!=std::string::npos);
  reset();ap_mode=false;WiFi.connection=WL_CONNECTED;radioPaused=false;
  immediateReply=true;ntpstart();assert(ntpsync==1 && clockNtpSyncCount==1);
  assert(Serial.log.find("confirmed server reply)")!=std::string::npos);
  reset();ap_mode=false;WiFi.connection=WL_CONNECTED;radioPaused=false;
  delayedReplyTick=tick+3000;ntpstart();assert(ntpsync==1 && clockNtpSyncCount==1);

  // A helper call preserves an existing AP/maintenance pause, with no RF
  // resume on a failed acknowledgement or NTP initialization.
  reset();WiFi.connection=WL_CONNECTED;assert(configureNtpClient());
  assert(radioPaused && resumes==0);
  reset();WiFi.connection=WL_CONNECTED;pauseOk=false;
  assert(!configureNtpClient() && configs==0 && radioPaused && resumes==0);
  reset();ap_mode=false;radioPaused=false;WiFi.connection=WL_CONNECTED;pauseOk=false;
  assert(!configureNtpClient() && ntpResumePending && !radioPaused);
  radioPaused=true;pauseOk=true; // The queued pause acknowledgement arrived late.
  assert(configureNtpClient() && !radioPaused && !ntpResumePending);
  reset();WiFi.connection=WL_CONNECTED;configOk=false;
  assert(!configureNtpClient() && configs==1 && !ntpEnabled && radioPaused);
  // A resume timeout remains retryable and never makes a real callback
  // disappear. The STA web server remains usable during NTP/RF errors.
  reset();beginWifiCredentialConnection();WiFi.connection=WL_CONNECTED;
  resumeOk=false;immediateReply=true;checkWiFiConnection();
  assert(wifiConnectionPending && webServerStarted && ntpResumePending && ntpsync==1);
  resumeOk=true;checkWiFiConnection();
  assert(!wifiConnectionPending && !ntpResumePending && !radioPaused && ntpsync==1);

  // Provisioning after the boot grace gets a fresh finite Wi-Fi on-window,
  // even if there is no imminent schedule, giving NTP time to reply.
  reset();wifiPowerMode=WIFI_POWER_SCHEDULED;bootMillis=tick-WIFI_BOOT_ON_DURATION_MS-1000;
  beginWifiCredentialConnection();tick+=5000;updateWifiPowerManagement();
  assert(wifiRadioEnabled && WiFi.disconnects==0);
  WiFi.connection=WL_CONNECTED;checkWiFiConnection();
  tick+=5000;updateWifiPowerManagement();assert(wifiRadioEnabled && ntpEnabled);
  tick+=WIFI_BOOT_ON_DURATION_MS+5000;updateWifiPowerManagement();
  assert(!wifiRadioEnabled && !ntpEnabled && WiFi.modeValue==WIFI_OFF);
  int attempts=WiFi.reconnects;checkWiFiConnection();assert(WiFi.reconnects==attempts);

  // Manual NTP starts an uninitialized client, restarts an active one and
  // reports disconnected, initialization or SDK restart errors accurately.
  registerRoutes();reset();server.post("/api/ntp-sync");
  assert(server.code==503 && configs==0 && restarts==0);
  WiFi.connection=WL_CONNECTED;server.post("/api/ntp-sync");
  assert(server.code==200 && configs==1 && restarts==0 && ntpEnabled);
  assert(ap_mode && radioPaused); // Does not implicitly close the AP.
  server.post("/api/ntp-sync");assert(server.code==200 && restarts==1 && configs==1);
  restartOk=false;server.post("/api/ntp-sync");assert(server.code==503 && configs==1);
  ntpEnabled=false;configOk=false;server.post("/api/ntp-sync");
  assert(server.code==503 && configs==2 && radioPaused);
  std::puts("Real Wi-Fi/SNTP AP provisioning, timeout recovery, fresh callbacks and power-save grace passed");
}
'''


class WifiNtpRecoveryTest(unittest.TestCase):
    def test_real_wifi_ntp_recovery(self):
        self.assertIsNotNone(shutil.which('g++'))
        source = FIRMWARE.read_text()
        # Reuse only the Arduino String mock, not the existing Wi-Fi state mocks.
        prefix = workflow.MOCKS.split('struct SerialMock {', 1)[0]
        names = ['flushNtpLogs', 'configureNtpClient', 'ntpstart', 'ntpstop', 'startAPMode',
                 'stopAPMode', 'beginWifiCredentialConnection', 'checkWiFiConnection',
                 'updateWifiPowerManagement']
        functions = '\n\n'.join(workflow.extract_function(source.replace('void\nntpstop(', 'void ntpstop('), name) for name in names)
        route = workflow.extract_route(source, '/api/ntp-sync')
        unit = prefix + MOCKS + functions + '\nvoid registerRoutes() {\n' + route + '\n}\n' + CASES
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'wifi.cpp'
            output = Path(temporary) / 'wifi'
            path.write_text(unit)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(FIRMWARE.parent), str(path), '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True)


if __name__ == '__main__':
    unittest.main()
