#!/usr/bin/env python3
"""Validate compact BT status snapshots and the Brisbane daily save allowance."""
import shutil
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import test_bluetooth_workflow as workflow

FIRMWARE = workflow.FIRMWARE


DRIVER = r'''
#include "BtSyncHistory.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>
using namespace BtSyncHistory;
static_assert(std::is_trivial<Snapshot>::value, "Snapshot must remain plain data");
static_assert(std::is_standard_layout<Snapshot>::value, "Snapshot layout must remain fixed");
static_assert(sizeof(Snapshot) <= 192, "Persist only compact status, not textual logs");
static Snapshot sample() {
  Snapshot s{};
  s.generation = 7;
  s.successEpoch = 1791453600;  // 2026-10-08 10:00:00 UTC / 20:00 Brisbane.
  s.savedDay = brisbaneDay(s.successEpoch);
  s.lastProfile = 2; s.lastProtocol = 0;
  s.connections = 9; s.ackedWrites = 15; s.notifications = 28; s.responseErrors = 2;
  s.protocolEpoch[0] = s.profileEpoch[2] = s.successEpoch;
  std::strcpy(s.profileAddress[2], "A1:B2:C3:D4:E5:F6");
  seal(s);
  return s;
}
static void quota() {
  const int64_t midnight = 1791468000;  // 2026-10-08 14:00 UTC / Oct 9 midnight Brisbane.
  const int64_t yesterday = brisbaneDay(midnight - 1), today = brisbaneDay(midnight);
  assert(today == yesterday + 1);
  assert(brisbaneDay(midnight + 86399) == today);
  assert(brisbaneDay(midnight + 86400) == today + 1);
  assert(brisbaneDay(EarliestClockUtc - 1) == -1);
  assert(brisbaneDay(LatestClockUtc + 1) == -1);
  assert(brisbaneDay(std::numeric_limits<int64_t>::min()) == -1);
  assert(brisbaneDay(std::numeric_limits<int64_t>::max()) == -1);
  assert(!saveDue(false, midnight, -1, -1));
  assert(saveDue(true, midnight, -1, -1));
  assert(!saveDue(true, midnight + 86399, today, -1));  // Reboot: persisted quota survives.
  assert(!saveDue(true, midnight + 86399, -1, today));  // Failed attempt: no busy retry loop.
  assert(!saveDue(true, midnight - 1, today, -1));     // Clock rollback.
  assert(!saveDue(true, midnight - 1, -1, today));
  assert(saveDue(true, midnight + 86400, today, today));
  assert(!saveDue(true, 0, -1, -1));
  assert(!saveDue(true, LatestClockUtc + 1, -1, -1));
  // An Off/On setting change must keep the saved-day allowance externally.
  assert(!saveDue(true, midnight + 1, today, -1));
  std::puts("Brisbane calendar-day quota: midnight, reboot, disable/re-enable, failed attempt and clock rollback passed");
}
static void corruption() {
  const Snapshot source = sample();
  assert(valid(source));
  std::vector<unsigned char> flash(sizeof(Snapshot));
  std::memcpy(flash.data(), &source, sizeof(source));
  Snapshot restored{};
  std::memcpy(&restored, flash.data(), sizeof(restored));
  assert(valid(restored));
  assert(restored.generation == 7 && restored.connections == 9 && restored.ackedWrites == 15);
  assert(restored.profileEpoch[2] == source.successEpoch);
  assert(restored.notifications == 28 && restored.responseErrors == 2);
  assert(std::strcmp(restored.profileAddress[2], source.profileAddress[2]) == 0);
  // Every bit in every stored byte, including the CRC/header/padding, is checked.
  for (size_t byte = 0; byte < sizeof(Snapshot); ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      Snapshot damaged{};
      std::memcpy(&damaged, &source, sizeof(damaged));
      reinterpret_cast<unsigned char *>(&damaged)[byte] ^= 1U << bit;
      assert(!valid(damaged));
    }
  }
  std::printf("Compact %zu-byte snapshot preserves counters, UTC timestamps and watch identity; every single-bit corruption rejected\n", sizeof(Snapshot));
}
static void validation() {
  Snapshot s = sample();
  s.magic ^= 1U; s.checksum = crc32(s); assert(!valid(s));
  s = sample(); s.schema++; s.checksum = crc32(s); assert(!valid(s));
  s = sample(); s.size--; s.checksum = crc32(s); assert(!valid(s));
  s = sample(); s.savedDay++; seal(s); assert(!valid(s));
  s = sample(); s.successEpoch = EarliestClockUtc - 1; s.savedDay = -1; seal(s); assert(!valid(s));
  s = sample(); s.successEpoch = LatestClockUtc + 1; s.savedDay = -1; seal(s); assert(!valid(s));
  s = sample(); s.lastProfile = ProfileCount; seal(s); assert(!valid(s));
  s = sample(); s.lastProtocol = ProtocolCount; seal(s); assert(!valid(s));
  s = sample(); std::memset(s.profileAddress[2], 'x', sizeof(s.profileAddress[2])); seal(s); assert(!valid(s));
  s = sample(); s.profileAddress[2][2] = '-'; seal(s); assert(!valid(s));
  s = sample(); s.profileAddress[2][0] = 'g'; seal(s); assert(!valid(s));
  s = sample(); s.profileAddress[2][0] = 0; seal(s); assert(!valid(s));
  s = sample(); s.profileAddress[0][0] = 'x'; seal(s); assert(!valid(s));
  s = sample(); s.profileProtocol[0] = ProtocolCount; seal(s); assert(!valid(s));
  s = sample(); s.profileProtocol[2] = 1; seal(s); assert(!valid(s));
  s = sample(); s.profileEpoch[2]--; seal(s); assert(!valid(s));
  s = sample(); s.protocolEpoch[0]--; seal(s); assert(!valid(s));
  s = sample(); s.protocolEpoch[1] = EarliestClockUtc - 1; seal(s); assert(!valid(s));
  s = sample(); s.profileEpoch[0] = s.successEpoch - 1; seal(s); assert(!valid(s));
  s = sample(); s.profileEpoch[0] = EarliestClockUtc - 1;
  std::strcpy(s.profileAddress[0], "11:22:33:44:55:66"); seal(s); assert(!valid(s));
  // Clock corrections can make another profile's recorded success later than
  // the latest transaction. Preserve valid absolute times; do not invent order.
  s = sample(); s.profileEpoch[0] = s.successEpoch + 10;
  std::strcpy(s.profileAddress[0], "11:22:33:44:55:66");
  s.protocolEpoch[1] = s.successEpoch + 10; seal(s); assert(valid(s));
  // An unsynchronized bound watch is legitimate; it has no fabricated success.
  s = sample(); std::strcpy(s.profileAddress[0], "11:22:33:44:55:66"); seal(s); assert(valid(s));
  s = sample(); std::strcpy(s.profileAddress[2], "a1:b2:c3:d4:e5:f6"); seal(s); assert(valid(s));
  std::puts("Malformed schema, epoch, quota, profile, protocol and address fields rejected; unset profiles retained");
}
int main() {
  quota(); corruption(); validation();
}
'''


