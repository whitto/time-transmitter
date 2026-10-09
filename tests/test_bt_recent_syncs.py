#!/usr/bin/env python3
"""Verify the bounded RAM ring and actual firmware sync-result hook."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import test_bluetooth_workflow as workflow

FIRMWARE = workflow.FIRMWARE

RING_DRIVER = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <type_traits>
#include "BtRecentSyncs.h"

int main() {
  static_assert(sizeof(BtRecentSyncs::Store) <= 256, "history memory must stay bounded");
  static_assert(std::is_trivially_destructible<BtRecentSyncs::Store>::value, "no heap-owned members");
  BtRecentSyncs::Store history;
  const char *watch="aa:bb:cc:dd:ee:ff", *other="11:22:33:44:55:66";
  assert(history.count(0)==0 && history.latest(0,0)==nullptr);
  assert(!history.bind(4,watch,0) && !history.record(4,watch,0,1704110400,true));
  assert(history.bind(0,watch,0));
  for(uint32_t i=0;i<1000000;++i) {
    assert(history.record(0,watch,0,1704110400+i,i%2==0));
    assert(history.count(0)==(i<4?i+1:4));
    assert(history.latest(0,0)->utcEpoch==1704110400+i);
  }
  for(size_t i=0;i<4;++i) {
    const auto *entry=history.latest(0,i);
    assert(entry && entry->utcEpoch==1704110400+999999-i && entry->protocol==0);
    assert(entry->successful==((999999-i)%2==0));
  }
  assert(history.latest(0,4)==nullptr && history.latest(4,0)==nullptr);
  assert(history.bind(0,"AA:BB:CC:DD:EE:FF",0) && history.count(0)==4);
  assert(history.bind(1,other,2) && history.record(1,other,2,1704110500,true));
  assert(!history.record(0,other,0,1704110600,false)); // Failed replacement not attributed to old watch.
  assert(history.count(0)==4 && history.count(1)==1);
  assert(history.bind(0,other,0) && history.count(0)==0); // Confirmed replacement clears only this profile.
  assert(history.record(0,other,0,0,false) && history.latest(0,0)->utcEpoch==0);
  assert(history.count(1)==1);
  assert(history.bind(0,other,1) && history.count(0)==0); // Changed protocol is a changed identity.
  assert(history.record(0,other,1,42,false) && history.latest(0,0)->utcEpoch==0);
  assert(!history.record(0,other,1,1704110700,false,true) && history.count(0)==1);
  assert(history.record(0,other,1,1704110700,true,true) && history.count(0)==2);
  assert(!history.bind(0,"",1) && history.count(0)==0);
  for(const char* bad:{"a", "aa:bb", "aa:bb:cc:dd:ee:f", "aa:bb:cc:dd:ee:ffx", "gg:bb:cc:dd:ee:ff"})
    assert(!history.bind(0,bad,0));
  assert(!history.bind(0,other,3) && history.count(0)==0);
  BtRecentSyncs::Store rebooted;
  assert(rebooted.count(0)==0 && rebooted.count(1)==0);
  std::printf("RAM history uses %zu fixed bytes: million inserts, newest-four order, identity/profile isolation and reboot reset passed\n",sizeof(history));
}
'''

HOOK_DRIVER = r'''
int main() {
  initBluetoothSync(); bind();
  btAlwaysWaitEnabled=false; btHistoryPersistEnabled=false;
  btLastWatchAddress="aa:bb:cc:dd:ee:ff"; btLastWatchName="Original watch";
  for(int i=0;i<6;++i) {
    fakeEpoch=1704110400+i*60; writeResult=i%2==0;
    assert(attemptBluetoothSync()==writeResult);
  }
  assert(btRecentSyncs.count(0)==4 && flash.empty() && !configDirty);
  for(size_t i=0;i<4;++i) {
    const auto* entry=btRecentSyncs.latest(0,i);
    assert(entry && entry->utcEpoch==1704110400+(5-i)*60 && entry->successful==((5-i)%2==0));
  }
  // Missing-clock/full-time/RF-entry deferrals are not sync attempts.
  auto count=btRecentSyncs.count(0);
  full_time_tx=true; assert(!attemptBluetoothSync() && btRecentSyncs.count(0)==count); full_time_tx=false;
  trustedClock=false; assert(!attemptBluetoothSync() && btRecentSyncs.count(0)==count); trustedClock=true;
  preemptWrite=true; writeResult=false;
  const auto beforeRfEpoch=btRecentSyncs.latest(0,0)->utcEpoch;
  assert(!attemptBluetoothSync() && btRecentSyncs.latest(0,0)->utcEpoch==beforeRfEpoch);
  assert(!attemptBluetoothSync() && btRecentSyncs.latest(0,0)->utcEpoch==beforeRfEpoch);
  preemptWrite=false; shutdownBluetoothForRadio();
  assert(radioBleArbiter.requestRf()); radioBleArbiter.releaseRf(); initBluetoothSync();
  // A real failure after clock confidence was lost has an honest unknown date.
  btLastWatchAddress="aa:bb:cc:dd:ee:ff"; loseClockDuringAttempt=true;
  assert(!attemptBluetoothSync() && btRecentSyncs.latest(0,0)->utcEpoch==0);
  assert(!btRecentSyncs.latest(0,0)->successful && flash.empty());
  trustedClock=true; loseClockDuringAttempt=false;
  // A failed candidate pairing preserves old binding/history; confirmed new
  // watch replaces the profile and starts its own history with that success.
  btPairModeActive=true; btLastWatchAddress="11:22:33:44:55:66";
  assert(!attemptBluetoothSync() && btRecentSyncs.latest(0,0)->utcEpoch==0);
  assert(btProfileAddress[0]=="aa:bb:cc:dd:ee:ff");
  writeResult=true; fakeEpoch=1704111400;
  assert(attemptBluetoothSync() && btRecentSyncs.count(0)==1);
  assert(btRecentSyncs.latest(0,0)->utcEpoch==fakeEpoch && btRecentSyncs.latest(0,0)->successful);
  assert(btProfileAddress[0]=="11:22:33:44:55:66" && configDirty);
  assert(flash.empty()); // Only ordinary binding save was queued; ring never writes flash.
  btRecentSyncs.bind(0,"",0); assert(btRecentSyncs.count(0)==0);
  std::puts("Actual sync-result hook records real success/failure only; RF deferrals/replacement failures excluded; zero history flash writes");
}
'''


class BtRecentSyncsTest(unittest.TestCase):
    def run_cpp(self, code):
        self.assertIsNotNone(shutil.which('g++'))
        with tempfile.TemporaryDirectory(prefix='radioclock-recent-syncs-') as tmp:
            unit, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            unit.write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                            '-I', str(FIRMWARE.parent), str(unit), '-o', str(binary)], check=True)
            env = os.environ.copy()
            env['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'
            env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
            subprocess.run([str(binary)], env=env, check=True, timeout=20)

    def test_fixed_ram_ring_order_rollover_identity_and_reset(self):
        self.run_cpp(RING_DRIVER.replace('#include <cstdio>', '#include <cstdio>\n#include <initializer_list>'))

    def test_actual_sync_result_hook_keeps_flash_and_identity_isolated(self):
        source = workflow.BluetoothWorkflowTest().unit_source(FIRMWARE.read_text())
        old_attempt = workflow.extract_function(workflow.HELPERS, 'performGShockBX5600Sync')
        new_attempt = old_attempt.replace('return writeResult;',
                                         'if(loseClockDuringAttempt) { trustedClock=false; } return writeResult;')
        source = source.replace(old_attempt, 'bool loseClockDuringAttempt=false;\n' + new_attempt, 1)
        helpers = workflow.extract_function(workflow.DRIVER, 'bind')
        self.run_cpp(source.replace(workflow.DRIVER, helpers + HOOK_DRIVER, 1))


if __name__ == '__main__':
    unittest.main()
