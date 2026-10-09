#!/usr/bin/env python3
"""Fault-inject the actual bounded config/schedule saves and recovery route."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

try:
    from . import test_bluetooth_workflow as workflow
except ImportError:
    import test_bluetooth_workflow as workflow

DRIVER = r'''
static void clearFaults() {
  writeOpenOk = readbackOpenOk = renameOk = true;
  shortWrite = shortRead = corruptReadback = wrongReadbackSize = false;
  failJsonAllocation = false; testJsonCapacity = 8192;
}
static void boundedWriter() {
  char bytes[32];
  RadioConfigWriter writer(bytes, sizeof(bytes));
  assert(writer.number(INT32_MIN) && writer.unsignedNumber(UINT32_MAX));
  assert(writer.complete(2) && std::strcmp(bytes, "-2147483648\n4294967295\n") == 0);
  assert(!writer.complete(3));
  char exact[3]; RadioConfigWriter exactWriter(exact, sizeof(exact));
  assert(exactWriter.line("a") && exactWriter.complete(1));
  char shortBuffer[2]; RadioConfigWriter tooSmall(shortBuffer, sizeof(shortBuffer));
  assert(!tooSmall.line("a") && !tooSmall.complete(1));
  RadioConfigWriter zero(nullptr, 0); assert(!zero.line("a") && !zero.complete(0));
  RadioConfigWriter injection(bytes, sizeof(bytes));
  assert(!injection.line("bad\nline") && !injection.complete(1));
  RadioConfigWriter names(bytes, sizeof(bytes));
  assert(names.line("\rWatch\nName", true) && names.complete(1));
  assert(!std::strcmp(bytes, "WatchName\n"));
  std::puts("Bounded config writer: exact capacity, zero capacity, integer extrema, line count and newline injection passed");
}
static void configFaults() {
  assert(writeConfigNow());
  const auto previous = flash.at(CONFIG_FILE);
  std::strcpy(ssid, "Changed credentials");
  for (int fault = 0; fault < 7; ++fault) {
    clearFaults();
    writeOpenOk = fault != 0; shortWrite = fault == 1;
    readbackOpenOk = fault != 2; shortRead = fault == 3;
    corruptReadback = fault == 4; wrongReadbackSize = fault == 5; renameOk = fault != 6;
    assert(!writeConfigNow());
    assert(configStorageFault && !configDirty && flash.at(CONFIG_FILE) == previous);
    const auto failedWrites = writeOpens;
    // The handlers' rollback saveConfig calls cannot restart idle writes.
    saveConfig();
    for (int i=0;i<100;++i) { tick += 3001; servicePendingConfigSave(); }
    assert(writeOpens == failedWrites && !configDirty);
  }
  clearFaults(); assert(writeConfigNow());
  assert(!configStorageFault && flash.at(CONFIG_FILE) != previous);
  const auto restored = flash.at(CONFIG_FILE);
  const auto beforeOverflowWrites = writeOpens;
  btProfileName[0] = std::string(2048, 'X');
  assert(!writeConfigNow() && writeOpens == beforeOverflowWrites && flash.at(CONFIG_FILE) == restored);
  btProfileName[0] = "Watch\r\nName";
  assert(writeConfigNow());
  assert(flash.at(CONFIG_FILE).find("WatchName\n") != std::string::npos);
  const auto sanitized = flash.at(CONFIG_FILE);
  const auto beforeNewlineWrites = writeOpens;
  std::strcpy(ssid, "Injected\nconfig line");
  assert(!writeConfigNow() && writeOpens == beforeNewlineWrites && flash.at(CONFIG_FILE) == sanitized);
  std::strcpy(ssid, "Valid"); assert(writeConfigNow());
  // One deferred binding/config save succeeds, then remains idle.
  saveConfig(); const auto beforeDeferred = writeOpens;
  tick += 2999; servicePendingConfigSave(); assert(writeOpens == beforeDeferred);
  tick += 1; servicePendingConfigSave(); assert(writeOpens == beforeDeferred + 1 && !configDirty);
  writeOpenOk = false; saveConfig(); tick += 3000; servicePendingConfigSave();
  assert(configStorageFault && !configDirty);
  const auto lastAttempt = writeOpens;
  tick += 3000; servicePendingConfigSave(); assert(writeOpens == lastAttempt);
  clearFaults();
  std::puts("Actual config saves: bounded complete format, seven storage faults, exact readback, explicit retry and no recurring writes passed");
}
static void configBootBounds() {
  clearFaults();
  std::strcpy(ssid, "Saved network");
  btProfileName[0] = std::string(47, 'W');
  assert(writeConfigNow());
  const auto valid = flash.at(CONFIG_FILE);
  ssid[0] = '\0'; loadConfig();
  assert(std::strcmp(ssid, "Saved network") == 0 && !configStorageFault);
  // Legacy records without the appended features/final newline remain usable.
  flash[CONFIG_FILE] = "Legacy network\nLegacy password\nAsia/Tokyo";
  loadConfig(); assert(std::strcmp(ssid, "Legacy network") == 0 && timezone_name == "Asia/Tokyo");
  flash[CONFIG_FILE] = "Legacy network\r\nLegacy password\r\nAsia/Tokyo\r\n";
  loadConfig(); assert(std::strcmp(ssid, "Legacy network") == 0 && timezone_name == "Asia/Tokyo");
  const std::vector<std::string> corrupt = {
    std::string(2048, 'X'), std::string(65, 'X') + "\npass\nAsia/Tokyo\n",
    "ssid\npassword", std::string("ssid\npass\nAsia\0Tokyo\n", 21)
  };
  const auto beforeWrites = writeOpens;
  for (const auto &record : corrupt) {
    flash[CONFIG_FILE] = record; configStorageFault = false;
    std::strcpy(ssid, "RAM credentials"); full_time_tx = true;
    loadConfig();
    assert(configStorageFault && !ssid[0] && !passwd[0] && !full_time_tx);
    assert(flash.at(CONFIG_FILE) == record && writeOpens == beforeWrites && formats == 0);
  }
  flash[CONFIG_FILE] = valid; configStorageFault = false; loadConfig();
  assert(std::strcmp(ssid, "Saved network") == 0);
  std::puts("Actual boot config: bounded file/lines, binary/truncated rejection, preserved corrupt records and legacy records passed");
}
static void scheduleFaults() {
  const std::string previous = "[{\"station\":0,\"start\":390,\"end\":395}]";
  flash[STATION_CONFIG_FILE] = previous;
  schedules[0] = {0, 400, 405}; schedule_count = 1;
  auto before = writeOpens;
  failJsonAllocation = true;
  assert(!saveSchedules() && writeOpens == before && flash.at(STATION_CONFIG_FILE) == previous);
  failJsonAllocation = false; testJsonCapacity = 32;
  assert(!saveSchedules() && writeOpens == before && flash.at(STATION_CONFIG_FILE) == previous);
  testJsonCapacity = 8192; schedule_count = MAX_SCHEDULES + 1;
  assert(!saveSchedules() && writeOpens == before && flash.at(STATION_CONFIG_FILE) == previous);
  schedule_count = -1; assert(!saveSchedules() && writeOpens == before);
  schedule_count = 1; schedules[0].end_min = 399;
  assert(!saveSchedules() && writeOpens == before && flash.at(STATION_CONFIG_FILE) == previous);
  schedules[0].end_min = 405;
  for (int fault = 0; fault < 7; ++fault) {
    clearFaults(); writeOpenOk = fault != 0; shortWrite = fault == 1;
    readbackOpenOk = fault != 2; shortRead = fault == 3;
    corruptReadback = fault == 4; wrongReadbackSize = fault == 5; renameOk = fault != 6;
    assert(!saveSchedules() && flash.at(STATION_CONFIG_FILE) == previous);
  }
  clearFaults(); assert(saveSchedules());
  ArduinoJson::DynamicJsonDocument verify(8192);
  assert(!ArduinoJson::deserializeJson(verify, flash.at(STATION_CONFIG_FILE)));
  assert(verify.as<ArduinoJson::JsonArray>().size() == 1);
  assert(verify[0]["start"].as<int>() == 400 && verify[0]["end"].as<int>() == 405);
  schedule_count = MAX_SCHEDULES;
  for (int i=0;i<MAX_SCHEDULES;++i) schedules[i] = {i % NUM_STATIONS, i * 10, i * 10 + 5};
  assert(saveSchedules());
  assert(!ArduinoJson::deserializeJson(verify, flash.at(STATION_CONFIG_FILE)));
  assert(verify.as<ArduinoJson::JsonArray>().size() == MAX_SCHEDULES);
  schedule_count = 0; assert(saveSchedules());
  assert(flash.at(STATION_CONFIG_FILE) == "[]"); // Explicit empty schedules remain valid.
  filesystemAvailable = false; before = writeOpens;
  assert(!saveSchedules() && writeOpens == before);
  filesystemAvailable = true;
  std::puts("Actual schedule saves: allocator failure, partial JSON, invalid entries/counts, seven storage faults and empty/max lists passed");
}
static void mountRecovery() {
  flash[CONFIG_FILE] = "precious settings";
  mountOk = false; initFilesystem();
  assert(!filesystemAvailable && configStorageFault && !lastMountFormat && formats == 0);
  assert(flash.at(CONFIG_FILE) == "precious settings");
  registerStorageRoute();
  ap_mode = false; server.post("/api/storage/reset", {{"confirm", "ERASE_SAVED_SETTINGS"}});
  assert(server.code == 409 && formats == 0);
  ap_mode = true; server.post("/api/storage/reset"); assert(server.code == 400 && formats == 0);
  server.post("/api/storage/reset", {{"confirm", "yes"}}); assert(server.code == 400 && formats == 0);
  pauseOk = false; server.post("/api/storage/reset", {{"confirm", "ERASE_SAVED_SETTINGS"}});
  assert(server.code == 503 && formats == 0 && flash.at(CONFIG_FILE) == "precious settings");
  pauseOk = true; formatOk = false;
  server.post("/api/storage/reset", {{"confirm", "ERASE_SAVED_SETTINGS"}});
  assert(server.code == 500 && formats == 1 && restarts == 0 && flash.at(CONFIG_FILE) == "precious settings");
  formatOk = true;
  server.post("/api/storage/reset", {{"confirm", "ERASE_SAVED_SETTINGS"}});
  assert(server.code == 200 && formats == 2 && restarts == 1 && flash.empty());
  mountOk = true; initFilesystem(); assert(filesystemAvailable && !configStorageFault && !lastMountFormat);
  server.post("/api/storage/reset", {{"confirm", "ERASE_SAVED_SETTINGS"}});
  assert(server.code == 409 && formats == 2); // Ordinary save failures never authorize erasure.
  std::puts("Actual mount/recovery: mount preserves flash, confirmed fault-only AP formatting, pause/format failures and restart passed");
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  if (!std::strcmp(argv[1], "writer")) boundedWriter();
  else if (!std::strcmp(argv[1], "config")) configFaults();
  else if (!std::strcmp(argv[1], "boot")) configBootBounds();
  else if (!std::strcmp(argv[1], "schedules")) scheduleFaults();
  else if (!std::strcmp(argv[1], "mount")) mountRecovery();
  else return 2;
}
'''


class StorageReliabilityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = workflow.FIRMWARE.read_text()
        unit = workflow.BluetoothWorkflowTest().unit_source(source)
        mocks = workflow.MOCKS.replace('#include "RadioBleArbiter.h"',
                                       '#include <ArduinoJson.h>\n#include "RadioBleArbiter.h"')
        mocks = mocks.replace('struct Schedule { int start_min, end_min; } schedules[4]{};', '''
constexpr int MAX_SCHEDULES = 24, NUM_STATIONS = 7;
struct TimeSchedule { int station = 0, start_min = 0, end_min = 0; };
TimeSchedule schedules[MAX_SCHEDULES]{};
bool pauseOk=true;
bool ap_mode=false;
bool radioSetPaused(bool value) { assert(value); return pauseOk; }
''')
        mocks = mocks.replace('struct File {', '''
bool writeOpenOk=true, readbackOpenOk=true, renameOk=true;
bool shortWrite=false, shortRead=false, corruptReadback=false, wrongReadbackSize=false;
int writeOpens=0;
bool mountOk=true, lastMountFormat=true, formatOk=true;
int formats=0, restarts=0;
struct EspMock { void restart() { ++restarts; } } ESP;
bool failJsonAllocation=false;
size_t testJsonCapacity=8192;
struct FaultAllocator {
  void *allocate(size_t count) { return failJsonAllocation ? nullptr : std::malloc(count); }
  void deallocate(void *value) { std::free(value); }
  void *reallocate(void *value, size_t count) { return failJsonAllocation ? nullptr : std::realloc(value, count); }
};
using FaultJsonDocument = ArduinoJson::BasicJsonDocument<FaultAllocator>;
struct File {''')
        mocks = mocks.replace('return contents ? contents->size() : 0;',
                              'return contents ? contents->size() + (wrongReadbackSize ? 1 : 0) : 0;')
        mocks = mocks.replace('position += count; return count;',
                              'if (corruptReadback && count) { out[0] ^= 1; } position += count; return shortRead ? 0 : count;')
        mocks = mocks.replace('contents->append(reinterpret_cast<const char*>(data), count); return count;',
                              'contents->append(reinterpret_cast<const char*>(data), count); return shortWrite ? 0 : count;')
        mocks = mocks.replace("if (*mode == 'w') {", "if (*mode == 'w') { ++writeOpens; if (!writeOpenOk) return {}; ")
        mocks = mocks.replace('auto it = flash.find(name);', 'if (!readbackOpenOk) { return {}; } auto it = flash.find(name);')
        mocks = mocks.replace('flash[to] = flash.at(from);', 'if (!renameOk) return false; flash[to] = flash.at(from);')
        mocks = mocks.replace('struct LittleFsMock {', '''struct LittleFsMock {
  bool begin(bool formatOnFailure) { lastMountFormat = formatOnFailure; return mountOk; }
  bool format() { ++formats; if (formatOk) flash.clear(); return formatOk; }
''')
        mocks += '\nconstexpr const char *STATION_CONFIG_FILE="schedules", *SCHEDULE_TEMP_FILE="schedules.tmp";\n'
        unit = unit.replace(workflow.MOCKS, mocks, 1)
        additions = '\n'.join(workflow.extract_function(source, name) for name in
                               ['servicePendingConfigSave', 'initFilesystem', 'validScheduleJson', 'saveSchedules'])
        additions = additions.replace('DynamicJsonDocument doc(8192);', 'FaultJsonDocument doc(testJsonCapacity);')
        additions += '\nvoid registerStorageRoute() {\n' + workflow.extract_route(source, '/api/storage/reset') + '\n}\n'
        unit = unit.replace(workflow.DRIVER, additions + DRIVER, 1)
        cls.temporary = tempfile.TemporaryDirectory(prefix='radioclock-storage-faults-')
        unit_path = Path(cls.temporary.name) / 'test.cpp'
        unit_path.write_text(unit)
        headers = Path(os.environ.get('RADIOCLOCK_TOOLS_DIR', '/workspace/.radioclock-tools')) / 'user/libraries/ArduinoJson/src'
        cls.binary = Path(cls.temporary.name) / 'test'
        command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(workflow.FIRMWARE.parent),
                   '-I', str(headers), str(unit_path), '-o', str(cls.binary)]
        subprocess.run(command, check=True)
        cls.sanitized = Path(cls.temporary.name) / 'test-asan'
        command[-1] = str(cls.sanitized)
        command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g', '-no-pie']
        subprocess.run(command, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_checked_config_and_explicit_retry(self):
        subprocess.run([str(self.binary), 'config'], check=True)

    def test_bounded_writer_edges(self):
        subprocess.run([str(self.binary), 'writer'], check=True)

    def test_boot_config_file_and_line_bounds(self):
        subprocess.run([str(self.binary), 'boot'], check=True)

    def test_schedule_oom_and_partial_record(self):
        subprocess.run([str(self.binary), 'schedules'], check=True)

    def test_preserved_mount_and_explicit_recovery(self):
        subprocess.run([str(self.binary), 'mount'], check=True)

    def test_fault_paths_under_sanitizers(self):
        environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1')
        for case in ['writer', 'config', 'boot', 'schedules', 'mount']:
            subprocess.run([str(self.sanitized), case], check=True, env=environment)


if __name__ == '__main__':
    unittest.main()
