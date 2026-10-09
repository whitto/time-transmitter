#!/usr/bin/env python3
"""Exercise shipped GET routes/helpers with a fixed-buffer HTTP boundary.

The route bodies and both shared encoders come directly from the active sketch;
only radio/network/flash transport is mocked. Python parses every emitted JSON
response, while native sanitizers and allocation accounting exercise repeated
polls. Hardware heap/SDK soak testing remains separate.
"""
import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
try:
    from . import test_bluetooth_workflow as workflow
except ImportError:
    import test_bluetooth_workflow as workflow

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/RadioClock_V4_15/RadioClock_V4_15.ino'

EXTRAS = r'''
#define FIRMWARE_VERSION "V4.15"
#define FIRMWARE_BUILD "R1"
constexpr int MAX_SCHEDULES=24, SN_DCF77=3, SN_MSF=4;
constexpr int HTTP_GET=0;
std::atomic<int> last_station{0};
std::atomic<bool> radioPaused{false}, rfSilenceFailed{false};
std::atomic<uint32_t> radioMissedBoundaries{0}, carrierFrequencyHz{40000}, ntpIntervalSec{3600};
int radioLogMux=0,clockMux=0;
int radioApplicableCountSnapshot=24, radioApplicableSchedulesSnapshot[MAX_SCHEDULES];
double ntpDriftPpm=-15.75, estimatedError=0.125;
bool ap_mode=false, radioFault=false;
uint32_t ntpAge=30;
bool radioFaultActive(){return radioFault;}
bool wifiAccessWindowOpen(){return true;}
uint64_t monotonicUptimeSeconds(){return 6307200000ULL;}
uint32_t clockAgeSeconds(){return ntpAge;}
double clockEstimatedError(){return estimatedError;}
const char* clockConfidence(){return trustedClock?"Synchronized":"Unsynchronized";}
void stationTime(time_t utc, int, bool next, struct tm& out){utc+=next?60:0;localtime_r(&utc,&out);}
struct EspApiMock { uint32_t getFreeHeap(){return 120000;} } ESP;
constexpr int NUM_STATIONS=7;
const char* station_names[NUM_STATIONS]={"JJY East", "JJY West", "WWVB", "DCF77", "MSF", "BPC", "BSF é 🕒"};
const char* stationEncodingName(int id){return id==0?"JJY \"40 kHz\"":"Time + date";}

struct alignas(std::max_align_t) AllocationHeader { size_t size; };
static size_t liveAllocationBytes=0, liveAllocationCount=0;
static bool forbidAllocation=false;
static const char* allocationPhase="none";
void* operator new(size_t size){
  if(forbidAllocation){std::fprintf(stderr,"allocation in %s: %zu bytes\n",allocationPhase,size);std::abort();}
  auto* header=static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader)+size));
  if(!header)throw std::bad_alloc();header->size=size;liveAllocationBytes+=size;++liveAllocationCount;return header+1;
}
void operator delete(void* memory) noexcept {
  if(!memory)return;auto* header=static_cast<AllocationHeader*>(memory)-1;
  liveAllocationBytes-=header->size;--liveAllocationCount;std::free(header);
}
void operator delete(void* memory,size_t) noexcept {operator delete(memory);}
'''

HTTP_MOCK = r'''
struct ServerMock {
  std::map<std::string,std::function<void()>> routes;
  std::map<std::string,String> args;
  int code=0;size_t contentLength=0,used=0;
  char response[8192]={};
  bool hasArg(const char* key)const{return args.count(key);}
  String arg(const char* key)const{auto i=args.find(key);return i==args.end()?String():i->second;}
  bool checkedArg(const char* key,String& result)const{auto i=args.find(key);if(i==args.end())return false;result=i->second;return result.length()==i->second.length();}
  void setContentLength(size_t length){contentLength=length;}
  void send(int status,const char*,const String& data){code=status;used=data.length();assert(used<sizeof(response));std::memcpy(response,data.c_str(),used+1);}
  void sendContent(const char* data,size_t length){assert(used+length<sizeof(response));std::memcpy(response+used,data,length);used+=length;response[used]=0;assert(used==contentLength);}
  void on(const char* path,int,std::function<void()> handler){routes[path]=handler;}
  void get(const char* path){code=0;used=0;routes.at(path)();assert(code);}
  void post(const char* path,std::map<std::string,String> values={}){args=std::move(values);code=0;used=0;routes.at(path)();assert(code);}
} server;
'''

