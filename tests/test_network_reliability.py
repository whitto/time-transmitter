#!/usr/bin/env python3
"""Fault/recovery checks using the actual V4.16 network policy functions."""
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

POLICY_MOCKS = r'''
constexpr int BT_SYNC_SLOT_COUNT=4, BT_PREPARE_MINUTES=5, BT_WINDOW_AFTER_MINUTES=5;
constexpr int WIFI_PRE_SCHEDULE_LEAD_MIN=10;
bool full_time_tx=false, btSyncEnabled[4]={};
int btSyncTimes[4]={30,390,750,1110};
struct TimeSchedule { int start_min,end_min; };
TimeSchedule schedules[24]={{0,1440}};
int schedule_count=1;
time_t fakeEpoch=1791547200; // Daytime UTC; outside the LF midnight lead.
time_t fakeTime(time_t* value) { if(value) *value=fakeEpoch; return fakeEpoch; }
#define time fakeTime
void bluetoothLocalTime(time_t epoch,struct tm &out) { gmtime_r(&epoch,&out); }
bool shouldWifiBeOnForSchedule();
'''

CASES = r'''
void setupPowerSave() {
  reset(); ap_mode=false; radioPaused=false; WiFi.modeValue=WIFI_STA;
  wifiPowerMode=WIFI_POWER_SCHEDULED; wifiBootAccessComplete=true;
  wifiRecoveryWindowActive=false; ntpIntervalSec=3600;
  fakeEpoch=1791547200; clockMockAge=0; WiFi.connection=WL_CONNECTED;
}
void powerTick() { tick+=1001;updateWifiPowerManagement(); }
int main() {
  setenv("TZ","UTC0",1);tzset();
  // Default all-day LF with BT windows Off still gets adaptive NTP wakes.
  setupPowerSave();assert(!shouldWifiBeOnForSchedule());
  clockMockAge=3539;powerTick();assert(!wifiRadioEnabled);
  clockMockAge=3540;const uint32_t before=tick;powerTick();
  assert(tick==before+1001 && wifiRadioEnabled && wifiConnectionPending && wifiClockWakeActive);
  assert(WiFi.begins==1 && !ntpEnabled);
  WiFi.connection=WL_CONNECTED;immediateReply=true;checkWiFiConnection();
  assert(ntpsync==1 && !wifiConnectionPending && !wifiRecoveryWindowActive);
  powerTick();assert(!wifiRadioEnabled && !wifiClockWakeActive && !ntpEnabled);
  // Much shorter confidence deadlines override arbitrary user schedules.
  clockMockAge=600;ntpIntervalSec=630;powerTick();
  assert(wifiRadioEnabled && wifiClockWakeActive && WiFi.begins==2);
  WiFi.connection=WL_CONNECTED;checkWiFiConnection();powerTick();assert(!wifiRadioEnabled);

  // No NTP reply gives a finite two-minute wake, then bounded retry backoff.
  // The unsafe clock flag/holdover rejection never gets promoted by policy.
  setupPowerSave();clockMockAge=UINT32_MAX;powerTick();
  assert(wifiClockWakeActive && wifiRadioEnabled);
  immediateReply=false; // Station connected, but servers unavailable.
  tick+=WIFI_CLOCK_WAKE_TIMEOUT_MS;powerTick();
  assert(!wifiClockWakeActive && !wifiRadioEnabled && wifiClockRetryDelayMs==60000);
  const uint64_t retry=wifiClockRetryAtMs;
  tick+=28000;powerTick();assert((uint64_t)tick<retry && !wifiRadioEnabled);
  tick+=1000;powerTick();assert(wifiClockWakeActive && wifiRadioEnabled);
  // Failed SNTP initialization must not retain an association-pending flag
  // forever or defeat the bounded wake. Retry attempts have their own backoff.
  setupPowerSave();clockMockAge=UINT32_MAX;configOk=false;powerTick();
  checkWiFiConnection();assert(!wifiConnectionPending && configs==1 && !ntpEnabled);
  for(int i=0;i<50;++i) { checkWiFiConnection(); }
  assert(configs==1);
  tick+=WIFI_CLOCK_WAKE_TIMEOUT_MS;powerTick();assert(!wifiRadioEnabled);

  // Callback rejection also creates a retry window regardless of nominal age.
  setupPowerSave();clockNtpRejected=true;powerTick();assert(wifiClockWakeActive);
  clockNtpRejected=false;++clockNtpSyncCount;clockMockAge=0;
  powerTick();assert(!wifiClockWakeActive && !wifiRadioEnabled);

  // An already-completed startup grace cannot reopen after 49.71 days.
  setupPowerSave();bootMillis=100;tick=UINT32_MAX-2000;powerTick();
  assert(!wifiRadioEnabled && wifiBootAccessComplete);
  monoOffsetMs=0x100000000ULL;tick=100;powerTick();
  assert(!wifiRadioEnabled && wifiBootAccessComplete);
  // Latch also runs while Always-on, before a later switch to Power-save.
  setupPowerSave();wifiBootAccessComplete=false;wifiPowerMode=WIFI_POWER_ALWAYS_ON;
  bootMillis=100;tick=400000;updateWifiPowerManagement();assert(wifiBootAccessComplete);
  wifiPowerMode=WIFI_POWER_SCHEDULED;monoOffsetMs=0x100000000ULL;tick=100;
  powerTick();powerTick();assert(!wifiRadioEnabled);

  // Both station and NTP retry deadlines stay 64-bit across millis wrap.
  setupPowerSave();tick=UINT32_MAX-1000;monoOffsetMs=0;
  ap_mode=true;WiFi.connection=0;wifiStaRetryAtMs=(uint64_t)tick+30000;
  int attempts=WiFi.begins;tick+=2000;monoOffsetMs=0x100000000ULL;
  checkWiFiConnection();assert(WiFi.begins==attempts && webServerStarted);
  tick+=28000;checkWiFiConnection();assert(WiFi.begins==attempts+1 && wifiConnectionPending);

  // Persistent router failures remain bounded; each retry leaves AP access.
  reset();ap_mode=false;radioPaused=false;WiFi.modeValue=WIFI_STA;ntpstart();
  for(int cycle=0;cycle<20;++cycle) {
    tick+=WIFI_CONNECT_TIMEOUT;checkWiFiConnection();
    assert(ap_mode && !wifiConnectionPending && webServerStarted);
    assert(wifiStaRetryDelayMs<=WIFI_RETRY_MAX_MS);
    const uint64_t due=wifiStaRetryAtMs;
    attempts=WiFi.begins;
    for(int i=0;i<50;++i) checkWiFiConnection();
    assert(WiFi.begins==attempts);
    tick=(uint32_t)due;checkWiFiConnection();
    assert(ap_mode && wifiConnectionPending && webServerStarted && WiFi.modeValue==WIFI_AP_STA);
  }
  assert(wifiStaRetryDelayMs==WIFI_RETRY_MAX_MS);
  WiFi.connection=WL_CONNECTED;immediateReply=true;checkWiFiConnection();
  assert(!ap_mode && ntpsync==1 && !radioPaused && wifiStaRetryDelayMs==30000);

  // The reproduced connected-but-unserviced failure now retains web/SNTP
  // and reports desired Off separately from the actual station mode.
  setupPowerSave();ntpEnabled=true;WiFi.disconnectOk=WiFi.offModeOk=false;
  powerTick();
  assert(!wifiRadioEnabled && wifiPowerShutdownPending && wifiPowerFault);
  assert(WiFi.getMode()==WIFI_STA && WiFi.status()==WL_CONNECTED);
  assert(webServerStarted && ntpEnabled && wifiPowerFaultCount==1);
  const int shutdowns=WiFi.disconnects;
  for(int i=0;i<100;++i) { checkWiFiConnection();assert(!ntpstop()); }
  assert(WiFi.disconnects==shutdowns && webServerStarted && ntpEnabled);
  // Every failed attempt has a deadline; the backoff caps at one minute.
  for(int i=0;i<12;++i) {
    monoOffsetMs=wifiPowerRetryAtMs-(uint64_t)tick;
    assert(!ntpstop() && wifiPowerShutdownPending && wifiPowerFault);
    assert(wifiPowerRetryDelayMs<=WIFI_POWER_RETRY_MAX_MS);
    assert(webServerStarted && ntpEnabled);
  }
  assert(wifiPowerRetryDelayMs==WIFI_POWER_RETRY_MAX_MS);
  // Successful observed Off is accepted even if disconnect reported an
  // error; the SDK mode transition actually disabled the radio.
  WiFi.offModeOk=true;monoOffsetMs=wifiPowerRetryAtMs-(uint64_t)tick;
  assert(ntpstop() && WiFi.getMode()==WIFI_OFF && !wifiPowerFault);
  assert(!wifiPowerShutdownPending && !webServerStarted && !ntpEnabled);
  // A user/clock window cancels a pending shutdown; stale retry must not
  // power the newly requested connection back off.
  setupPowerSave();WiFi.disconnectOk=WiFi.offModeOk=false;powerTick();
  assert(wifiPowerShutdownPending);
  WiFi.disconnectOk=WiFi.offModeOk=true;accessWantsWifi=true;powerTick();
  assert(wifiRadioEnabled && !wifiPowerShutdownPending && wifiConnectionPending);
  checkWiFiConnection();assert(!wifiPowerFault && webServerStarted && ntpEnabled);
  tick+=60000;powerTick();assert(wifiRadioEnabled && WiFi.getMode()==WIFI_STA);
  puts("Adaptive independent NTP wakes, bounded failures/recovery, AP retention and 64-bit rollover policy passed");
}
'''

