#!/usr/bin/env python3
"""Exercise the shipped crash-dump setting's durable save and failure paths."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

try:
    from . import test_bluetooth_workflow as workflow
except ImportError:
    import test_bluetooth_workflow as workflow

DRIVER = r'''
int main() {
  setenv("TZ", "UTC0", 1); tzset();
  registerRoutes();
  loadConfig();
  assert(!crashDumpEnabled && !RadioCrashDumpGate::gateEnabled && flash.empty());
  wifiAccessEnabled = true; wifiAccessStart = 1080; wifiAccessEnd = 1200;
  assert(writeConfigNow());
  const auto defaultConfig = flash.at(CONFIG_FILE);
  server.post("/api/config", {{"crash_dump_enabled", "invalid"}});
  assert(server.code == 400 && flash.at(CONFIG_FILE) == defaultConfig);
  assert(!crashDumpEnabled && !RadioCrashDumpGate::gateEnabled);
  server.post("/api/config", {{"crash_dump_enabled", "1"}});
  assert(server.code == 200 && crashDumpEnabled && RadioCrashDumpGate::gateEnabled);
  assert(!configDirty && flash.at(CONFIG_FILE) != defaultConfig);
  crashDumpEnabled = false; RadioCrashDumpGate::gateEnabled = false;
  loadConfig();
  assert(crashDumpEnabled && RadioCrashDumpGate::gateEnabled && wifiAccessEnabled);
  const auto onConfig = flash.at(CONFIG_FILE);
  // Storage faults cannot acknowledge or apply a different panic policy.
  for (int failure = 0; failure < 3; ++failure) {
    crashOpenOk = failure != 0;
    crashShortWrite = failure == 1;
    crashRenameOk = failure != 2;
    server.post("/api/config", {{"crash_dump_enabled", "0"}});
    assert(server.code == 500 && crashDumpEnabled && RadioCrashDumpGate::gateEnabled);
    assert(flash.at(CONFIG_FILE) == onConfig);
  }
  crashOpenOk = crashRenameOk = true; crashShortWrite = false;
  server.post("/api/config", {{"crash_dump_enabled", "false"}});
  assert(server.code == 200 && !crashDumpEnabled && !RadioCrashDumpGate::gateEnabled);
  crashDumpEnabled = true; RadioCrashDumpGate::gateEnabled = true;
  loadConfig(); assert(!crashDumpEnabled && !RadioCrashDumpGate::gateEnabled);
  const auto offConfig = flash.at(CONFIG_FILE);
  for (int failure = 0; failure < 3; ++failure) {
    crashOpenOk = failure != 0; crashShortWrite = failure == 1; crashRenameOk = failure != 2;
    server.post("/api/config", {{"crash_dump_enabled", "true"}});
    assert(server.code == 500 && !crashDumpEnabled && !RadioCrashDumpGate::gateEnabled);
    assert(flash.at(CONFIG_FILE) == offConfig);
  }
  crashOpenOk = crashRenameOk = true; crashShortWrite = false;
  RadioCrashDumpGate::gateAvailable = false;
  server.post("/api/config", {{"crash_dump_enabled", "true"}});
  assert(server.code == 503 && !crashDumpEnabled && !RadioCrashDumpGate::gateEnabled);
  assert(flash.at(CONFIG_FILE) == offConfig);
  RadioCrashDumpGate::gateAvailable = true;
  // V4.13 and older files have no last-line preference: always default Off.
  auto legacyConfig = onConfig.substr(onConfig.find('\n')+1);
  legacyConfig.erase(legacyConfig.rfind("CRC32:"));
  flash[CONFIG_FILE] = legacyConfig.substr(0,legacyConfig.size()-2);
  crashDumpEnabled = true; RadioCrashDumpGate::gateEnabled = true;
  loadConfig(); assert(!crashDumpEnabled && !RadioCrashDumpGate::gateEnabled);
  assert(wifiAccessEnabled && wifiAccessStart == 1080 && wifiAccessEnd == 1200);
  std::puts("Actual crash setting: default Off, durable On/Off, legacy defaults, unsupported core and all storage rollbacks passed");
}
'''


class CrashDumpConfigTest(unittest.TestCase):
    def test_actual_config_and_panic_policy(self):
        self.assertIsNotNone(shutil.which("g++"))
        source = workflow.FIRMWARE.read_text()
        unit = workflow.BluetoothWorkflowTest().unit_source(source)
        mocks = workflow.MOCKS.replace(
            'struct File {',
            'bool crashOpenOk=true, crashRenameOk=true, crashShortWrite=false;\nstruct File {')
        mocks = mocks.replace("if (*mode == 'w') {", "if (*mode == 'w') { if (!crashOpenOk) return {}; ")
        mocks = mocks.replace('flash[to] = flash.at(from);',
                              'if (!crashRenameOk) return false; flash[to] = flash.at(from);')
        mocks = mocks.replace('contents->append(reinterpret_cast<const char*>(data), count); return count;',
                              'contents->append(reinterpret_cast<const char*>(data), count); return crashShortWrite ? 0 : count;')
        unit = unit.replace(workflow.MOCKS, mocks, 1).replace(workflow.DRIVER, DRIVER, 1)
        with tempfile.TemporaryDirectory(prefix="radioclock-crash-config-") as tmp:
            file, binary = Path(tmp) / "test.cpp", Path(tmp) / "test"
            file.write_text(unit)
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(workflow.FIRMWARE.parent), str(file), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
