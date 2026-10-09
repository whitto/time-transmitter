#!/usr/bin/env python3
"""Exercise real font transactions, isolated setting changes and idle BLE power."""
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
import test_bluetooth_workflow as workflow

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = workflow.FIRMWARE

FONT_MOCKS = r'''
#include "CasioWatchSettings.h"
#include "CasioBxProtocol.h"
#include <array>
#include <vector>
struct SerialMock {
  void println(const char*) {} void println(const String&) {}
  template<class... T> void printf(const char*, T...) {}
} Serial;
struct NimBLEUUID { std::string value; NimBLEUUID(const char *s): value(s) {} };
struct NimBLERemoteCharacteristic {
  bool subscribeOk=true;
  bool canWrite() { return true; } bool canWriteNoResponse() { return false; }
  bool canNotify() { return true; } bool canIndicate() { return false; }
  bool subscribe(bool, void(*)(NimBLERemoteCharacteristic*,uint8_t*,size_t,bool),bool) { return subscribeOk; }
} requestChar, setChar, spDataChar;
std::atomic<bool> btClientConnected{true}, btClientPreemptionEnabled{true};
bool disconnectDuringDiscovery=false, disconnectAfterTime=true;
struct NimBLERemoteService {
  bool missing=false;
  NimBLERemoteCharacteristic *getCharacteristic(NimBLEUUID) {
    if(disconnectDuringDiscovery)btClientConnected=false;
    return missing?nullptr:&requestChar;
  }
} service;
NimBLERemoteService *btService=&service;
NimBLERemoteCharacteristic *btSetChar=&setChar, *btSpData=&spDataChar;
#define CASIO_BASIC_REQUEST_CHAR "26eb002c-b012-49a8-b1f8-394fb2032b0f"
constexpr int BT_RESPONSE_CAPACITY=512, BT_PROTOCOL_BX5600_MIP=0;
uint8_t btFontMode[4]={0,0,0,0}, btActiveProfile=0;
bool btPairModeActive=false;
String btLastWatchName="CASIO GW-BX5600", btProfileName[4], btFontLastStatus;
const char *btGattStage="idle", *btDeliveryEvidence="none";
int btResponseErrors=0, timeWrites=0, fontWrites=0, requests=0, disconnects=0;
bool cancelled=false, failWrite=false, failWait=false, corruptReadback=false;
bool invalidReply=false, missingByte=false, badLength=false, oversizedReply=false;
bool overflowReply=false, changedReadbackLength=false, corruptOtherSetting=false;
portMUX_TYPE btResponseMux=0;
size_t btResponseLength=0;
bool btResponseOverflow=false;
std::vector<uint8_t> hardware;
std::vector<uint8_t> reply;
std::vector<char> order;
std::vector<std::vector<uint8_t>> timePackets, spPackets;
time_t elapsed=0, sampledEpoch=0;
int sampleTime(struct timeval *tv,void*) {
  order.push_back('C'); sampledEpoch=1704110400+elapsed;
  tv->tv_sec=sampledEpoch; tv->tv_usec=123000; return 0;
}
#define gettimeofday sampleTime
void gshockNotifyCallback(NimBLERemoteCharacteristic*,uint8_t*,size_t,bool) {}
bool readBluetoothWatchBattery() { return false; } // Battery has its own transport/capture tests.
bool bleOperationCancelled() { return cancelled; }
uint32_t tick = 0;
uint32_t millis() { return tick; }
static bool btTransactionCancelled();
void prepareBtResponse(uint8_t header) {
  assert(header==0x13); reply.clear(); btResponseLength=0; btResponseOverflow=false;
}
void cancelBtResponse() {}
bool waitBt(size_t minimum) { return !failWait && !cancelled && reply.size()>=minimum; }
size_t copyBt(uint8_t *out,size_t capacity) {
  const size_t n=std::min(capacity,reply.size()); std::copy_n(reply.begin(),n,out); return n;
}
bool writeBt(NimBLERemoteCharacteristic *c,const uint8_t *data,size_t size,bool response) {
  assert(response);
  if(btTransactionCancelled() || !btClientConnected || !btClientPreemptionEnabled)return false;
  if(c==&requestChar) {
    assert(size==1 && data[0]==0x13); ++requests; order.push_back('R');
    reply=hardware; ++elapsed;
    if(corruptReadback && fontWrites) reply[8]^=0x20;
    if(corruptOtherSetting && fontWrites) reply[5]^=1;
    if(invalidReply) reply[0]=0x12;
    if(missingByte) reply.pop_back();
    if(badLength) reply.resize(16);
    if(oversizedReply) reply.resize(18);
    if(overflowReply) { reply.resize(600); btResponseOverflow=true; }
    if(changedReadbackLength && fontWrites) reply.resize(17);
    btResponseLength=reply.size();
  } else if(c==&setChar && data[0]==0x13) {
    assert(size==hardware.size()); ++fontWrites; order.push_back('F'); ++elapsed;
    if(failWrite)return false;
    std::copy(data,data+size,hardware.begin());
  } else if(c==&setChar) {
    assert(data[0]==0x09 && size==11); ++timeWrites; order.push_back('T');
    timePackets.emplace_back(data,data+size);
    if(disconnectAfterTime)btClientConnected=false; // Manual D session ends at final TIME ACK.
  } else {
    assert(c==&spDataChar && (size==35 || size==94 || size==133));
    spPackets.emplace_back(data,data+size);
  }
  return true;
}
bool connectGShock(int p) { assert(p==0); return true; }
void disconnectGShock() { ++disconnects; }
void bluetoothLocalTime(time_t epoch,struct tm &out) { gmtime_r(&epoch,&out); }
size_t bxRequest(const uint8_t*,size_t,uint8_t header,size_t minimum,uint8_t *out,size_t capacity) {
  const std::vector<uint8_t> *data=header==5?&settingsReply:header==3?&dstReply:&namesReply;
  assert(data->size()>=minimum && data->size()<=capacity);
  std::copy(data->begin(),data->end(),out); return data->size();
}
void reset(size_t size=12) {
  btFontMode[0]=0; btLastWatchName="CASIO GW-BX5600";
  service.missing=false; setChar.subscribeOk=true;
  cancelled=failWrite=failWait=corruptReadback=invalidReply=missingByte=false;
  badLength=oversizedReply=overflowReply=changedReadbackLength=corruptOtherSetting=false;
  disconnectDuringDiscovery=false; disconnectAfterTime=true;
  btClientConnected=true; btClientPreemptionEnabled=true;
  timeWrites=fontWrites=requests=disconnects=0; order.clear();
  timePackets.clear(); spPackets.clear(); elapsed=0; sampledEpoch=0;
  hardware.resize(size);
  for(size_t i=0;i<hardware.size();++i)hardware[i]=uint8_t(i*13+1);
  hardware[0]=0x13; hardware[8]|=0x20;
}
'''

