
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <strings.h>
#include <vector>
#include "RadioBleArbiter.h"
#include "CasioWatchBattery.h"
#include "BtSyncHistory.h"
#include "RadioWifiAccessWindow.h"
#include "RadioConfigWriter.h"
#include "RadioJsonWriter.h"
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
size_t strlcpy(char* to, const char* from, size_t count) {
  if (count) { std::strncpy(to, from, count - 1); to[count - 1] = '\0'; }
  return std::strlen(from);
}
class String {
public:
  std::string s;
  String(const char* p = "") : s(p) {}
  String(std::string p) : s(std::move(p)) {}
  String(int n) : s(std::to_string(n)) {}
  String(unsigned long n) : s(std::to_string(n)) {}
  size_t length() const { return s.length(); }
  const char* c_str() const { return s.c_str(); }
  void reserve(size_t n) { s.reserve(n); }
  void trim() {
    auto first = s.find_first_not_of(" \t\r\n");
    s = first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
  }
  void replace(const char* from, const char* to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, std::strlen(from), to); pos += std::strlen(to);
    }
  }
  void toUpperCase() { for (auto& c : s) c = std::toupper(static_cast<unsigned char>(c)); }
  int indexOf(const char* p) const { auto pos = s.find(p); return pos == std::string::npos ? -1 : int(pos); }
  bool equalsIgnoreCase(const char* p) const { return strcasecmp(s.c_str(), p) == 0; }
  long toInt() const { return std::strtol(s.c_str(), nullptr, 10); }
  void toCharArray(char* out, size_t count) const { strlcpy(out, c_str(), count); }
  String& operator+=(const String& v) { s += v.s; return *this; }
  String& operator+=(char v) { s += v; return *this; }
  friend String operator+(const String& a, const String& b) { return String(a.s + b.s); }
  friend bool operator==(const String& a, const String& b) { return a.s == b.s; }
  friend bool operator!=(const String& a, const String& b) { return !(a == b); }
};

