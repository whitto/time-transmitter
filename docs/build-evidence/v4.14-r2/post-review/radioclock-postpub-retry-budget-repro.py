from pathlib import Path
import sys
import subprocess
sys.path.insert(0, '/workspace/time-transmitter')
from tests import test_radio_reliability as radio
radio.RadioReliabilityTests.setUpClass()
fixture = radio.RadioReliabilityTests().unit()
driver = r'''
int main() {
  reset();
  assert(starttimer());
  nowUs += 1000; onTimer(); radioSafetyTick();
  nowUs += 1000; onTimer(); radioSafetyTick();
  assert(radioTimerOperational());
  for (int fault=0; fault<4; ++fault) {
    nowUs += 7ULL*86400ULL*1000000ULL;
    applicationLoopLastUs=radioTaskLastUs=nowUs;
    radioTimerLastUs=nowUs-300001;
    latchRadioFault(RADIO_FAULT_TIMER_STALLED);
    assert(radioFaultActive());
    const uint32_t before=timerAllocations;
    nowUs+=5000000ULL; radioTaskLastUs=nowUs;
    serviceRadioReliability();
    if (fault<3) {
      assert(timerAllocations==before+1);
      nowUs+=1000; onTimer(); radioSafetyTick(); serviceRadioReliability();
      nowUs+=1000; onTimer(); radioSafetyTick(); serviceRadioReliability();
      assert(!radioFaultActive() && radioTimerOperational());
    } else {
      assert(timerAllocations==before && radioFaultActive());
      for(int check=0;check<100;++check){nowUs+=10000000;radioTaskLastUs=nowUs;serviceRadioReliability();}
      assert(timerAllocations==before && radioFaultActive());
    }
  }
  std::printf("Three isolated timer stalls recovered across weeks; fourth transient stall receives no recovery attempt. retries=%u fault=%d.\n", radioTimerRetryCount(),radioFaultCode());
}
'''
unit = Path('/tmp/radioclock-postpub-retry-budget-repro.cpp')
unit.write_text(fixture+driver)
binary=unit.with_suffix('')
subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I',str(radio.FIRMWARE.parent),str(unit),'-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