FONT_DRIVER = r'''
int main() {
  // 17-byte web encoder fixtures plus actual 12-byte GW-BX5600 capture values.
  const uint8_t classic[]={CLASSIC_BYTES}, standard[]={STANDARD_BYTES};
  const uint8_t bxClassic[]={0x13,0x05,0x01,0x00,0x01,0x00,0x00,0x00,0x20,0x00,0x00,0x00};
  const uint8_t bxStandard[]={0x13,0x05,0x01,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
  // izivkov/gshock_api ae817310dd9551d5e6e53ee61d89af4e8d954767:
  // btsnoop_hci_bx.log #1295 reply, #1299 font write, #1309 readback, #1317 reverse write.
  uint8_t out[17];
  assert(CasioWatchSettings::buildFont(classic,17,1,out,17));
  assert(!memcmp(out,standard,17));
  assert(CasioWatchSettings::buildFont(standard,17,2,out,17));
  assert(!memcmp(out,classic,17));
  memset(out,0xa5,17);
  assert(CasioWatchSettings::buildFont(bxClassic,12,1,out,17));
  assert(!memcmp(out,bxStandard,12));
  for(size_t i=12;i<17;++i)assert(out[i]==0xa5); // Never pad or write tail bytes.
  assert(CasioWatchSettings::buildFont(bxStandard,12,2,out,12));
  assert(!memcmp(out,bxClassic,12));
  memset(out,0xa5,17);
  assert(!CasioWatchSettings::buildFont(bxClassic,11,1,out,17));
  assert(!CasioWatchSettings::buildFont(bxClassic,12,1,out,11));
  assert(!CasioWatchSettings::buildFont(classic,16,1,out,17));
  assert(!CasioWatchSettings::buildFont(classic,17,0,out,17));
  assert(!CasioWatchSettings::buildFont(classic,17,3,out,17));
  assert(!CasioWatchSettings::buildFont(nullptr,17,1,out,17));
  assert(!CasioWatchSettings::buildFont(classic,17,1,out,16));
  for(auto x:out)assert(x==0xa5);
  reset(); auto before=hardware;
  assert(performGShockBX5600Sync() && timeWrites==1 && requests==0 && fontWrites==0 && disconnects==1);
  assert(hardware==before); // Opt-out preserves the complete previous time-only protocol.
  const auto timeOnlySp=spPackets;
  assert((order==std::vector<char>{'C','T'}));
  for(size_t size:{size_t(12),size_t(17)}) {
    reset(size); before=hardware; btFontMode[0]=1;
    assert(performGShockBX5600Sync() && timeWrites==1 && requests==2 && fontWrites==1);
    assert((hardware[8]&0x20)==0 && (order==std::vector<char>{'R','F','R','C','T'}));
    assert(spPackets==timeOnlySp); // Working SP handshake stays byte-for-byte identical.
    assert(sampledEpoch==1704110403 && timePackets[0][7]==3); // Fresh time after optional work.
    for(size_t i=0;i<size;++i)if(i!=8)assert(before[i]==hardware[i]);
    assert((before[8]&~0x20)==hardware[8] && btFontLastStatus.indexOf("confirmed")>=0);
    reset(size); hardware[8]&=~0x20; btFontMode[0]=2;
    assert(performGShockBX5600Sync() && (hardware[8]&0x20));
    reset(size); btFontMode[0]=2;
    assert(applyBluetoothWatchFont() && requests==1 && fontWrites==0); // Already Classic.
    for(int failure=0;failure<9;++failure) {
      reset(size); btFontMode[0]=1; before=hardware;
      service.missing=failure==0; setChar.subscribeOk=failure!=1; failWrite=failure==2;
      failWait=failure==3; invalidReply=failure==4; missingByte=failure==5;
      badLength=failure==6; oversizedReply=failure==7; overflowReply=failure==8;
      assert(performGShockBX5600Sync() && timeWrites==1 && disconnects==1);
      assert(btFontLastStatus.indexOf("failed")>=0 && hardware==before);
      assert(spPackets==timeOnlySp);
    }
    for(bool changeOther:{false,true}) {
      reset(size); btFontMode[0]=1;
      corruptReadback=!changeOther; corruptOtherSetting=changeOther;
      assert(performGShockBX5600Sync() && timeWrites==1 && fontWrites==1);
      assert(btFontLastStatus.indexOf("failed")>=0); // ATT ACK alone is insufficient font evidence.
    }
  }
  reset(); btFontMode[0]=1; changedReadbackLength=true;
  assert(performGShockBX5600Sync() && timeWrites==1 && fontWrites==1);
  assert(btFontLastStatus.indexOf("failed")>=0); // Readback length must match the original packet.
  reset(); btFontMode[0]=1; btLastWatchName="CASIO GW-B5600";
  assert(!applyBluetoothWatchFont() && requests==0 && fontWrites==0);
  reset(); btFontMode[0]=1; cancelled=true;
  assert(!applyBluetoothWatchFont() && requests==0 && fontWrites==0);
  reset(); btFontMode[0]=1; btClientPreemptionEnabled=false;
  assert(!applyBluetoothWatchFont() && requests==0 && fontWrites==0);
  reset(); btFontMode[0]=1; btClientConnected=false;
  assert(!applyBluetoothWatchFont() && requests==0 && fontWrites==0);
  reset(); btFontMode[0]=1; disconnectDuringDiscovery=true;
  assert(!performGShockBX5600Sync() && timeWrites==0 && requests==0 && fontWrites==0);
  assert(btFontLastStatus.indexOf("disconnected")>=0);
  reset(); btFontMode[0]=1;
  btTransactionStartedMillis.store(UINT32_MAX - 100);
  btTransactionActive.store(true);
  tick = static_cast<uint32_t>(UINT32_MAX - 100 + BT_TRANSACTION_DEADLINE_MS);
  assert(!performGShockBX5600Sync() && timeWrites==0 && fontWrites==0 && requests==0);
  btTransactionActive.store(false);
  std::puts("Captured 12-byte and webapp 17-byte font packets preserved; read/write/readback precedes fresh TIME; font failures stay separate");
}
'''