class NetworkReliabilityTests(unittest.TestCase):
    def test_real_network_policy_fault_recovery_and_rollover(self):
        self.assertIsNotNone(shutil.which('g++'))
        source=workflow.FIRMWARE.read_text()
        prefix=workflow.MOCKS.split('struct SerialMock {',1)[0]
        mocks=recovery.MOCKS.replace('bool shouldWifiBeOnForSchedule() { return scheduleWantsWifi; }','')
        names=['configureNtpClient','ntpstart','ntpstop','startAPMode',
               'serviceAPStartup','stopAPMode','beginWifiCredentialConnection',
               'checkWiFiConnection','shouldWifiBeOnForSchedule','wifiClockSyncWindowOpen',
               'updateWifiPowerManagement']
        functions='\n\n'.join(workflow.extract_function(
            source.replace('void\nntpstop(', 'void ntpstop('),name) for name in names)
        unit=prefix+mocks+POLICY_MOCKS+functions+CASES
        with tempfile.TemporaryDirectory(prefix='radioclock-network-reliability-') as temporary:
            cpp=Path(temporary)/'network.cpp';binary=Path(temporary)/'network'
            cpp.write_text(unit)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
                            '-I',str(workflow.FIRMWARE.parent),str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

if __name__=='__main__':
    unittest.main()