INTEGRATION_DRIVER = r'''
static constexpr time_t firstEpoch = 1791453600;  // Oct 8, 20:00 Brisbane.
static constexpr time_t nextDay = 1791468000;     // Oct 9, midnight Brisbane.
static void initialize() {
  setenv("TZ", "UTC0", 1); tzset();
  flash.clear(); clearBluetoothRuntimeHistory(); btLegacyHistory = {};
  btHistoryPersistEnabled = true; btHistoryGeneration = 0;
  btHistorySavedDay = btHistoryAttemptDay = -1; btHistorySavedEpoch = 0;
  btActiveProfile = btManualProfile = 0;
  btActiveProtocol = btManualProtocol = BT_PROTOCOL_BX5600_MIP;
  btProfileAddress[0] = "aa:bb:cc:dd:ee:ff";
  btProfileName[0] = "Original watch";
  btProfileProtocol[0] = BT_PROTOCOL_BX5600_MIP;
  btTimezoneName = "Australia/Brisbane"; btTimeOffsetMinutes = 0;
  btAlwaysWaitEnabled = false;
  btConnectionAttempts = 3; btWriteAcknowledgements = 4;
  btNotifications = 5; btResponseErrors = 1;
  trustedClock = true; full_time_tx = false; configDirty = false;
  assert(radioBleArbiter.acquireBle());
  registerRoutes();
}
static bool syncAt(time_t epoch, bool ok = true) {
  fakeEpoch = epoch; writeResult = ok;
  btLastWatchAddress = btProfileAddress[btActiveProfile];
  btLastWatchName = btProfileName[btActiveProfile];
  return attemptBluetoothSync();
}
static BtSyncHistory::Snapshot stored() {
  const auto &bytes = flash.at(BT_HISTORY_FILE);
  assert(bytes.size() == sizeof(BtSyncHistory::Snapshot));
  BtSyncHistory::Snapshot result{};
  std::memcpy(&result, bytes.data(), sizeof(result));
  assert(BtSyncHistory::valid(result));
  return result;
}
static void normal() {
  assert(syncAt(firstEpoch));
  assert(historyWriteOpens == 1 && historyRenameCalls == 1);
  assert(!configDirty);  // A routine successful sync never queues a config rewrite.
  const auto firstBytes = flash.at(BT_HISTORY_FILE);
  const String firstDate = btLastSyncDate;
  assert(stored().successEpoch == firstEpoch);
  assert(btHistorySavedDay == BtSyncHistory::brisbaneDay(firstEpoch));
  btConnectionAttempts = 30; btWriteAcknowledgements = 40;
  btNotifications = 50; btResponseErrors = 10;
  assert(syncAt(firstEpoch + 300));
  assert(btLastSyncEpoch == firstEpoch + 300 && btLastOutcomeSuccessful);
  assert(btLastSyncDate != firstDate && !configDirty);
  assert(historyWriteOpens == 1 && flash.at(BT_HISTORY_FILE) == firstBytes);
  assert(!syncAt(firstEpoch + 600, false));
  assert(!btLastOutcomeSuccessful && historyWriteOpens == 1);
  assert(flash.at(BT_HISTORY_FILE) == firstBytes);
  // Reboot restores exactly the accepted first-success snapshot, not later RAM events.
  btBatteryReadings[0].available = true; btBatteryReadings[0].percent = 60;
  btFontLastStatus = "Temporary font result";
  loadBluetoothHistory();
  assert(btLastSyncEpoch == firstEpoch && btLastOutcomeSuccessful);
  assert(btLastSyncDate == firstDate && btProfileDoneToday(0));
  assert(btConnectionAttempts == 3 && btWriteAcknowledgements == 4);
  assert(btNotifications == 5 && btResponseErrors == 1);
  assert(!btBatteryReadings[0].available && btFontLastStatus != "Temporary font result");
  assert(syncAt(firstEpoch + 900));
  assert(historyWriteOpens == 1);  // Loading retained metadata does not reset quota.
  assert(syncAt(nextDay));
  assert(historyWriteOpens == 2 && historyRenameCalls == 2);
  assert(stored().successEpoch == nextDay);
  assert(syncAt(nextDay - 1));
  assert(historyWriteOpens == 2);  // A clock correction cannot buy an older day.
  std::puts("Real sync outcome hooks: immediate first save, later success/failure RAM only, reboot counters/LED evidence, midnight and rollback passed");
}
static void disabled() {
  assert(syncAt(firstEpoch));
  const auto firstBytes = flash.at(BT_HISTORY_FILE);
  server.post("/api/config", {{"bt_history_persist", "0"}});
  assert(server.code == 200 && !btHistoryPersistEnabled && btHistoryGeneration == 1);
  assert(historyWriteOpens == 1 && flash.at(BT_HISTORY_FILE) == firstBytes);
  assert(syncAt(firstEpoch + 3600));
  assert(btLastOutcomeSuccessful && btLastSyncEpoch == firstEpoch + 3600);
  assert(historyWriteOpens == 1 && !configDirty);
  const auto runtimeDate = btLastSyncDate;
  assert(writeConfigNow());
  assert(flash.at(CONFIG_FILE).find(runtimeDate.s) == std::string::npos);
  loadConfig();
  assert(!btHistoryPersistEnabled && btHistoryGeneration == 1);
  assert(!btLastOutcomeSuccessful && btLastSyncEpoch == 0 && btLastSyncDate.length() == 0);
  assert(btConnectionAttempts == 0 && btNotifications == 0);
  assert(btHistorySavedDay == BtSyncHistory::brisbaneDay(firstEpoch));
  server.post("/api/config", {{"bt_history_persist", "1"}});
  assert(server.code == 200 && btHistoryPersistEnabled && btHistoryGeneration == 1);
  loadConfig();
  assert(btLastSyncEpoch == 0 && !btLastOutcomeSuccessful);  // Old generation never returns.
  assert(syncAt(firstEpoch + 7200));
  assert(historyWriteOpens == 1 && flash.at(BT_HISTORY_FILE) == firstBytes);
  server.post("/api/config", {{"bt_history_persist", "invalid"}});
  assert(server.code == 400 && btHistoryPersistEnabled && btHistoryGeneration == 1);
  assert(syncAt(nextDay));
  assert(historyWriteOpens == 2 && stored().generation == 1);
  assert(writeConfigNow()); loadConfig();
  assert(btHistoryGeneration == 1 && btLastSyncEpoch == nextDay && btLastOutcomeSuccessful);
  // Generation wrap cannot resurrect a disabled old snapshot.
  btHistoryGeneration = UINT32_MAX;
  const auto configBefore = flash.at(CONFIG_FILE);
  server.post("/api/config", {{"bt_history_persist", "0"}});
  assert(server.code == 400 && btHistoryPersistEnabled && btHistoryGeneration == UINT32_MAX);
  assert(flash.at(CONFIG_FILE) == configBefore && historyWriteOpens == 2);
  std::puts("Real retention toggle: durable Off, RAM-only data, no config-history leak, stale-generation suppression and preserved daily quota passed");
}
static void replacement() {
  assert(syncAt(firstEpoch));
  btProfileAddress[0] = "11:22:33:44:55:66"; btProfileName[0] = "Replacement watch";
  assert(writeConfigNow()); loadConfig();
  assert(btProfileLastSyncEpoch[0] == 0 && !btProfileDoneToday(0));
  assert(!btBatteryReadings[0].available && btProfileLastSyncAddress[0].length() == 0);
  // Global history remains a historical transmitter-wide delivery record.
  assert(btLastSyncEpoch == firstEpoch && btLastOutcomeSuccessful);
  assert(btLastWatchAddress == "aa:bb:cc:dd:ee:ff" && btLastWatchName.length() == 0);
  btProfileAddress[0] = "aa:bb:cc:dd:ee:ff"; btProfileProtocol[0] = BT_PROTOCOL_STANDARD;
  loadBluetoothHistory();
  assert(btProfileLastSyncEpoch[0] == 0 && !btProfileDoneToday(0));
  std::puts("Saved delivery remains historical; replacement addresses/protocols cannot inherit profile completion or battery");
}
static void invalid() {
  assert(syncAt(firstEpoch));
  const auto source = flash.at(BT_HISTORY_FILE);
  for (size_t size : {size_t(0), size_t(1), sizeof(BtSyncHistory::Snapshot) - 1,
                     sizeof(BtSyncHistory::Snapshot) + 1}) {
    flash[BT_HISTORY_FILE] = source.substr(0, size);
    if (size > source.size()) flash[BT_HISTORY_FILE] += "x";
    loadBluetoothHistory();
    assert(btLastSyncEpoch == 0 && !btLastOutcomeSuccessful);
    assert(btLastSyncDate.length() == 0 && btHistorySavedDay == -1);
  }
  flash[BT_HISTORY_FILE] = source; flash[BT_HISTORY_FILE][0] ^= 1;
  loadBluetoothHistory();
  assert(btLastSyncEpoch == 0 && !btLastOutcomeSuccessful);
  BtSyncHistory::Snapshot semanticallyBad{};
  std::memcpy(&semanticallyBad, source.data(), sizeof(semanticallyBad));
  semanticallyBad.profileEpoch[semanticallyBad.lastProfile]--;
  BtSyncHistory::seal(semanticallyBad);
  flash[BT_HISTORY_FILE].assign(reinterpret_cast<const char *>(&semanticallyBad), sizeof(semanticallyBad));
  loadBluetoothHistory(); assert(btLastSyncEpoch == 0 && !btLastOutcomeSuccessful);
  assert(historyWriteOpens == 1);  // Bad history cannot cause an automatic startup write.
  std::puts("Real loader ignores truncated, oversized, CRC-corrupt and semantically invalid snapshots without writes");
}
static void failure(const char *stage) {
  assert(syncAt(firstEpoch));
  const auto prior = flash.at(BT_HISTORY_FILE);
  if (std::strcmp(stage, "open") == 0) historyOpenOk = false;
  else if (std::strcmp(stage, "short") == 0) historyShortWrite = true;
  else if (std::strcmp(stage, "readback") == 0) historyCorruptWrite = true;
  else { assert(std::strcmp(stage, "rename") == 0); historyRenameOk = false; }
  assert(syncAt(nextDay));  // History storage failure cannot undo delivered TIME.
  assert(btLastOutcomeSuccessful && btLastSyncEpoch == nextDay);
  assert(historyWriteOpens == 2 && flash.at(BT_HISTORY_FILE) == prior);
  assert(btHistorySavedDay == BtSyncHistory::brisbaneDay(firstEpoch));
  assert(btHistoryAttemptDay == BtSyncHistory::brisbaneDay(nextDay));
  assert(btHistorySaveStatus.s.find("failed") != std::string::npos && !configDirty);
  historyOpenOk = historyRenameOk = true; historyShortWrite = historyCorruptWrite = false;
  assert(syncAt(nextDay + 1));
  assert(historyWriteOpens == 2 && flash.at(BT_HISTORY_FILE) == prior);
  assert(syncAt(nextDay + 86400));
  assert(historyWriteOpens == 3 && stored().successEpoch == nextDay + 86400);
  std::printf("Real history save failure (%s) keeps prior snapshot, time success and one attempt/day; next day recovers\n", stage);
}
int main(int argc, char **argv) {
  assert(argc == 2); initialize();
  if (std::strcmp(argv[1], "normal") == 0) normal();
  else if (std::strcmp(argv[1], "disabled") == 0) disabled();
  else if (std::strcmp(argv[1], "replacement") == 0) replacement();
  else if (std::strcmp(argv[1], "invalid") == 0) invalid();
  else failure(argv[1]);
}
'''