POWER_DRIVER = r'''
int main() {
  registerRoutes(); btAlwaysWaitEnabled=false; btIdlePowerSaveEnabled=true;
  std::fill(std::begin(btSyncEnabled),std::end(btSyncEnabled),false);
  serviceBluetoothSync(); assert(!btBleInitialized && NimBLEDevice::inits==0 && !radioBleArbiter.bleOwned());
  bind(0); btManualProfile=0;
  server.post("/api/bluetooth-sync"); assert(server.code==200 && btManualSyncRequested);
  serviceBluetoothSync(); assert(btBleInitialized && btWindowActive && radioBleArbiter.bleOwned());
  discover("GW-BX5600"); serviceBluetoothSync(); assert(!btWindowActive);
  serviceBluetoothSync(); assert(!btBleInitialized && !radioBleArbiter.bleOwned());
  // The automatic window wakes without Wi-Fi/HTTP and shuts down after expiry.
  btSyncEnabled[0]=true; btSyncTimes[0]=1321;
  serviceBluetoothSync(); assert(btWindowActive && btSyncActiveSlot==0);
  fakeEpoch+=12*60; tick+=12*60000;
  serviceBluetoothSync(); serviceBluetoothSync(); assert(!btBleInitialized && btSyncEnabled[0]);
  fakeEpoch+=86400-12*60; tick+=86400000-12*60000;
  serviceBluetoothSync(); assert(btWindowActive && btSyncActiveSlot==0); // Recurs next day.
  stopBluetoothWindow(); btSyncEnabled[0]=false;
  btAlwaysWaitEnabled=true; serviceBluetoothSync(); assert(btBleInitialized && btPersistentWaitActive);
  assert(btIdlePowerSaveEnabled); // Always Wait takes precedence without changing either saved preference.
  btAlwaysWaitEnabled=false; stopBluetoothWindow();
  NimBLEDevice::deinitOk=false; serviceBluetoothSync();
  assert(radioBleArbiter.bleOwned() && !radioBleArbiter.requestRf());
  tick+=1001; NimBLEDevice::deinitOk=true; NimBLEDevice::controllerStops=false;
  serviceBluetoothSync(); assert(radioBleArbiter.bleOwned());
  tick+=1001; NimBLEDevice::controllerStops=true; serviceBluetoothSync();
  assert(!radioBleArbiter.bleOwned() && controllerStatus==ESP_BT_CONTROLLER_STATUS_IDLE);
  assert(radioBleArbiter.requestRf()); serviceBluetoothSync(); assert(!btBleInitialized);
  radioBleArbiter.releaseRf();
  btIdlePowerSaveEnabled=false; serviceBluetoothSync(); assert(btBleInitialized);
  std::puts("Idle controller power saving wakes for manual/daily windows, honors Always Wait and retains RF exclusion on failures");
}
'''

