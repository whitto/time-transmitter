#!/usr/bin/env python3
"""Exercise the shipped clock, timezone and BPC functions with host mocks.

The tests compile real sketch functions. SNTP's receive ordering is mocked:
its notification runs before the next timeout reads the configured interval.
The pinned SDK uses this ordering; hardware/network timing still needs a device.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/RadioClock_V4_16/RadioClock_V4_16.ino'


def function(source, name):
    match = re.search(r'^(?:extern "C" )?(?:static )?(?:void|bool|int|double|uint32_t|String|const char \*)\s*' +
                      name + r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(f'Function {name} missing')
    begin = source.index('{', match.start())
    depth, pos, state = 1, begin + 1, 'code'
    while depth:
        ch, pair = source[pos], source[pos:pos + 2]
        if state == 'line':
            if ch == '\n':
                state = 'code'
        elif state == 'block':
            if pair == '*/':
                state, pos = 'code', pos + 1
        elif state in ('"', "'"):
            if ch == '\\':
                pos += 1
            elif ch == state:
                state = 'code'
        elif pair == '//':
            state, pos = 'line', pos + 1
        elif pair == '/*':
            state, pos = 'block', pos + 1
        elif ch in ('"', "'"):
            state = ch
        elif ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        pos += 1
    return source[match.start():pos]


MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>
using String = std::string;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
std::atomic<int> ntpsync{1};
uint64_t mockMonoUs = 1000000;
int64_t esp_timer_get_time() { return mockMonoUs; }
int64_t restoredEpochUs = 0;
unsigned clockCommitCalls = 0;
extern std::atomic<uint32_t> radioClockStepGeneration;
bool setTimeOk = true;
int fakeSetTime(const struct timeval* tv, const void*) {
  assert(radioClockStepGeneration.load() & 1U);
  ++clockCommitCalls;
  if (!setTimeOk) return -1;
  restoredEpochUs = (int64_t)tv->tv_sec*1000000 + tv->tv_usec; return 0;
}
#define settimeofday fakeSetTime
uint32_t sdkIntervalMs = 3600000, sdkNextTimeoutMs = 0;
unsigned ntpRestarts = 0;
constexpr int SNTP_SYNC_STATUS_RESET=0,SNTP_SYNC_STATUS_COMPLETED=1;
int sdkSyncStatus=SNTP_SYNC_STATUS_RESET;
void sntp_set_sync_status(int status) { sdkSyncStatus=status; }
void sntp_set_sync_interval(uint32_t ms) { sdkIntervalMs = ms < 15000 ? 15000 : ms; }
void sntp_restart() { ++ntpRestarts; sdkNextTimeoutMs = 2000; }
String btTimezoneName = "Australia/Sydney", timezone_name = "UTC";
int btTimeOffsetMinutes = 0, transmission_offset_minutes = 0;
uint8_t txSymbol[60] = {}, txEnvelope[60][10] = {};
constexpr int SN_BPC = 6, SP_M4 = 4;
time_t fakeEpoch = 0;
time_t fakeTime(time_t* out) { if (out) *out = fakeEpoch; return fakeEpoch; }
#define time fakeTime
'''

CASES = r'''
time_t utc(int year, int month, int day, int hour = 0, int minute = 0, int second = 0) {
  struct tm input{};
  input.tm_year = year - 1900; input.tm_mon = month - 1; input.tm_mday = day;
  input.tm_hour = hour; input.tm_min = minute; input.tm_sec = second;
  return timegm(&input);
}
bool sameCivil(const struct tm& a, const struct tm& b) {
  return a.tm_year == b.tm_year && a.tm_mon == b.tm_mon && a.tm_mday == b.tm_mday &&
         a.tm_hour == b.tm_hour && a.tm_min == b.tm_min && a.tm_sec == b.tm_sec &&
         a.tm_wday == b.tm_wday && a.tm_yday == b.tm_yday && a.tm_isdst == b.tm_isdst;
}
void checkZone(const String& zone, time_t epoch) {
  btTimezoneName = zone;
  timezone_name = zone;
  applyTimezone();
  struct tm actual{}, expected{};
  bluetoothLocalTime(epoch, actual);
  localtime_r(&epoch, &expected);
  if (!sameCivil(actual, expected)) {
    fprintf(stderr, "%s at UTC epoch %lld: BT %04d-%02d-%02d %02d:%02d:%02d DST=%d; POSIX %04d-%02d-%02d %02d:%02d:%02d DST=%d\n",
      zone.c_str(), (long long)epoch,
      actual.tm_year+1900, actual.tm_mon+1, actual.tm_mday, actual.tm_hour, actual.tm_min, actual.tm_sec, actual.tm_isdst,
      expected.tm_year+1900, expected.tm_mon+1, expected.tm_mday, expected.tm_hour, expected.tm_min, expected.tm_sec, expected.tm_isdst);
    assert(false);
  }
}
void testTimezoneBoundaries() {
  // Sydney starts/ends on Sunday local time, Saturday UTC. 2018/2023
  // exercise the previous UTC month; 2000/2028/2100 cover leap rules.
  struct Boundary { int y,m,d; } boundaries[] = {
    {2026,10,3}, {2026,4,4}, {2023,9,30}, {2018,3,31},
    {2000,9,30}, {2000,4,1}, {2028,9,30}, {2028,4,1},
    {2100,10,2}, {2100,4,3}
  };
  for (const auto& b : boundaries) {
    const time_t transition = utc(b.y,b.m,b.d,16);
    for (int offset : {-86400,-1,0,1,86400})
      checkZone("Australia/Sydney", transition + offset);
  }
  for (const char* zone : {"Australia/Brisbane", "Australia/Sydney", "Asia/Tokyo", "Asia/Shanghai",
                           "Europe/London", "America/New_York", "America/Los_Angeles", "UTC"}) {
    for (int year : {2000,2026,2028,2100}) {
      for (time_t epoch = utc(year,1,1); epoch < utc(year+1,1,1); epoch += 6*3600)
        checkZone(zone, epoch);
    }
  }
}
void testTimezoneIsolation() {
  timezone_name = "Asia/Tokyo";
  applyTimezone();
  const String radioRule = getenv("TZ");
  const time_t epoch = utc(2026,10,3,16,15);
  struct tm before{}, after{}, bt{}, expected{};
  stationTime(epoch, SN_BPC, false, before);
  btTimezoneName = "Australia/Sydney";
  btTimeOffsetMinutes = -90;
  bluetoothLocalTime(epoch, bt);
  const time_t shifted = epoch + 11*3600 - 90*60;
  gmtime_r(&shifted, &expected);
  expected.tm_isdst = 1;
  assert(sameCivil(bt, expected));
  assert(String(getenv("TZ")) == radioRule);
  stationTime(epoch, SN_BPC, false, after);
  assert(sameCivil(before, after));
  assert(after.tm_hour == 1 && after.tm_mday == 4);
}
void resetClock() {
  radioClockStepGeneration = 0; clockCommitCalls = 0;
  ntpLastMonoUs = 0; ntpLastEpochUs = 0;
  ntpDriftPpm = 0; ntpDriftAbsSecPerHour = 0;
  clockNtpEverSynced = false; clockNtpSyncCount = 0;
  clockDriftSampleAvailable = false; clockDriftBoundPpm = CLOCK_INITIAL_PPM;
  clockNtpRejected = false; clockNtpRejectedCount = 0; restoredEpochUs = 0;
  setTimeOk = true;
  ntpCandidateReplies = clockNtpCandidateReplyCount = 0;
  ntpCandidateMonoUs = ntpCandidateLastMonoUs = 0; ntpCandidateEpochUs = 0;
  clockNtpReacquisitionCount = 0;
  ntpDriftAnchorMonoUs = 0; ntpDriftAnchorEpochUs = 0;
  ntpIntervalSec = NTP_INITIAL_INTERVAL_SEC;
  mockMonoUs = 1000000; sdkIntervalMs = 3600000;
  sdkNextTimeoutMs = 0; ntpRestarts = 0;
}
void receiveReply(int64_t epochUs) {
  const uint32_t generationBefore = radioClockStepGeneration.load();
  const unsigned commitsBefore = clockCommitCalls;
  struct timeval tv{(time_t)(epochUs/1000000), (suseconds_t)(epochUs%1000000)};
  // The pinned lwIP sntp_recv calls sntp_process / the notification
  // before it reads SNTP_UPDATE_DELAY to arm its next request.
  sntp_sync_time(&tv);
  assert(!(radioClockStepGeneration.load() & 1U));
  assert(radioClockStepGeneration.load() == generationBefore +
      (clockCommitCalls == commitsBefore ? 0U : 2U));
  sdkNextTimeoutMs = sdkIntervalMs;
}
void establishClock(int64_t epoch) {
  receiveReply(epoch);
  assert(!clockTrusted() && restoredEpochUs == 0 && clockNtpCandidateReplyCount == 1);
  mockMonoUs += 15000000ULL; receiveReply(epoch + 15000000LL);
  assert(!clockTrusted() && restoredEpochUs == 0 && clockNtpCandidateReplyCount == 2);
  mockMonoUs += 15000000ULL; receiveReply(epoch + 30000000LL);
  assert(clockTrusted() && clockNtpSyncCount == 1 && clockNtpCandidateReplyCount == 0);
}
void testNtpShortReplies() {
  resetClock();
  assert(clockAgeSeconds() == UINT32_MAX && !clockTrusted());
  const int64_t epoch = 1760000000LL*1000000;
  for (int reply=0; reply<8; ++reply) {
    mockMonoUs += 2000000;
    receiveReply(epoch + reply*2000000LL);
    assert(sdkNextTimeoutMs == NTP_MIN_INTERVAL_SEC*1000 && !clockTrusted());
  }
  // Too-fast notifications cannot manufacture independent acquisition.
  mockMonoUs += 16000000ULL; receiveReply(epoch + 30000000LL);
  assert(clockTrusted() && clockNtpSyncCount == 1 && clockNtpRejectedCount == 0);
  for (int reply=1;reply<=8;++reply) {
    mockMonoUs += 2000000ULL; receiveReply(epoch+30000000LL+reply*2000000LL);
  }
  assert(ntpRestarts == 0 && clockNtpSyncCount == 9);
  assert(!clockDriftSampleAvailable);
  assert(fabs(clockEstimatedError() - 0.20) < 1e-9);
  mockMonoUs += 18000ULL*1000000;
  assert(fabs(clockEstimatedError() - 1.10) < 1e-9);
  assert(!clockTrusted());
}
void measureDrift(double ppm) {
  resetClock();
  const int64_t epoch = 1760000000LL*1000000;
  establishClock(epoch);
  const int64_t anchor=ntpLastEpochUs;
  const int64_t elapsed = 3600LL*1000000;
  mockMonoUs += elapsed;
  receiveReply(anchor + elapsed + (int64_t)llround(elapsed*ppm/1000000.0));
  assert(clockDriftSampleAvailable);
  assert(fabs(ntpDriftPpm - ppm) < 1e-7);
  assert(sdkNextTimeoutMs == ntpIntervalSec.load()*1000);
  assert(ntpRestarts == 0);
}
void testAdaptiveTrust() {
  for (double ppm : {0.0,1.0,50.0,-50.0,1000.0}) {
    measureDrift(ppm);
    const uint32_t interval = ntpIntervalSec.load();
    assert(interval >= 15 && interval < CLOCK_MAX_HOLDOVER_SEC);
    mockMonoUs = ntpLastMonoUs + (interval+30ULL)*1000000;
    // Margin leaves time for a response, even at high measured drift.
    assert(clockEstimatedError() < 0.90 && clockTrusted());
    // Both endpoint uncertainty and absolute drift contribute to the bound.
    const double expectedBound = fabs(ppm) + 0.4 * 1000000.0 / 3600.0;
    assert(fabs(clockDriftBoundPpm-expectedBound) < 1e-7);
    const uint32_t expectedInterval=(uint32_t)(0.63*1000000.0/expectedBound);
    assert(interval == expectedInterval);
    mockMonoUs = ntpLastMonoUs + (CLOCK_MAX_HOLDOVER_SEC+1ULL)*1000000;
    assert(!clockTrusted());
  }
  // A valid zero-ppm measurement must still participate in EMA; zero
  // is a real sample, not a sentinel for "no measurement".
  measureDrift(0);
  mockMonoUs += 3600ULL*1000000;
  receiveReply(ntpLastEpochUs + 3600LL*1000000 + 360000);
  assert(fabs(ntpDriftPpm - 35.0) < 1e-7);
}
void testDriftReversalsAndRejectedCorrections() {
  measureDrift(50);
  for (int i=0; i<8; ++i) {
    const double ppm=i%2 ? 50.0 : -50.0;
    mockMonoUs += 3600ULL*1000000;
    receiveReply(ntpLastEpochUs+3600LL*1000000+(int64_t)(3600*ppm));
    assert(clockDriftBoundPpm >= 161.0);
    assert(clockTrusted());
  }
  // Signed EMA heads toward zero, while the trust envelope retains magnitude.
  assert(fabs(ntpDriftPpm) < 20 && clockDriftBoundPpm > 160);
  const uint32_t count=clockNtpSyncCount;
  const int64_t anchor=ntpLastEpochUs;
  mockMonoUs+=60000000;
  receiveReply(anchor+60LL*1000000+3600LL*1000000);
  assert(clockNtpSyncCount==count && !clockTrusted() && clockNtpRejectedCount==1);
  // Rejected time never reaches settimeofday, even briefly.
  assert(restoredEpochUs==anchor && sdkSyncStatus==SNTP_SYNC_STATUS_RESET && sdkNextTimeoutMs==15000);
  mockMonoUs+=15000000;
  receiveReply(anchor+75LL*1000000);
  assert(clockNtpSyncCount==count && !clockTrusted());
  mockMonoUs+=15000000; receiveReply(anchor+90LL*1000000);
  assert(clockNtpSyncCount==count && !clockTrusted());
  mockMonoUs+=15000000; receiveReply(anchor+105LL*1000000);
  assert(clockNtpSyncCount==count+1 && clockTrusted() && !clockNtpRejected);
  assert(clockNtpReacquisitionCount==1 && !clockDriftSampleAvailable);
  resetClock(); receiveReply(0);
  assert(!clockNtpEverSynced && clockNtpRejectedCount==1 && !clockTrusted());
  mockMonoUs=0x100000000ULL*1000000ULL+1234567;
  assert(monotonicUptimeSeconds()==0x100000001ULL);
  puts("Absolute uncertainty retains sign reversals; implausible corrections lose trust/restore anchor; 64-bit uptime");
}
void testAcquisitionAndBadAnchorRecovery() {
  resetClock();
  const int64_t epoch = 1791500000LL * 1000000LL;
  // The reproduced arbitrary one-hour-ahead first reply is never committed.
  receiveReply(epoch + 3600LL*1000000);
  assert(!clockTrusted() && restoredEpochUs == 0);
  for (int i=1;i<=3;++i) {
    mockMonoUs += 15000000ULL;
    receiveReply(epoch+i*15000000LL);
    if (i<3) assert(!clockTrusted() && restoredEpochUs==0);
  }
  assert(clockTrusted() && ntpLastEpochUs==epoch+45000000LL);
  // Even if three consistent wrong replies established an old bad anchor,
  // correct replies replace it in 30 seconds instead of weeks.
  resetClock();establishClock(epoch+3600LL*1000000);
  const uint64_t startMono = mockMonoUs;
  const int64_t correctStart=epoch+30000000LL;
  for (int i=1;i<=5760;++i) {
    mockMonoUs=startMono+(uint64_t)i*15000000ULL;
    receiveReply(correctStart+i*15000000LL);
    if(i<3) assert(!clockTrusted() && clockNtpSyncCount==1);
    else assert(clockTrusted() && restoredEpochUs==correctStart+i*15000000LL);
  }
  assert(clockNtpReacquisitionCount==1 && clockNtpSyncCount==5759);
  puts("One wrong initial reply never committed; accepted bad anchor recovered after three correct replies and stayed trusted for 24 hours");
}
void testAcquisitionMalformedAlternatingExpiredAndCommitFailure() {
  const int64_t epoch=1791500000LL*1000000LL;
  resetClock();
  for(int i=0;i<10;++i) {
    mockMonoUs+=15000000ULL;
    receiveReply(epoch+(int64_t)i*15000000LL+(i%2 ? 3600LL*1000000LL : 0));
    assert(!clockTrusted() && restoredEpochUs==0 && clockNtpCandidateReplyCount==1);
  }
  struct timeval malformed{(time_t)(epoch/1000000LL),1000000};
  sntp_sync_time(&malformed);
  assert(clockNtpCandidateReplyCount==0 && !clockTrusted());
  resetClock();receiveReply(epoch);
  mockMonoUs+=15000000ULL;receiveReply(epoch+15000000LL);
  mockMonoUs+=NTP_CANDIDATE_MAX_SPAN_US+1;
  receiveReply(epoch+15000000LL+(int64_t)NTP_CANDIDATE_MAX_SPAN_US+1);
  assert(!clockTrusted() && clockNtpCandidateReplyCount==1 && restoredEpochUs==0);
  resetClock();receiveReply(epoch);
  mockMonoUs+=15000000ULL;receiveReply(epoch+15000000LL);
  setTimeOk=false;mockMonoUs+=15000000ULL;receiveReply(epoch+30000000LL);
  assert(!clockTrusted() && !clockNtpEverSynced && restoredEpochUs==0);
  setTimeOk=true;mockMonoUs+=15000000ULL;receiveReply(epoch+45000000LL);
  assert(clockTrusted() && clockNtpSyncCount==1);
  puts("Alternating, malformed, expired and failed-commit NTP candidates cannot grant trust");
}
void testAdaptiveShortIntervalRecovers() {
  resetClock();const int64_t epoch=1791500000LL*1000000LL;establishClock(epoch);
  const int64_t anchor=ntpLastEpochUs;
  mockMonoUs+=300000000ULL;receiveReply(anchor+300000000LL+500000LL);
  const uint32_t penaltyInterval=ntpIntervalSec.load();
  const double penaltyBound=clockDriftBoundPpm;
  assert(penaltyInterval<300 && penaltyInterval>=15);
  for(int i=1;i<=100;++i) {
    mockMonoUs+=(uint64_t)penaltyInterval*1000000ULL;
    receiveReply(anchor+300000000LL+(int64_t)i*penaltyInterval*1000000LL);
    assert(clockTrusted());
  }
  assert(clockDriftBoundPpm<penaltyBound && ntpIntervalSec>penaltyInterval);
  printf("500ms jitter penalty recovered from %.3fppm/%us to %.3fppm/%us using accumulated sub-300s replies\n",
    penaltyBound,penaltyInterval,clockDriftBoundPpm,ntpIntervalSec.load());
}
unsigned bitCount(unsigned symbol) { return (symbol&1) + ((symbol>>1)&1); }
unsigned decode(const int* positions, int count, int base) {
  unsigned result = 0;
  for (int i=0; i<count; ++i) {
    const int bit = positions[i];
    result = (result<<1) | ((txSymbol[base+bit/2] >> (1-bit%2)) & 1);
  }
  return result;
}
void testBpcBlocks() {
  timezone_name = "UTC";
  transmission_offset_minutes = 0;
  applyTimezone();
  const int hourBits[] = {6,7,8,9}, minuteBits[] = {10,11,12,13,14,15};
  const int dowBits[] = {17,18,19}, dayBits[] = {23,24,25,26,27};
  const int monthBits[] = {28,29,30,31}, yearBits[] = {38,32,33,34,35,36,37};
  for (int sec : {0,19,20,39,40,59}) {
    fakeEpoch = utc(2026,10,7,23,57,sec);
    mb_bpc();
    for (int block=0; block<3; ++block) {
      const int base = block*20;
      assert(txSymbol[base] == SP_M4);
      assert(txSymbol[base+1] == block);
      assert(decode(hourBits,4,base) == 11 && decode(minuteBits,6,base) == 57);
      assert(decode(dowBits,3,base) == 3 && decode(dayBits,5,base) == 7);
      assert(decode(monthBits,4,base) == 10 && decode(yearBits,7,base) == 26);
      assert((txSymbol[base+10] >> 1) == 1); // PM flag
      unsigned parity = 0;
      for (int s=1; s<=9; ++s) parity ^= bitCount(txSymbol[base+s]) & 1;
      assert((txSymbol[base+10]&1) == parity);
      parity = 0;
      for (int s=11; s<=18; ++s) parity ^= bitCount(txSymbol[base+s]) & 1;
      assert((txSymbol[base+19]&1) == parity);
      for (int s=0; s<20; ++s) {
        for (int k=0; k<10; ++k) {
          const unsigned expected = s == 0 ? 1 : (k < txSymbol[base+s]+1 ? 2 : 1);
          assert(txEnvelope[base+s][k] == expected);
        }
      }
    }
  }
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const String test = argv[1];
  if (test == "timezone") testTimezoneBoundaries();
  else if (test == "isolation") testTimezoneIsolation();
  else if (test == "short_replies") testNtpShortReplies();
  else if (test == "adaptive") testAdaptiveTrust();
  else if (test == "reliability") testDriftReversalsAndRejectedCorrections();
  else if (test == "acquisition") testAcquisitionAndBadAnchorRecovery();
  else if (test == "candidate_faults") testAcquisitionMalformedAlternatingExpiredAndCommitFailure();
  else if (test == "short_recovery") testAdaptiveShortIntervalRecovers();
  else if (test == "bpc") testBpcBlocks();
  else assert(false);
  puts("PASS");
}
'''


class ClockAndEncoderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('g++'):
            raise RuntimeError('g++ is required for firmware clock/encoder tests')
        source = FIRMWARE.read_text()
        start = source.index('portMUX_TYPE clockMux')
        end = source.index('//...................................................................', start)
        clock_globals = source[start:end]
        functions = ['btWeekday', 'btNthSunday', 'btLastSunday', 'btDayOfYear',
                     'btDstAtUtc', 'btBaseOffsetSeconds', 'bluetoothLocalTime',
                     'posixTzFor', 'applyTimezone', 'stationTime', 'clearTxFrame',
                     'mb_bpc', 'configureAdaptiveNtp', 'ntpReplyPlausible', 'rejectNtpReply',
                     'admitNtpReply', 'recordAcceptedNtpReply', 'onNtpSync', 'sntp_sync_time']
        body = '\n'.join(function(source, name) for name in functions)
        cls.directory = tempfile.TemporaryDirectory(prefix='radioclock-clock-tests-')
        path = Path(cls.directory.name)
        cpp = path / 'clock.cpp'
        cpp.write_text(MOCKS + clock_globals + body + CASES)
        cls.binary = path / 'clock'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                        str(cpp), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_timezone_boundaries_leap_years_and_all_supported_zones(self):
        self.run_case('timezone')

    def test_bluetooth_timezone_and_offset_do_not_change_radio_time(self):
        self.run_case('isolation')

    def test_short_sntp_replies_keep_conservative_holdover_and_do_not_restart(self):
        self.run_case('short_replies')

    def test_live_sntp_interval_resyncs_before_error_and_holdover_limits(self):
        self.run_case('adaptive')

    def test_absolute_drift_envelope_rejected_corrections_and_multi_year_uptime(self):
        self.run_case('reliability')

    def test_initial_candidate_and_prolonged_bad_anchor_reacquisition(self):
        self.run_case('acquisition')

    def test_malformed_alternating_expired_and_failed_commit_candidates(self):
        self.run_case('candidate_faults')

    def test_short_interval_jitter_penalty_can_recover(self):
        self.run_case('short_recovery')

    def test_bpc_block_numbers_parity_fields_and_pulse_envelopes(self):
        self.run_case('bpc')


if __name__ == '__main__':
    unittest.main()