#include <sys/time.h>
const std::vector<uint8_t> settingsReply={5,15,0,29,0,1,6,6,233,118,0,0,255,255,255,255,255,255,15,0,29,2,3,2,0,25,1,0,0,255,255,255,255,255,255,20,0,36,0,1,64,68,186,54,239,128,85,252,64,2,0,115,114,190,99,125,4,20,0,36,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,20,0,36,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2};
const std::vector<uint8_t> dstReply={3,7,0,30,0,233,118,4,4,2,7,0,30,1,0,0,0,0,0,7,0,30,2,25,1,36,4,0};
const std::vector<uint8_t> namesReply={6,20,0,31,0,77,65,68,82,73,68,0,0,0,0,0,0,0,0,0,0,0,0,20,0,31,6,77,65,68,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,20,0,31,1,40,85,84,67,41,0,0,0,0,0,0,0,0,0,0,0,0,0,20,0,31,7,85,84,67,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,20,0,31,2,84,79,75,89,79,0,0,0,0,0,0,0,0,0,0,0,0,0,20,0,31,8,84,89,79,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
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
bool trustedClock=true; bool clockTrusted() { return trustedClock; }
bool readBluetoothWatchBattery() { trustedClock=false; return false; } // Battery has its own transport/capture tests.
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
static std::atomic<bool> btTransactionActive{false};
static std::atomic<uint32_t> btTransactionStartedMillis{0};
static constexpr uint32_t BT_TRANSACTION_DEADLINE_MS = 45000;
static bool btTransactionDeadlineReached(void) {
  return btTransactionActive.load(std::memory_order_acquire) &&
         static_cast<uint32_t>(millis() - btTransactionStartedMillis.load(std::memory_order_acquire)) >=
           BT_TRANSACTION_DEADLINE_MS;
}
static bool btTransactionCancelled(void) {
  return bleOperationCancelled() || btTransactionDeadlineReached();
}
static bool requestBluetoothBasicSettings(NimBLERemoteCharacteristic *request,
                                          uint8_t *reply, size_t &size) {
  size = 0;
  if (!request || !reply || btTransactionCancelled() ||
      !btClientPreemptionEnabled.load(std::memory_order_acquire) ||
      !btClientConnected.load(std::memory_order_acquire)) return false;
  const uint8_t command[] = {CasioWatchSettings::kBasicHeader};
  prepareBtResponse(command[0]);
  if (!writeBt(request, command, sizeof(command), request->canWrite())) {
    cancelBtResponse(); return false;
  }
  if (!waitBt(CasioWatchSettings::kBxBasicSize)) return false;
  // Both observed layouts are complete packets. Reject excess bytes before
  // copying so a truncated prefix can never pass as a valid settings reply.
  portENTER_CRITICAL(&btResponseMux);
  const size_t received = btResponseLength;
  const bool overflow = btResponseOverflow;
  portEXIT_CRITICAL(&btResponseMux);
  if (overflow || !CasioWatchSettings::validBasicSize(received) ||
      btTransactionCancelled() ||
      !btClientPreemptionEnabled.load(std::memory_order_acquire) ||
      !btClientConnected.load(std::memory_order_acquire)) return false;
  size = copyBt(reply, BT_RESPONSE_CAPACITY);
  Serial.printf("BT: %s response header=0x%02x bytes=%u\n", btGattStage,
                size ? reply[0] : 0, (unsigned)size);
  return size == received && reply[0] == command[0];
}
static bool applyBluetoothWatchFont(void) {
  const uint8_t mode = btFontMode[btActiveProfile];
  if (!mode) { btFontLastStatus = "No font override requested"; return true; }
  String name = btLastWatchName.length() ? btLastWatchName :
                btPairModeActive ? String("") : btProfileName[btActiveProfile];
  name.toUpperCase();
  if (name.indexOf("GW-BX5600") < 0 || !btService || !btSetChar) {
    btFontLastStatus = "Font skipped: a GW-BX5600 must be identified";
    Serial.println(btFontLastStatus);
    return false;
  }
  const auto ready = []() {
    return !btTransactionCancelled() &&
           btClientPreemptionEnabled.load(std::memory_order_acquire) &&
           btClientConnected.load(std::memory_order_acquire);
  };
  const auto failed = [](const char *reason) {
    btFontLastStatus = String("Font update failed at ") + btGattStage + ": " + reason;
    Serial.println(btFontLastStatus);
    return false;
  };
  btGattStage = "BX font discovery";
  if (!ready()) return failed("watch disconnected or RF cancelled the operation");
  auto request = btService->getCharacteristic(NimBLEUUID(CASIO_BASIC_REQUEST_CHAR));
  if (!ready()) return failed("watch disconnected or RF cancelled the operation");
  if (!request || (!request->canWrite() && !request->canWriteNoResponse()))
    return failed("basic-settings request characteristic unavailable");
  btGattStage = "BX font subscription";
  bool subscribed = false;
  if (btSetChar->canNotify()) subscribed = btSetChar->subscribe(true, gshockNotifyCallback, true);
  else if (btSetChar->canIndicate()) subscribed = btSetChar->subscribe(false, gshockNotifyCallback, true);
  if (!ready()) return failed("watch disconnected or RF cancelled the operation");
  if (!subscribed) return failed("settings notification subscription rejected");
  uint8_t reply[BT_RESPONSE_CAPACITY], packet[CasioWatchSettings::kBasicSize];
  size_t size = 0;
  btGattStage = "BX font read settings";
  if (!requestBluetoothBasicSettings(request, reply, size) ||
      !CasioWatchSettings::buildFont(reply, size, mode, packet, sizeof(packet)))
    return failed("missing, incomplete or unsupported settings reply");
  if (memcmp(reply, packet, size) != 0) {
    const size_t writtenSize = size;
    btGattStage = "BX font write settings";
    if (!writeBt(btSetChar, packet, writtenSize, true))
      return failed("settings write was not acknowledged");
    btGattStage = "BX font readback";
    if (!requestBluetoothBasicSettings(request, reply, size) ||
        size != writtenSize || memcmp(reply, packet, writtenSize) != 0)
      return failed("settings readback did not match the requested font packet");
  }
  btFontLastStatus = String("Watch ") + (btActiveProfile + 1) + ": " +
                     (mode == 2 ? "Classic" : "Standard (new)") +
                     " font confirmed by settings readback; display unverified";
  Serial.println(btFontLastStatus);
  return true;
}
bool performGShockBX5600Sync(void) {
  if (!connectGShock(BT_PROTOCOL_BX5600_MIP)) return false;
  // The manual D-button captures read condition before the SP handshake.
  // A battery failure is optional; the required time handshake still runs.
  readBluetoothWatchBattery();
  uint8_t r[BT_RESPONSE_CAPACITY];
  uint8_t cities[CasioBxProtocol::kCitiesSize];
  uint8_t settings[CasioBxProtocol::kSettingsWriteSize];
  uint8_t dst[CasioBxProtocol::kDstWriteSize];
  bool ok = false;

  do {
    btGattStage = "BX step 1 settings";
    const uint8_t req1[] = {0x05,0x1D,0x00,0x1D,0x00,0x24,0x00,0x24,0x01,0x24,0x02};
    size_t n1 = bxRequest(req1, sizeof(req1), 0x05, 101, r, sizeof(r));
    if (!n1) break;
    if (!CasioBxProtocol::splitSettings(r, n1, settings, sizeof(settings), cities, sizeof(cities))) {
      Serial.println("BT: BX step 1 malformed settings/city records");
      ++btResponseErrors;
      break;
    }
    // Send only the two 0x1D records; save the three 0x24 city records for step 2.
    if (!writeBt(btSpData, settings, sizeof(settings), true)) break;

    btGattStage = "BX step 2 DST/cities";
    const uint8_t req2[] = {0x03,0x1E,0x00,0x1E,0x00,0x1E,0x00};
    size_t n2 = bxRequest(req2, sizeof(req2), 0x03, 28, r, sizeof(r));
    if (!n2) break;
    if (!CasioBxProtocol::buildDst(r, n2, cities, sizeof(cities), dst, sizeof(dst))) {
      Serial.println("BT: BX step 2 malformed DST/city records");
      ++btResponseErrors;
      break;
    }
    if (!writeBt(btSpData, dst, sizeof(dst), true)) break;

    btGattStage = "BX step 3 alarms";
    uint8_t req3[13] = {0x06};
    for (int i = 0; i < 6; ++i) {
      int idx = (i / 2) + ((i % 2) ? 6 : 0);
      req3[1 + i * 2] = 0x1F;
      req3[2 + i * 2] = (uint8_t)idx;
    }
    size_t n3 = bxRequest(req3, sizeof(req3), 0x06, 133, r, sizeof(r));
    if (!n3) break;
    if (!CasioBxProtocol::validAlarms(r, n3)) {
      Serial.println("BT: BX step 3 malformed alarm records");
      ++btResponseErrors;
      break;
    }
    if (!writeBt(btSpData, r, n3, true)) break;

    // A watch-triggered time session can close immediately after TIME is
    // acknowledged. Apply optional settings first; a font failure alone must
    // not prevent a time attempt. Sample time only after this optional work.
    applyBluetoothWatchFont();
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    time_t epoch = tv.tv_sec;
    struct tm local;
    bluetoothLocalTime(epoch, local);
    int year = local.tm_year + 1900;
    uint8_t tc[11] = {
      0x09,(uint8_t)(year&255),(uint8_t)((year>>8)&255),
      (uint8_t)(local.tm_mon+1),(uint8_t)local.tm_mday,
      (uint8_t)local.tm_hour,(uint8_t)local.tm_min,(uint8_t)local.tm_sec,
      (uint8_t)(local.tm_wday==0?7:local.tm_wday),
      (uint8_t)((tv.tv_usec/1000UL*256UL)/1000UL),0x01
    };
    btGattStage = "BX step 4 TIME";
    if (!writeBt(btSetChar, tc, sizeof(tc), true)) break;

    ok = true;
    btDeliveryEvidence = "ATT write acknowledged; watch display unverified";
    Serial.printf("BT: GW-BX5600 time command delivered %04d-%02d-%02d %02d:%02d:%02d\n",
                  year, local.tm_mon+1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
  } while (false);

  disconnectGShock();
  btGattStage = "idle";
  return ok;
}
int main() {
  reset();
  assert(clockTrusted()); // Entry trust check succeeds.
  const bool success=performGShockBX5600Sync();
  assert(!clockTrusted()); // Injected expiry during optional battery work.
  assert(success && timeWrites == 1);
  std::printf("Actual BX transaction acknowledged TIME while clockTrusted=false after transaction entry; success=%d TIME writes=%d.\n", success, timeWrites);
}