CONFIG_DRIVER = r'''
int main() {
  registerRoutes(); btAlwaysWaitEnabled=false;
  std::fill(std::begin(btSyncEnabled),std::end(btSyncEnabled),false);
  server.post("/api/config",{{"bt_font_profile","1"},{"bt_font_mode","1"}});
  assert(server.code==200 && btFontMode[1]==1 && !configDirty);
  btFontMode[1]=0; loadConfig(); assert(btFontMode[1]==1 && btFontMode[0]==0 && !btAlwaysWaitEnabled);
  server.post("/api/config",{{"bt_font_profile","2"},{"bt_font_mode","2"}});assert(server.code==200);
  server.post("/api/config",{{"bt_idle_power_save","true"}});assert(server.code==200 && btIdlePowerSaveEnabled);
  btFontMode[2]=0;btIdlePowerSaveEnabled=false;loadConfig();assert(btFontMode[2]==2 && btIdlePowerSaveEnabled);
  const auto disk=flash.at(CONFIG_FILE); mockRenameOk=false;
  server.post("/api/config",{{"bt_font_profile","1"},{"bt_font_mode","0"}});
  assert(server.code==500 && btFontMode[1]==1 && flash.at(CONFIG_FILE)==disk);
  server.post("/api/config",{{"bt_idle_power_save","false"}});
  assert(server.code==500 && btIdlePowerSaveEnabled && flash.at(CONFIG_FILE)==disk);
  mockRenameOk=true;
  server.post("/api/config",{{"bt_font_profile","4"},{"bt_font_mode","1"}});assert(server.code==400);
  server.post("/api/config",{{"bt_font_profile","1"},{"bt_font_mode","3"}});assert(server.code==400);
  server.post("/api/config",{{"bt_font_mode","1"}});assert(server.code==400);
  server.post("/api/config",{{"bt_idle_power_save","yes"}});assert(server.code==400);
  server.post("/api/config",{{"bt_font_profile","1"},{"bt_font_mode","0"}});assert(server.code==200);
  server.post("/api/config",{{"bt_idle_power_save","false"}});assert(server.code==200);
  btFontMode[1]=1;btIdlePowerSaveEnabled=true;loadConfig();assert(btFontMode[1]==0 && !btIdlePowerSaveEnabled && btFontMode[2]==2);
  assert(!btPairRequested && !btManualSyncRequested);
  std::puts("Per-profile font and power settings persist, validate inputs and roll back failed flash writes");
}
'''