DRIVER = r'''
static void emit(const char* label){std::printf("%s\t%d\t%s\n",label,server.code,server.response);}
int main(){
  setenv("TZ","UTC0",1);tzset();registerRoutes();registerApiRoutes();
  for(int i=0;i<BT_WATCH_PROFILE_COUNT;i++){
    char address[]="11:22:33:44:55:00";address[16]=static_cast<char>('0'+i);
    btProfileAddress[i]=address;btProfileName[i]=std::string(48,'\x01');
    btProfileProtocol[i]=i==1?BT_PROTOCOL_STANDARD:BT_PROTOCOL_BX5600_MIP;
    btFontMode[i]=i%3;btBatteryReadings[i].available=true;btBatteryReadings[i].percent=i*30;
    btBatteryReadings[i].sampledUtc=fakeEpoch-5;strlcpy(btBatteryReadings[i].address,address,18);
    btBatteryStatuses[i]="Battery: read \"OK\"\n é 🕒";
    btSyncTimes[i]=30+360*i;btSyncEnabled[i]=i%2==0;btSyncProtocol[i]=i%3;btSyncProfile[i]=i;
    assert(btRecentSyncs.bind(i,address,btProfileProtocol[i]));
    for(int j=0;j<5;++j) assert(btRecentSyncs.record(i,address,btProfileProtocol[i],static_cast<uint32_t>(fakeEpoch+j),j%2==0));
  }
  btProfileName[3]="Watch \"four\"\\\n\t\r\b\f\x01\x1f é 🕒";
  std::memset(ssid,'\x02',63);ssid[63]=0;
  btLastSyncStatus="Time delivered \"ack\"\\\n\t\r\b\f\x01\x1f é 🕒";
  btLastSyncDate="2024-01-01 22:00:00";btLastSyncEpoch=fakeEpoch;btLastOutcomeSuccessful=true;
  btFontLastStatus="New font verified";btHistoryPersistEnabled=true;
  btHistorySavedEpoch=fakeEpoch;btHistorySnapshotGeneration=btHistoryGeneration=7;
  btHistorySaveStatus="First daily snapshot saved";btManualProfile=0;btManualProtocol=0;
  btProfileSuccessYear[0]=2024;btProfileSuccessYday[0]=0;btSyncDayComplete=true;
  wifiAccessEnabled=true;wifiAccessStart=1350;wifiAccessEnd=75;activityLedEnabled=false;
  btConnectionAttempts=17;btWriteAcknowledgements=23;btResponseErrors=2;
  for(int i=0;i<MAX_SCHEDULES;i++){
    schedules[i]={i*30,i*30+60,i%NUM_STATIONS};radioApplicableSchedulesSnapshot[i]=i;
  }
  schedule_count=MAX_SCHEDULES;setBluetoothPhase("Idle");
  server.get("/api/config");assert(server.code==200);emit("config");
  assert(radioBleArbiter.requestRf());
  server.get("/api/status");assert(server.code==200);emit("status");
  server.get("/api/stations");assert(server.code==200);emit("stations");
  server.get("/api/schedules");assert(server.code==200);emit("schedules");
  radioBleArbiter.releaseRf();btBleBusy=true;setBluetoothPhase("Syncing \"watch\"\n");
  server.get("/api/config");emit("busy");btBleBusy=false;
  btPairRequested=true;server.get("/api/config");emit("pairing");btPairRequested=false;
  filesystemAvailable=false;configStorageFault=true;ap_mode=true;
  server.get("/api/status");emit("storage-fault");
  filesystemAvailable=true;configStorageFault=false;ap_mode=false;
  ntpAge=UINT32_MAX;estimatedError=NAN;radioFault=true;assert(radioBleArbiter.requestRf());
  server.get("/api/status");emit("unknown-clock");ntpAge=30;estimatedError=0.125;radioFault=false;
  radioPaused=true;server.get("/api/status");emit("paused");radioPaused=false;
  last_station=SN_DCF77;server.get("/api/status");emit("next-minute");last_station=0;radioBleArbiter.releaseRf();
  btHistorySnapshotGeneration=6;server.get("/api/config");emit("history-invalid");btHistorySnapshotGeneration=7;
  btBatteryReadings[0].address[0]='9';server.get("/api/config");emit("battery-mismatch");btBatteryReadings[0].address[0]='1';
  // Oversized/unexpected state fails closed: never send the partial buffer.
  btProfileName[0]=std::string(9000,'x');server.get("/api/config");assert(server.code==503);emit("overflow-config");
  server.get("/api/status");assert(server.code==503);emit("overflow-status");btProfileName[0]=std::string(48,'\x01');
  radioApplicableCountSnapshot=MAX_SCHEDULES+1;server.get("/api/status");assert(server.code==503);emit("invalid-snapshot");radioApplicableCountSnapshot=MAX_SCHEDULES;
  schedule_count=MAX_SCHEDULES+1;server.get("/api/schedules");assert(server.code==503);emit("invalid-count");schedule_count=MAX_SCHEDULES;
  // Route storage and callbacks are allocated once. Warm libc time state,
  // then prove repeated actual encoders retain no heap and need no C++ new.
  auto& configRoute=server.routes.at("/api/config");auto& statusRoute=server.routes.at("/api/status");
  auto& scheduleRoute=server.routes.at("/api/schedules");auto& stationsRoute=server.routes.at("/api/stations");
  configRoute();statusRoute();scheduleRoute();stationsRoute();
  const auto beforeBytes=liveAllocationBytes,beforeCount=liveAllocationCount;
  forbidAllocation=true;
  for(int i=0;i<20000;i++){
    allocationPhase="config";configRoute();assert(server.code==200);allocationPhase="status";statusRoute();assert(server.code==200);
    allocationPhase="schedules";scheduleRoute();assert(server.code==200);allocationPhase="stations";stationsRoute();assert(server.code==200);
  }
  forbidAllocation=false;assert(liveAllocationBytes==beforeBytes&&liveAllocationCount==beforeCount);
  std::puts("stress\t200\t{\"cycles\":20000,\"responses\":80000,\"retained_allocation_growth\":0}");
}
'''