class BtHistoryTest(unittest.TestCase):
    def test_snapshot_and_daily_quota(self):
        self.assertIsNotNone(shutil.which('g++'), 'g++ is required for BT history tests')
        with tempfile.TemporaryDirectory(prefix='radioclock-bt-history-') as tmp:
            source, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            source.write_text(DRIVER)
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I', str(FIRMWARE.parent), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_real_history_outcomes_storage_and_retention(self):
        unit = workflow.BluetoothWorkflowTest().unit_source(FIRMWARE.read_text())
        unit = unit.replace(workflow.DRIVER, INTEGRATION_DRIVER, 1)
        unit = unit.replace('struct File {', '''
bool historyOpenOk = true, historyRenameOk = true;
bool historyShortWrite = false, historyCorruptWrite = false;
unsigned historyWriteOpens = 0, historyRenameCalls = 0;
struct File {''', 1)
        unit = unit.replace('File open(const char* name, const char* mode) {', '''
File open(const char* name, const char* mode) {
  if (*mode == 'w' && std::strcmp(name, "/bt-sync-status.tmp") == 0) {
    ++historyWriteOpens; if (!historyOpenOk) return {};
  }''', 1)
        unit = unit.replace('bool rename(const char* from, const char* to) {', '''
bool rename(const char* from, const char* to) {
  if (std::strcmp(from, "/bt-sync-status.tmp") == 0) {
    ++historyRenameCalls; if (!historyRenameOk) return false;
  }''', 1)
        write_match = re.search(r'size_t write\(\s*const uint8_t\s*\*\s*\w+,\s*size_t\s+(\w+)\s*\)\s*\{', unit)
        self.assertIsNotNone(write_match, 'shared file mock must expose binary writes')
        count = write_match.group(1)
        write = workflow.balanced_block(unit, write_match.start())
        patched_write = write.replace('{', '{ if (historyShortWrite) ' + count + ' /= 2;', 1)
        patched_write = patched_write.replace('return ' + count + ';',
            'if (historyCorruptWrite && !contents->empty()) (*contents)[0] ^= 1; return ' + count + ';', 1)
        unit = unit.replace(write, patched_write, 1)
        with tempfile.TemporaryDirectory(prefix='radioclock-bt-history-integration-') as tmp:
            source, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            source.write_text(unit)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(FIRMWARE.parent), str(source), '-o', str(binary)], check=True)
            for case in ['normal', 'disabled', 'replacement', 'invalid', 'open', 'short', 'readback', 'rename']:
                with self.subTest(case=case):
                    subprocess.run([str(binary), case], check=True)


if __name__ == '__main__':
    unittest.main()