class WatchOptionsTest(unittest.TestCase):
    def run_cpp(self, source):
        self.assertIsNotNone(shutil.which('g++'))
        with tempfile.TemporaryDirectory(prefix='radioclock-options-') as tmp:
            unit,binary=Path(tmp)/'test.cpp',Path(tmp)/'test'
            unit.write_text(source)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(FIRMWARE.parent),str(unit),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_font_transaction_against_github_packets(self):
        source=FIRMWARE.read_text()
        fixtures=json.loads((ROOT/'tests/fixtures/watch-font.json').read_text())['packets']
        driver=FONT_DRIVER
        for name in ['Classic','Standard']:
            driver=driver.replace(name.upper()+'_BYTES',','.join('0x'+fixtures[name][i:i+2] for i in range(0,34,2)))
        sp_source=(ROOT/'tests/test_bx_protocol.cpp').read_text()
        captures=[]
        for name in ['settingsReply','dstReply','namesReply']:
            block=re.search(r'static const Bytes '+name+r' = hex\((.*?)\);',sp_source,re.S).group(1)
            data=bytes.fromhex(''.join(re.findall(r'"([0-9a-f]+)"',block)))
            captures.append('const std::vector<uint8_t> '+name+'={'+','.join(map(str,data))+'};')
        string_mock=workflow.MOCKS.split('struct SerialMock')[0]
        functions='\n'.join(workflow.extract_function(source,n) for n in ['requestBluetoothBasicSettings','applyBluetoothWatchFont','performGShockBX5600Sync'])
        self.run_cpp(string_mock+'\n#include <sys/time.h>\n'+'\n'.join(captures)+FONT_MOCKS+
                     workflow.transaction_deadline_source(source)+functions+driver)

    def test_power_controller_windows_and_failures(self):
        source=workflow.BluetoothWorkflowTest().unit_source(FIRMWARE.read_text())
        helpers='\n'.join(workflow.extract_function(workflow.DRIVER,n) for n in ['bind','discover'])
        self.run_cpp(source.replace(workflow.DRIVER,helpers+POWER_DRIVER,1))

    def test_font_and_power_flash_routes(self):
        mocks=workflow.MOCKS.replace('struct LittleFsMock {','bool mockRenameOk=true;\nstruct LittleFsMock {')
        mocks=mocks.replace('flash[to] = flash.at(from);','if(!mockRenameOk)return false; flash[to] = flash.at(from);')
        source=workflow.BluetoothWorkflowTest().unit_source(FIRMWARE.read_text())
        self.run_cpp(source.replace(workflow.MOCKS,mocks,1).replace(workflow.DRIVER,CONFIG_DRIVER,1))

if __name__=='__main__':unittest.main()