class ApiReliabilityTest(unittest.TestCase):
    def test_actual_ssid_admission_preserves_prior_credentials(self):
        source=FIRMWARE.read_text()
        start=source.index('    const bool ssidRequested=server.hasArg("ssid");',source.index('server.on("/api/config", HTTP_POST'))
        end=source.index('    if (server.hasArg("timezone")) {',start)
        admission=source[start:end]
        driver=r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
size_t strlcpy(char* to,const char* from,size_t count){if(count){std::strncpy(to,from,count-1);to[count-1]=0;}return std::strlen(from);}
struct String{std::string s;String(const char* p=""):s(p){}String(std::string p):s(std::move(p)){}size_t length()const{return s.size();}const char* c_str()const{return s.c_str();}
int indexOf(const char* p)const{auto n=s.find(p);return n==s.npos?-1:int(n);}void toCharArray(char* out,size_t size)const{strlcpy(out,c_str(),size);}friend bool operator!=(const String&a,const String&b){return a.s!=b.s;}};
struct ServerMock{String value,password;int code=200;bool passwordRequested=false,failSsidCopy=false,failPasswordCopy=false;
bool hasArg(const char* key){return !strcmp(key,"ssid") || passwordRequested;}
bool checkedArg(const char* key,String& out){bool isSsid=!strcmp(key,"ssid");if(isSsid?failSsidCopy:failPasswordCopy)return false;out=isSsid?value:password;return true;}
void send(int n,const char*,const char*){code=n;}}server;
char ssid[64]="previous-network",passwd[64]="previous-password";bool wifiChanged=false;
void handleSsid(){ADMISSION}
void reset(const std::string& value){server.value=String(value);server.code=200;wifiChanged=false;server.passwordRequested=false;server.failSsidCopy=false;server.failPasswordCopy=false;strlcpy(ssid,"previous-network",sizeof(ssid));strlcpy(passwd,"previous-password",sizeof(passwd));}
int main(){for(const std::string& value:{std::string(32,'x'),std::string("éééééééééééééééé"),std::string("😀😀😀😀😀😀😀😀")}){reset(value);handleSsid();assert(server.code==200&&wifiChanged&&std::string(ssid)==value);}
for(const std::string& value:{std::string(33,'x'),std::string("ééééééééééééééééé"),std::string("😀😀😀😀😀😀😀😀x"),std::string("a\0b",3),std::string("a\nb"),std::string("a\rb")}){reset(value);handleSsid();assert(server.code==400&&!wifiChanged&&std::string(ssid)=="previous-network");}
reset("new-network");server.failSsidCopy=true;handleSsid();assert(server.code==503&&!wifiChanged&&std::string(ssid)=="previous-network");
reset("new-network");server.passwordRequested=true;server.failPasswordCopy=true;handleSsid();assert(server.code==503&&!wifiChanged&&std::string(ssid)=="previous-network"&&std::string(passwd)=="previous-password");
reset("new-network");server.passwordRequested=true;server.password=String(std::string(64,'x'));handleSsid();assert(server.code==400&&!wifiChanged&&std::string(ssid)=="previous-network"&&std::string(passwd)=="previous-password");
reset("new-network");server.passwordRequested=true;server.password=String("new-password");handleSsid();assert(server.code==200&&wifiChanged&&std::string(ssid)=="new-network"&&std::string(passwd)=="new-password");
std::puts("Actual API SSID UTF-8 byte limits preserve prior credentials PASS");}
'''.replace('ADMISSION',admission)
        with tempfile.TemporaryDirectory(prefix='radioclock-ssid-admission-') as tmp:
            src,binary=Path(tmp)/'ssid.cpp',Path(tmp)/'ssid'
            src.write_text(driver)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-fno-omit-frame-pointer','-no-pie','-O1',str(src),'-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('preserve prior credentials PASS',result.stdout)

    def test_actual_get_routes_parse_and_stress(self):
        source=FIRMWARE.read_text()
        unit=workflow.BluetoothWorkflowTest().unit_source(source)
        mocks=workflow.MOCKS
        begin=mocks.index('struct ServerMock {')
        end=mocks.index('} server;',begin)+len('} server;')
        mocks=mocks[:begin]+HTTP_MOCK.strip()+mocks[end:]
        mocks=mocks.replace('struct Schedule { int start_min, end_min; } schedules[4]{};',
                            'struct Schedule { int start_min,end_min,station; } schedules[24]{};')
        mocks=mocks.replace('struct WiFiMock { int status() const { return WL_CONNECTED; } } WiFi;',
            'struct IpMock { const char* address; String toString()const{return address;} };\nstruct WiFiMock { int status()const{return WL_CONNECTED;} IpMock localIP(){return {"10.0.1.137"};} IpMock softAPIP(){return {"192.168.4.1"};} } WiFi;')
        mocks=mocks.replace('struct LittleFsMock {','struct LittleFsMock {\n size_t totalBytes(){return 917504;} size_t usedBytes(){return 8192;}')
        mocks=mocks.replace('int full_time_station = SN_JJY_E, transmission_offset_minutes = 0, wifiPowerMode = 0;',
                            'int full_time_station=SN_JJY_E,wifiPowerMode=0;std::atomic<int> transmission_offset_minutes{0};')
        # Arduino String compares const-char literals directly; the shared
        # minimal workflow mock normally converts them to temporary Strings.
        mocks=mocks.replace('friend bool operator==(const String& a, const String& b)',
            'friend bool operator==(const String& a,const char* b){return a.s==b;}\n  friend bool operator==(const String& a, const String& b)')
        mocks=mocks.replace('#include <algorithm>','#include <algorithm>\n#include <new>\n#include <cstddef>')
        helper='\n'.join(workflow.extract_function(source,name) for name in ['appendBluetoothProfiles','appendRecentBluetoothSyncs','appendCommonSettings'])
        routes=[]
        for path in ['/api/config','/api/status','/api/stations','/api/schedules']:
            start=source.index(f'server.on("{path}", HTTP_GET, []() {{')
            routes.append(workflow.balanced_block(source,start)+');')
        extras=EXTRAS
        for name in ['FIRMWARE_VERSION','FIRMWARE_BUILD']:
            actual=re.search(r'^#define '+name+r'[^\n]*',source,re.M).group(0)
            extras=re.sub(r'^#define '+name+r'[^\n]*',actual,extras,flags=re.M)
        maximum=int(re.search(r'^#define MAX_SCHEDULES\s+(\d+)',source,re.M).group(1))
        self.assertEqual(maximum,24)
        replacement=extras+'\n'+helper+'\nvoid registerApiRoutes(){\n'+'\n'.join(routes)+'\n}\n'+DRIVER
        unit=unit.replace(workflow.MOCKS,mocks,1).replace(workflow.DRIVER,replacement,1)
        with tempfile.TemporaryDirectory(prefix='radioclock-api-reliability-') as tmp:
            src,binary=Path(tmp)/'api.cpp',Path(tmp)/'api'
            src.write_text(unit)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-O1',
                '-I',str(FIRMWARE.parent),str(src),'-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
        replies={}
        for line in result.stdout.splitlines():
            name,code,body=line.split('\t',2)
            replies[name]=(int(code),json.loads(body))
        config=replies['config'][1]
        common_keys={'timezone','full_time_tx','full_time_station','transmission_offset_minutes',
            'bt_timezone','bt_time_offset_minutes','wifi_power_mode','bt_day_complete',
            'bt_manual_protocol','bt_manual_profile','bt_active_protocol','bt_active_profile',
            'bt_last_sync_status','bt_last_sync_date','bt_always_wait','activity_led_enabled',
            'bt_idle_power_save','bt_font_status','bt_state','bt_pairing','bt_history_persist',
            'bt_history_saved_at','bt_history_save_status','bt_last_sync_epoch','bt_last_outcome_successful',
            'wifi_access_enabled','wifi_access_start','wifi_access_end','wifi_access_timezone',
            'wifi_access_active','crash_dump_enabled','crash_dump_available','filesystem_available',
            'config_storage_fault','firmware_version','firmware_build','bt_profiles','bt_times'}
        self.assertTrue(common_keys <= config.keys(),'all existing UI settings/status fields retained')
        self.assertFalse(config['crash_dump_enabled']);self.assertTrue(config['crash_dump_available'])
        self.assertTrue(config['filesystem_available']);self.assertFalse(config['config_storage_fault'])
        self.assertEqual(config['ssid'],'\x02'*63)
        self.assertEqual(config['firmware_version'],'V4.15')
        self.assertEqual(config['firmware_build'],'R1')
        self.assertEqual(config['bt_recent_sync_profile'],0)
        recent=config['bt_recent_syncs']
        self.assertEqual(len(recent),4)
        self.assertEqual([row['successful'] for row in recent],[True,False,True,False])
        self.assertTrue(all(row['protocol']==0 and row['date'] for row in recent))
        self.assertTrue(all(recent[i]['utc_epoch']>recent[i+1]['utc_epoch'] for i in range(3)))
        self.assertEqual(config['bt_last_sync_status'],'Time delivered "ack"\\\n\t\r\b\f\x01\x1f é 🕒')
        self.assertEqual(config['bt_last_sync_date'],'2024-01-01 22:00:00')
        self.assertEqual(config['bt_history_saved_at'],'2024-01-01 22:00:00')
        self.assertFalse(config['activity_led_enabled'])
        self.assertTrue(config['wifi_access_active'])
        self.assertEqual((config['wifi_access_start'],config['wifi_access_end']),(1350,75))
        profiles=config['bt_profiles'];self.assertEqual(len(profiles),4)
        self.assertEqual(profiles[0]['name'],'\x01'*48)
        self.assertEqual(profiles[0]['battery_percent'],0)
        self.assertIsNone(profiles[1]['battery_percent'])
        self.assertEqual(profiles[2]['battery_percent'],60)
        self.assertIn('é 🕒',profiles[3]['name'])
        self.assertEqual(profiles[0]['battery_read_at'],'2024-01-01 21:59:55')
        self.assertTrue(profiles[0]['done_today'])
        self.assertEqual(len(config['bt_times']),4)
        self.assertEqual([x['minute'] for x in config['bt_times']],[30,390,750,1110])
        self.assertEqual([x['enabled'] for x in config['bt_times']],[True,False,True,False])
        status=replies['status'][1]
        self.assertTrue(common_keys <= status.keys(),'status retains the complete UI settings contract')
        self.assertEqual(status['applicable_schedules'],list(range(24)))
        self.assertEqual(status['uptime_sec'],6307200000)
        self.assertEqual(status['ntp_drift_ppm'],-15.75)
        self.assertTrue(status['radio_active'])
        self.assertEqual(status['bt_state'],'Paused for radio transmission')
        self.assertEqual((status['bt_connection_attempts'],status['bt_acked_writes'],status['bt_response_errors']),(17,23,2))
        for key in ['ntp_age_sec','clock_error_est_sec','heap_free','missed_second_boundaries','littlefs_total','littlefs_used','carrier_hz','rf_silence_failed','radio_fault','firmware_build','filesystem_available','config_storage_fault']:
            self.assertIn(key,status)
        self.assertEqual(len(replies['stations'][1]),7)
        self.assertIn('é 🕒',replies['stations'][1][-1]['name'])
        self.assertEqual(len(replies['schedules'][1]),24)
        self.assertEqual(replies['schedules'][1][-1],{'station':2,'start':690,'end':750})
        self.assertEqual(replies['busy'][1]['bt_state'],'Syncing "watch"\n')
        self.assertEqual(replies['pairing'][1]['bt_state'],'Pairing')
        fault=replies['storage-fault'][1]
        self.assertFalse(fault['filesystem_available']);self.assertTrue(fault['config_storage_fault']);self.assertTrue(fault['ap_mode'])
        self.assertEqual((fault['littlefs_total'],fault['littlefs_used']),(0,0))
        unknown=replies['unknown-clock'][1]
        self.assertEqual(unknown['ntp_age_sec'],-1);self.assertIsNone(unknown['clock_error_est_sec']);self.assertTrue(unknown['radio_fault']);self.assertFalse(unknown['radio_active'])
        self.assertFalse(replies['paused'][1]['radio_active'])
        self.assertTrue(replies['next-minute'][1]['tx_next_minute'])
        self.assertEqual(replies['next-minute'][1]['tx_time'],'2024-01-01 12:01:00')
        self.assertEqual(replies['history-invalid'][1]['bt_history_saved_at'],'')
        self.assertIsNone(replies['battery-mismatch'][1]['bt_profiles'][0]['battery_percent'])
        for name in ['overflow-config','overflow-status','invalid-snapshot','invalid-count']:
            self.assertEqual(replies[name][0],503)
            self.assertEqual(replies[name][1]['status'],'error')
            self.assertNotIn('bt_profiles',replies[name][1],'overflow must return an error instead of partial status JSON')
        self.assertEqual(replies['stress'][1],{'cycles':20000,'responses':80000,'retained_allocation_growth':0})
        print('Actual bounded GET API checks passed: four profiles, 24 schedules, UTF-8/control quoting, build/history/battery/fault states, overflow 503 and 80,000 responses without retained host allocation growth.')

if __name__=='__main__':unittest.main()
