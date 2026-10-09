#include <cassert>
#include <cstdio>
constexpr int WIFI_OFF=0,WIFI_STA=1,WL_CONNECTED=3;
bool webServerStarted=true,wifiRadioEnabled=true,ntp=true;
bool esp_sntp_enabled() { return ntp; }
void esp_sntp_stop() { ntp=false; }
struct Server { void stop() {} } server;
struct Wifi {
 int modeValue=WIFI_STA, connection=WL_CONNECTED;
 bool disconnect(bool) { return false; }
 bool mode(int) { return false; }
} WiFi;
void ntpstop(void)
{
  // Wi-Fi sleep does not invalidate a previously synchronized holdover clock.
  // Stopping an unstarted client can post to an uninitialized TCP/IP mailbox.
  if (esp_sntp_enabled()) esp_sntp_stop();
  if (webServerStarted) { server.stop(); webServerStarted=false; }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}int main() {
wifiRadioEnabled=false;ntpstop();
assert(!wifiRadioEnabled && WiFi.modeValue==WIFI_STA && WiFi.connection==WL_CONNECTED);
assert(!webServerStarted && !ntp);
puts("CONFIRMED actual ntpstop with injected SDK disconnect/mode failures: intended radio Off, station still connected, web server and SNTP stopped; no reported/retried transition fault");
}
