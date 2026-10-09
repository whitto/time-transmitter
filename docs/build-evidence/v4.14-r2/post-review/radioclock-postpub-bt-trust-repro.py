from pathlib import Path
import sys
import re
import subprocess
sys.path.insert(0, '/workspace/time-transmitter/tests')
import test_bluetooth_workflow as workflow
import test_watch_options as options

source = workflow.FIRMWARE.read_text()
sp_source = Path('/workspace/time-transmitter/tests/test_bx_protocol.cpp').read_text()
captures = []
for name in ['settingsReply', 'dstReply', 'namesReply']:
    block = re.search(r'static const Bytes ' + name + r' = hex\((.*?)\);', sp_source, re.S).group(1)
    data = bytes.fromhex(''.join(re.findall(r'"([0-9a-f]+)"', block)))
    captures.append('const std::vector<uint8_t> ' + name + '={' + ','.join(map(str, data)) + '};')
mocks = options.FONT_MOCKS.replace('bool readBluetoothWatchBattery() { return false; }',
    'bool trustedClock=true; bool clockTrusted() { return trustedClock; }\n'
    'bool readBluetoothWatchBattery() { trustedClock=false; return false; }')
body = '\n'.join(workflow.extract_function(source, name) for name in
                 ['requestBluetoothBasicSettings', 'applyBluetoothWatchFont', 'performGShockBX5600Sync'])
driver = r'''
int main() {
  reset();
  assert(clockTrusted()); // Entry trust check succeeds.
  const bool success=performGShockBX5600Sync();
  assert(!clockTrusted()); // Injected expiry during optional battery work.
  assert(success && timeWrites == 1);
  std::printf("Actual BX transaction acknowledged TIME while clockTrusted=false after transaction entry; success=%d TIME writes=%d.\n", success, timeWrites);
}
'''
unit = Path('/tmp/radioclock-postpub-bt-trust-repro.cpp')
unit.write_text(workflow.MOCKS.split('struct SerialMock')[0] + '\n#include <sys/time.h>\n' +
                '\n'.join(captures) + mocks + workflow.transaction_deadline_source(source) + body + driver)
binary = unit.with_suffix('')
subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-no-pie', '-I', str(workflow.FIRMWARE.parent),
                str(unit), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
