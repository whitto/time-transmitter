from pathlib import Path
import sys
import subprocess
sys.path.insert(0, '/workspace/time-transmitter')
from tests import test_clock_and_encoders as clock

source = clock.FIRMWARE.read_text()
start = source.index('portMUX_TYPE clockMux')
end = source.index('//...................................................................', start)
names = ['configureAdaptiveNtp', 'ntpReplyPlausible', 'rejectNtpReply', 'onNtpSync', 'sntp_sync_time']
body = '\n'.join(clock.function(source, name) for name in names)
driver = r'''
int main() {
  const int64_t correctStart = 1791500000LL * 1000000LL;
  const int64_t wrongOffset = 3600LL * 1000000LL;
  struct timeval first{(time_t)((correctStart + wrongOffset) / 1000000LL), 0};
  sntp_sync_time(&first);
  assert(clockTrusted() && clockNtpSyncCount == 1);
  const int64_t stored = ntpLastEpochUs;
  for (int i = 1; i <= 5760; ++i) {
    mockMonoUs = 1000000ULL + (uint64_t)i * 15000000ULL;
    const int64_t corrected = correctStart + (int64_t)i * 15000000LL;
    struct timeval good{(time_t)(corrected / 1000000LL), (suseconds_t)(corrected % 1000000LL)};
    sntp_sync_time(&good);
    assert(!clockTrusted() && clockNtpSyncCount == 1);
    assert(ntpLastEpochUs == stored);
  }
  std::printf("Initial plausible reply accepted one hour ahead; 5760 correct replies over 24 hours all rejected. NTP accepted count=%u rejected count=%u confidence=%s.\n", clockNtpSyncCount, clockNtpRejectedCount, clockConfidence());
  // A one-off accepted NTP jitter sample can also lock the adaptive interval
  // below its own 300-second measurement admission threshold.
  clockNtpEverSynced=false; clockDriftSampleAvailable=false;
  clockNtpRejected=false; clockDriftBoundPpm=CLOCK_INITIAL_PPM;
  clockNtpSyncCount=0; ntpLastMonoUs=0; ntpLastEpochUs=0;
  ntpIntervalSec=NTP_INITIAL_INTERVAL_SEC; mockMonoUs=1000000ULL;
  struct timeval initial{(time_t)(correctStart/1000000LL),0};
  sntp_sync_time(&initial);
  mockMonoUs+=300000000ULL;
  struct timeval jitter{(time_t)(correctStart/1000000LL+300),500000};
  sntp_sync_time(&jitter);
  const uint32_t penaltyInterval=ntpIntervalSec.load();
  assert(penaltyInterval<300 && penaltyInterval>=15);
  const double penaltyBound=clockDriftBoundPpm;
  for (int reply=1;reply<=100;++reply) {
    mockMonoUs+=(uint64_t)penaltyInterval*1000000ULL;
    struct timeval perfect{(time_t)(correctStart/1000000LL+300+(int64_t)reply*penaltyInterval),0};
    sntp_sync_time(&perfect);
    assert(clockTrusted() && clockDriftBoundPpm==penaltyBound && ntpIntervalSec==penaltyInterval);
  }
  std::printf("One 500ms jitter sample at 300s produced %.3f ppm bound and %u-second interval; 100 perfect replies spanning %u seconds never update or reduce that bound.\n",penaltyBound,penaltyInterval,penaltyInterval*100);
}
'''
unit = Path('/tmp/radioclock-postpub-clock-repro.cpp')
unit.write_text(clock.MOCKS + source[start:end] + body + driver)
binary = unit.with_suffix('')
subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(unit),'-o',str(binary)], check=True)
subprocess.run([str(binary)], check=True)
