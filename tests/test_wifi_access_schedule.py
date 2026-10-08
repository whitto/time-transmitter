#!/usr/bin/env python3
"""Exercise daily access bounds, actual timezone conversion and Wi-Fi decisions."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

try:
    from . import test_bluetooth_workflow as workflow
    from . import test_wifi_ntp_recovery as recovery
except ImportError:
    import test_bluetooth_workflow as workflow
    import test_wifi_ntp_recovery as recovery

FIRMWARE = workflow.FIRMWARE

ACCESS_MOCKS = r'''
#include "RadioWifiAccessWindow.h"
bool wifiAccessEnabled=false;
int wifiAccessStart=1080, wifiAccessEnd=1200;
String wifiAccessTimezone="Australia/Brisbane";
String btTimezoneName="Asia/Tokyo";
int btTimeOffsetMinutes=600, transmission_offset_minutes=-420;
time_t fakeEpoch=0;
time_t fakeTime(time_t* out) { if (out) *out=fakeEpoch; return fakeEpoch; }
#define time fakeTime
time_t utc(int year,int month,int day,int hour=0,int minute=0,int second=0) {
  struct tm value{};
  value.tm_year=year-1900; value.tm_mon=month-1; value.tm_mday=day;
  value.tm_hour=hour; value.tm_min=minute; value.tm_sec=second;
  return timegm(&value);
}
'''

TIMEZONE_CASES = r'''
int main() {
  setenv("TZ","JST-9",1); tzset();
  wifiAccessEnabled=true;
  fakeEpoch=utc(2026,10,9,7,59,59); assert(!wifiAccessWindowOpen());
  fakeEpoch=utc(2026,10,9,8); assert(wifiAccessWindowOpen()); // Brisbane 18:00
  fakeEpoch=utc(2026,10,9,9,59,59); assert(wifiAccessWindowOpen());
  fakeEpoch=utc(2026,10,9,10); assert(!wifiAccessWindowOpen());
  fakeEpoch=utc(2026,10,10,8); assert(wifiAccessWindowOpen()); // repeats daily
  fakeEpoch=utc(2026,10,11,8); assert(wifiAccessWindowOpen());
  wifiAccessEnabled=false; assert(!wifiAccessWindowOpen());
  wifiAccessEnabled=true; fakeEpoch=0; assert(!wifiAccessWindowOpen());
  fakeEpoch=utc(2019,12,31,8); assert(!wifiAccessWindowOpen());
  fakeEpoch=utc(2026,10,9,8);
  wifiAccessStart=wifiAccessEnd=1080; assert(!wifiAccessWindowOpen());
  wifiAccessStart=-1; wifiAccessEnd=1200; assert(!wifiAccessWindowOpen());
  wifiAccessStart=1080; wifiAccessEnd=1440; assert(!wifiAccessWindowOpen());

  wifiAccessStart=23*60; wifiAccessEnd=60;
  fakeEpoch=utc(2026,10,9,12,59,59); assert(!wifiAccessWindowOpen());
  fakeEpoch=utc(2026,10,9,13); assert(wifiAccessWindowOpen());
  fakeEpoch=utc(2026,10,9,14,59,59); assert(wifiAccessWindowOpen());
  fakeEpoch=utc(2026,10,9,15); assert(!wifiAccessWindowOpen());
  // The access timezone is independent of RF/BT timezone and their offsets.
  btTimezoneName="America/Los_Angeles"; btTimeOffsetMinutes=-720;
  transmission_offset_minutes=840;
  fakeEpoch=utc(2026,10,9,13); assert(wifiAccessWindowOpen());
  assert(std::strcmp(std::getenv("TZ"),"JST-9")==0);

  // Compare the actual access-time helper with native POSIX civil time for
  // every supported zone across a full year and at both sides of DST edges.
  const char* zones[]={"Australia/Brisbane","Australia/Sydney","Asia/Tokyo",
    "Asia/Shanghai","Europe/London","America/New_York","America/Los_Angeles","UTC"};
  wifiAccessStart=90; wifiAccessEnd=210;
  const time_t begin=utc(2026,1,1), end=utc(2027,1,1);
  for (const char* zone : zones) {
    wifiAccessTimezone=zone;
    setenv("TZ",posixTzFor(wifiAccessTimezone).c_str(),1); tzset();
    const std::string unchanged=std::getenv("TZ");
    for (fakeEpoch=begin;fakeEpoch<end;fakeEpoch+=17*60) {
      struct tm expected{}; localtime_r(&fakeEpoch,&expected);
      const int minute=expected.tm_hour*60+expected.tm_min;
      assert(wifiAccessWindowOpen()==RadioWifiAccessWindow::containsMinute(minute,90,210));
      assert(std::getenv("TZ")==unchanged);
    }
  }
  // Sydney spring skips 02:00; autumn repeats 02:00. Both use civil windows,
  // rather than a fixed UTC offset or a BT-specific additive time offset.
  wifiAccessTimezone="Australia/Sydney"; wifiAccessStart=120; wifiAccessEnd=180;
  fakeEpoch=utc(2026,10,3,15,59,59); assert(!wifiAccessWindowOpen());
  fakeEpoch=utc(2026,10,3,16); assert(!wifiAccessWindowOpen());
  fakeEpoch=utc(2026,4,4,15,30); assert(wifiAccessWindowOpen());
  fakeEpoch=utc(2026,4,4,16,30); assert(wifiAccessWindowOpen());
  fakeEpoch=utc(2026,4,4,17); assert(!wifiAccessWindowOpen());
  std::puts("Actual daily Wi-Fi access conversion: recurring/overnight windows, independent zones and DST passed");
}
'''

POWER_CASES = r'''
void inspectPower() { tick+=5001; updateWifiPowerManagement(); }
void setupScheduled() {
  reset(); ap_mode=false; radioPaused=false; WiFi.modeValue=WIFI_STA;
  wifiPowerMode=WIFI_POWER_SCHEDULED;
  bootMillis=tick-WIFI_BOOT_ON_DURATION_MS-1000;
  wifiAccessEnabled=true; wifiAccessTimezone="Australia/Brisbane";
  wifiAccessStart=1080; wifiAccessEnd=1200;
  fakeEpoch=utc(2026,10,9,7); // 17:00 Brisbane, before the daily window.
}
int main() {
  // An expired boot grace outside all windows turns off Wi-Fi and the web
  // server; existing supervision respects that intentional off state.
  setupScheduled(); ntpEnabled=true; inspectPower();
  assert(!wifiRadioEnabled && !ntpEnabled && WiFi.modeValue==WIFI_OFF && !webServerStarted);
  const int reconnects=WiFi.reconnects; checkWiFiConnection();
  assert(WiFi.reconnects==reconnects);

  // Start/end boundaries drive the actual existing NTP/network wake path.
  fakeEpoch=utc(2026,10,9,8); WiFi.connection=WL_CONNECTED; immediateReply=true;
  inspectPower();
  assert(wifiRadioEnabled && WiFi.begins==1 && ntpEnabled && webServerStarted);
  fakeEpoch=utc(2026,10,9,9,59,59); inspectPower();
  assert(wifiRadioEnabled && WiFi.begins==1 && WiFi.disconnects==1);
  fakeEpoch=utc(2026,10,9,10); inspectPower();
  assert(!wifiRadioEnabled && !webServerStarted && !ntpEnabled);
  fakeEpoch=utc(2026,10,10,8); WiFi.connection=WL_CONNECTED;
  inspectPower(); assert(wifiRadioEnabled && WiFi.begins==2 && webServerStarted);

  // Existing NTP schedule wakes are additional access, never replaced.
  fakeEpoch=utc(2026,10,10,7); scheduleWantsWifi=true;
  inspectPower(); assert(wifiRadioEnabled);
  scheduleWantsWifi=false; inspectPower(); assert(!wifiRadioEnabled);

  // Startup and recovery access remain available outside the daily window.
  setupScheduled(); bootMillis=tick; inspectPower();
  assert(wifiRadioEnabled && WiFi.disconnects==0);
  bootMillis=tick-WIFI_BOOT_ON_DURATION_MS-1000;
  wifiRecoveryWindowActive=true; wifiRecoveryStarted=tick;
  inspectPower(); assert(wifiRadioEnabled && wifiRecoveryWindowActive);
  tick+=WIFI_BOOT_ON_DURATION_MS;
  inspectPower(); assert(!wifiRadioEnabled && !wifiRecoveryWindowActive);

  // AP provisioning and pending credential association retain their
  // existing ownership. A scheduled boundary cannot disconnect a user.
  setupScheduled(); ap_mode=true; inspectPower();
  assert(wifiRadioEnabled && WiFi.disconnects==0 && WiFi.begins==0);
  ap_mode=false; wifiConnectionPending=true; inspectPower();
  assert(wifiRadioEnabled && WiFi.disconnects==0 && WiFi.begins==0);
  wifiConnectionPending=false; inspectPower(); assert(!wifiRadioEnabled);

  // Always-on still overrides a saved daily window. Disabling daily access
  // leaves Power-save and its NTP windows unchanged.
  setupScheduled(); wifiPowerMode=WIFI_POWER_ALWAYS_ON;
  inspectPower(); assert(wifiRadioEnabled && WiFi.disconnects==0);
  wifiPowerMode=WIFI_POWER_SCHEDULED; wifiAccessEnabled=false;
  fakeEpoch=utc(2026,10,9,8); inspectPower(); assert(!wifiRadioEnabled);
  scheduleWantsWifi=true; WiFi.connection=WL_CONNECTED; immediateReply=true;
  inspectPower(); assert(wifiRadioEnabled && ntpEnabled);
  // Opening a Wi-Fi window does not take ownership of the RF/BLE arbiter.
  assert(!radioPaused);
  std::puts("Actual Wi-Fi power decisions preserve AP/association, boot/recovery grace, NTP wakes and daily access");
}
'''

CONFIG_CASES = r'''
std::map<std::string,String> accessFields(const char* enabled="1") {
  return {{"wifi_access_enabled",enabled},{"wifi_access_start","1380"},
          {"wifi_access_end","60"},{"wifi_access_timezone","Europe/London"}};
}
void assertSavedAccess() {
  assert(wifiAccessEnabled && wifiAccessStart==1380 && wifiAccessEnd==60);
  assert(wifiAccessTimezone=="Europe/London" && wifiPowerMode==WIFI_POWER_SCHEDULED);
}
int main() {
  registerRoutes();
  assert(!wifiAccessEnabled && wifiAccessStart==1080 && wifiAccessEnd==1200);
  assert(wifiAccessTimezone=="Australia/Brisbane");
  server.post("/api/config",accessFields());
  assert(server.code==200 && !configDirty); assertSavedAccess();
  // Successful acknowledgements are durable immediately, not after debounce.
  wifiAccessEnabled=false; wifiAccessStart=0; wifiAccessEnd=1;
  wifiAccessTimezone="Asia/Tokyo"; wifiPowerMode=WIFI_POWER_ALWAYS_ON;
  loadConfig(); assertSavedAccess();

  server.post("/api/config",accessFields("0"));
  assert(server.code==200 && !wifiAccessEnabled && wifiPowerMode==WIFI_POWER_SCHEDULED);
  wifiAccessEnabled=true; loadConfig();
  assert(!wifiAccessEnabled && wifiPowerMode==WIFI_POWER_SCHEDULED);
  // Disabling retains an explicitly selected Always-on mode too.
  wifiPowerMode=WIFI_POWER_ALWAYS_ON;
  server.post("/api/config",accessFields("false"));
  assert(server.code==200 && !wifiAccessEnabled && wifiPowerMode==WIFI_POWER_ALWAYS_ON);
  server.post("/api/config",accessFields("true"));
  assert(server.code==200); assertSavedAccess();
  const auto saved=flash.at(CONFIG_FILE);

  // Missing/grouped fields, malformed booleans/numbers, invalid bounds and
  // unsupported zones never mutate the existing working configuration.
  for (const char* key : {"wifi_access_enabled","wifi_access_start","wifi_access_end","wifi_access_timezone"}) {
    auto fields=accessFields(); fields.erase(key);
    server.post("/api/config",fields);
    assert(server.code==400); assertSavedAccess(); assert(flash.at(CONFIG_FILE)==saved);
  }
  const std::pair<const char*,const char*> invalid[]={
    {"wifi_access_enabled","yes"},{"wifi_access_enabled","2"},
    {"wifi_access_start","-1"},{"wifi_access_start","1440"},
    {"wifi_access_start","01380"},{"wifi_access_start","1380junk"},
    {"wifi_access_start",""},{"wifi_access_end","1440"},
    {"wifi_access_end","1380"},{"wifi_access_timezone","Mars/Olympus"}};
  for (const auto& entry : invalid) {
    auto fields=accessFields(); fields[entry.first]=entry.second;
    server.post("/api/config",fields);
    assert(server.code==400); assertSavedAccess(); assert(flash.at(CONFIG_FILE)==saved);
  }

  // The real atomic writer and API rollback retain the previous setting on
  // temp-open, short-write and rename failure, with an accurate HTTP error.
  auto changed=accessFields("0");
  changed["wifi_access_start"]="600"; changed["wifi_access_end"]="700";
  changed["wifi_access_timezone"]="UTC";
  accessRenameOk=false; server.post("/api/config",changed);
  assert(server.code==500); assertSavedAccess(); assert(flash.at(CONFIG_FILE)==saved);
  accessRenameOk=true; accessOpenOk=false; server.post("/api/config",changed);
  assert(server.code==500); assertSavedAccess(); assert(flash.at(CONFIG_FILE)==saved);
  accessOpenOk=true; accessShortWrite=true; server.post("/api/config",changed);
  assert(server.code==500); assertSavedAccess(); assert(flash.at(CONFIG_FILE)==saved);
  accessShortWrite=false; server.post("/api/config",changed);
  assert(server.code==200 && !wifiAccessEnabled && wifiAccessStart==600 && wifiAccessEnd==700);
  wifiAccessEnabled=true; wifiAccessStart=0; wifiAccessEnd=1; wifiAccessTimezone="Asia/Tokyo";
  loadConfig();
  assert(!wifiAccessEnabled && wifiAccessStart==600 && wifiAccessEnd==700 && wifiAccessTimezone=="UTC");

  // An attempted enable also rolls back its automatic mode change when the
  // durable save fails, instead of silently switching Always-on to sleep.
  wifiPowerMode=WIFI_POWER_ALWAYS_ON; assert(writeConfigNow());
  const auto beforeFailedEnable=flash.at(CONFIG_FILE);
  accessRenameOk=false; server.post("/api/config",accessFields());
  assert(server.code==500 && !wifiAccessEnabled && wifiPowerMode==WIFI_POWER_ALWAYS_ON);
  assert(wifiAccessStart==600 && wifiAccessEnd==700 && wifiAccessTimezone=="UTC");
  assert(flash.at(CONFIG_FILE)==beforeFailedEnable); accessRenameOk=true;

  // An older saved config with no appended access fields restores safe
  // disabled defaults rather than retaining stale RAM window values.
  std::string legacy=flash.at(CONFIG_FILE);
  for (int field=0;field<4;++field) {
    assert(!legacy.empty() && legacy.back()=='\n'); legacy.pop_back();
    const auto boundary=legacy.rfind('\n'); assert(boundary!=std::string::npos);
    legacy.resize(boundary+1);
  }
  flash[CONFIG_FILE]=legacy;
  wifiAccessEnabled=true; wifiAccessStart=10; wifiAccessEnd=20; wifiAccessTimezone="UTC";
  loadConfig();
  assert(!wifiAccessEnabled && wifiAccessStart==1080 && wifiAccessEnd==1200);
  assert(wifiAccessTimezone=="Australia/Brisbane");
  std::puts("Real daily Wi-Fi config: strict validation, durable save/load, legacy defaults and atomic failure rollback passed");
}
'''


class WifiAccessScheduleTest(unittest.TestCase):
    def compile_run(self, source):
        self.assertIsNotNone(shutil.which('g++'))
        with tempfile.TemporaryDirectory(prefix='radioclock-wifi-access-') as temporary:
            unit, binary = Path(temporary) / 'test.cpp', Path(temporary) / 'test'
            unit.write_text(source)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(FIRMWARE.parent), str(unit), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def timezone_functions(self, source):
        names = ['btWeekday', 'btNthSunday', 'btLastSunday', 'btDayOfYear',
                 'btDstAtUtc', 'btBaseOffsetSeconds', 'wifiAccessWindowOpen']
        return '\n\n'.join(workflow.extract_function(source, name) for name in names)

    def test_half_open_daily_bounds(self):
        self.compile_run(r'''
#include "RadioWifiAccessWindow.h"
#include <cassert>
#include <climits>
#include <cstdio>
int main() {
  const int points[]={0,1,30,60,600,1080,1200,1380,1438,1439};
  for (int start : points) for (int end : points) {
    assert(RadioWifiAccessWindow::validMinutes(start,end)==(start!=end));
    for (int current=0;current<1440;++current) {
      const int duration=(end-start+1440)%1440;
      const int elapsed=(current-start+1440)%1440;
      assert(RadioWifiAccessWindow::containsMinute(current,start,end)==
             (start!=end && elapsed<duration));
    }
  }
  const int invalid[]={INT_MIN,-1,1440,1441,INT_MAX};
  for (int value : invalid) {
    assert(!RadioWifiAccessWindow::containsMinute(value,1080,1200));
    assert(!RadioWifiAccessWindow::containsMinute(1100,value,1200));
    assert(!RadioWifiAccessWindow::containsMinute(1100,1080,value));
  }
  std::puts("Daily Wi-Fi bounds: exhaustive civil-minute comparisons and invalid inputs passed");
}
''')

    def test_real_access_timezone_and_daily_recurrence(self):
        source = FIRMWARE.read_text()
        prefix = workflow.MOCKS.split('struct SerialMock {', 1)[0]
        functions = self.timezone_functions(source)
        functions += '\n' + workflow.extract_function(source, 'posixTzFor')
        self.compile_run(prefix + ACCESS_MOCKS + functions + TIMEZONE_CASES)

    def test_real_power_management_preserves_existing_exceptions(self):
        source = FIRMWARE.read_text()
        prefix = workflow.MOCKS.split('struct SerialMock {', 1)[0]
        mocks = recovery.MOCKS.replace('bool wifiAccessWindowOpen() { return accessWantsWifi; }', '')
        mocks = mocks.replace('static void beginWifiCredentialConnection();', '')
        names = ['configureNtpClient', 'ntpstart', 'ntpstop', 'startAPMode',
                 'stopAPMode', 'checkWiFiConnection', 'updateWifiPowerManagement']
        functions = self.timezone_functions(source)
        functions += '\n\n' + '\n\n'.join(workflow.extract_function(
            source.replace('void\nntpstop(', 'void ntpstop('), name) for name in names)
        self.compile_run(prefix + mocks + ACCESS_MOCKS + functions + POWER_CASES)

    def test_real_access_configuration_is_durable_and_rolls_back(self):
        source = FIRMWARE.read_text()
        unit = workflow.BluetoothWorkflowTest().unit_source(source)
        mocks = workflow.MOCKS
        mocks = mocks.replace('struct LittleFsMock {',
                              'bool accessRenameOk=true, accessOpenOk=true, accessShortWrite=false;\nstruct LittleFsMock {')
        mocks = mocks.replace("if (*mode == 'w') {", "if (*mode == 'w') { if (!accessOpenOk) return {}; ")
        mocks = mocks.replace('flash[to] = flash.at(from);',
                              'if (!accessRenameOk) return false; flash[to] = flash.at(from);')
        mocks = mocks.replace('struct File {', 'extern bool accessShortWrite;\nstruct File {')
        mocks = mocks.replace('size_t print(const String& s) { *contents += s.s; return s.length(); }',
                              'size_t print(const String& s) { *contents += s.s; return accessShortWrite ? 0 : s.length(); }')
        mocks = mocks.replace('static bool validTimezoneName(const String&) { return true; }',
                              workflow.extract_function(source, 'validTimezoneName'))
        unit = unit.replace(workflow.MOCKS, mocks, 1)
        group = workflow.balanced_block(source, source.index('if (server.hasArg("wifi_access_enabled")'))
        self.assertIn(group, workflow.extract_route(unit, '/api/config'))
        unit = unit.replace(workflow.DRIVER, CONFIG_CASES, 1)
        self.compile_run('#include "RadioWifiAccessWindow.h"\n' + unit)


if __name__ == '__main__':
    unittest.main()
