
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
struct FailAllocator {
 void* allocate(size_t) { return nullptr; }
 void deallocate(void* p) { std::free(p); }
 void* reallocate(void*,size_t) { return nullptr; }
};
std::map<std::string,std::string> flash;
struct File {
 std::string *p=nullptr;
 operator bool() const { return p; }
 size_t write(uint8_t c) { p->push_back(char(c)); return 1; }
 size_t write(const uint8_t* b,size_t n) { p->append(reinterpret_cast<const char*>(b),n);return n; }
 void flush() {}
 void close() {}
};
struct Fs {
 File open(const char* path,const char*) { flash[path]=""; return {&flash[path]}; }
 bool rename(const char* from,const char* to) { flash[to]=flash[from];flash.erase(from);return true; }
} LittleFS;
struct SerialMock { void println(const char*) {} } Serial;
struct TimeSchedule { int station,start_min,end_min; } schedules[1]={{0,390,395}};
int schedule_count=1;
bool configDirty=false;
void saveConfig() { configDirty=true; }
void radioRequestRefresh() {}
const char *SCHEDULE_TEMP_FILE="schedule.tmp",*STATION_CONFIG_FILE="schedule.json";
bool saveSchedules(void)
{
  File f=LittleFS.open(SCHEDULE_TEMP_FILE,"w");
  if (!f) { Serial.println("Schedule save failed: temp open"); return false; }
  BasicJsonDocument<FailAllocator> doc(8192);
  JsonArray a=doc.to<JsonArray>();
  for (int i=0;i<schedule_count;++i) {
    JsonObject o=a.createNestedObject();
    o["station"]=schedules[i].station;
    o["start"]=schedules[i].start_min;
    o["end"]=schedules[i].end_min;
  }
  size_t expected=measureJson(doc),written=serializeJson(doc,f);
  f.flush(); f.close();
  if (expected!=written || !LittleFS.rename(SCHEDULE_TEMP_FILE,STATION_CONFIG_FILE)) {
    Serial.println("Schedule save failed: previous file retained"); return false;
  }
  // Saving RF schedules must retain the user's recurring Bluetooth settings.
  // Legacy overlapping Bluetooth slots are only reported during boot.
  saveConfig();
  radioRequestRefresh();
  Serial.println("Schedules saved atomically"); return true;
}
int main() {
 const std::string previous="[{\"station\":0,\"start\":390,\"end\":395}]";
 flash[STATION_CONFIG_FILE]=previous;
 const bool reportedSuccess=saveSchedules();
 std::cout<<"saveSchedules reported success: "<<reportedSuccess<<"\n";
 std::cout<<"Previous valid schedule: "<<previous<<"\n";
 std::cout<<"Committed after JSON allocation failure: "<<flash.at(STATION_CONFIG_FILE)<<"\n";
 assert(reportedSuccess);
 assert(flash.at(STATION_CONFIG_FILE)!=previous);
 assert(configDirty);
}
