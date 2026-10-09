#!/usr/bin/env python3
"""Exercise the production scan coordinator with a separate native host task.

The completion fixture matches the important NimBLE ordering: GAP becomes
inactive while DISC_COMPLETE still holds an advertised device. Commands must
wait on the host queue instead of clearing that live device from loopTask.
The tests cover the real header; NPL and the radio remain native fixtures.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/RadioClock_V4_15'


FIXTURE = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ble_npl_event {
  void (*fn)(ble_npl_event*) = nullptr;
  void* argument = nullptr;
  bool queued = false;
};

std::atomic<uint32_t> clockTicks{0};
uint32_t millis() { return clockTicks.load(); }
void delay(uint32_t ms) {
  clockTicks.fetch_add(ms);
  // Virtual milliseconds keep timeouts short; the sleep lets the independent
  // host task run instead of relying on a synchronous callback in delay().
  std::this_thread::sleep_for(std::chrono::microseconds(100));
}

class HostQueue {
public:
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::function<void()>> jobs;
  std::vector<ble_npl_event*> submitted;
  std::atomic<bool> running{false};
  std::thread worker;
  std::thread::id hostId;
  bool stopping = false;

  void start() {
    stopping = false;
    worker = std::thread([this] {
      {
        std::lock_guard<std::mutex> lock(mutex);
        hostId = std::this_thread::get_id();
        running = true;
      }
      changed.notify_all();
      std::unique_lock<std::mutex> lock(mutex);
      for (;;) {
        changed.wait(lock, [this] { return stopping || !jobs.empty(); });
        if (stopping && jobs.empty()) break;
        auto job = std::move(jobs.front());
        jobs.pop_front();
        lock.unlock();
        job();
        lock.lock();
      }
      running = false;
    });
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [this] { return running.load(); });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    changed.notify_all();
    worker.join();
    assert(jobs.empty());
  }

  void putJob(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      assert(running);
      jobs.push_back(std::move(job));
    }
    changed.notify_all();
  }

  size_t submissions() {
    std::lock_guard<std::mutex> lock(mutex);
    return submitted.size();
  }

  void waitForSubmissions(size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(lock, std::chrono::seconds(5),
                           [this, count] { return submitted.size() >= count; }));
  }
};

HostQueue* hostQueue = nullptr;
int eventInitializations = 0, eventReleases = 0;
void ble_npl_event_init(ble_npl_event* event, void (*callback)(ble_npl_event*), void* argument) {
  assert(!event->fn && !event->queued);
  ++eventInitializations;
  event->fn = callback;
  event->argument = argument;
}
void ble_npl_event_deinit(ble_npl_event* event) {
  assert(!hostQueue->running && !event->queued);
  ++eventReleases;
  event->fn = nullptr;
  event->argument = nullptr;
}
void* ble_npl_event_get_arg(ble_npl_event* event) { return event->argument; }
HostQueue* nimble_port_get_dflt_eventq() { return hostQueue; }
void ble_npl_eventq_put(HostQueue* queue, ble_npl_event* event) {
  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    assert(queue->running && event->fn && !event->queued);
    event->queued = true;
    queue->submitted.push_back(event);
    queue->jobs.push_back([queue, event] {
      {
        std::lock_guard<std::mutex> lock(queue->mutex);
        event->queued = false;
      }
      assert(event->fn);
      event->fn(event);
    });
  }
  queue->changed.notify_all();
}

std::atomic<bool> rfRequested{false};
bool canStartScan() { return !rfRequested.load(); }

struct Device {
  std::atomic<int>& freed;
  int value = 42;
  explicit Device(std::atomic<int>& counter) : freed(counter) {}
  ~Device() { ++freed; }
};

class NimBLEScan {
public:
  std::atomic<bool> scanning{false}, startOk{true}, stopOk{true};
  std::atomic<int> starts{0}, stops{0}, clears{0}, freed{0}, completions{0};
  std::vector<std::unique_ptr<Device>> results;
  std::mutex completionMutex;
  std::condition_variable completionChanged;
  bool held = false, completionReleased = false;

  bool isScanning() const { return scanning.load(); }

  void assertHost() const {
    assert(hostQueue->running);
    assert(std::this_thread::get_id() == hostQueue->hostId);
  }

  void clearResults() {
    assertHost();
    {
      std::lock_guard<std::mutex> lock(completionMutex);
      assert(!held && "Cannot delete the device still used by DISC_COMPLETE");
    }
    ++clears;
    results.clear();
  }

  bool start(uint32_t duration, bool continuing, bool restart) {
    assertHost();
    assert(duration == 1000 && !continuing && restart);
    ++starts;
    clearResults();
    scanning = startOk.load();
    return startOk;
  }

  bool stop() {
    assertHost();
    ++stops;
    if (!stopOk) return false;
    scanning = false;
    clearResults();
    return true;
  }

  void holdCompletion() {
    {
      std::lock_guard<std::mutex> lock(completionMutex);
      held = false;
      completionReleased = false;
    }
    hostQueue->putJob([this] {
      assertHost();
      results.push_back(std::make_unique<Device>(freed));
      Device* callbackDevice = results.back().get();
      // GAP status becomes inactive before the host finishes its callback.
      scanning = false;
      {
        std::unique_lock<std::mutex> lock(completionMutex);
        held = true;
        completionChanged.notify_all();
        completionChanged.wait(lock, [this] { return completionReleased; });
        assert(callbackDevice->value == 42);
        held = false;
      }
      ++completions;
      clearResults();
    });
    std::unique_lock<std::mutex> lock(completionMutex);
    assert(completionChanged.wait_for(lock, std::chrono::seconds(5), [this] { return held; }));
  }

  void releaseCompletion() {
    {
      std::lock_guard<std::mutex> lock(completionMutex);
      assert(held);
      completionReleased = true;
    }
    completionChanged.notify_all();
  }
};

#include "RadioBleScanControl.h"

class Fixture {
public:
  HostQueue queue;
  NimBLEScan scan;
  RadioBleScanControl control;
  Fixture() {
    clockTicks = 0;
    rfRequested = false;
    hostQueue = &queue;
    queue.start();
    assert(control.initialize(&scan, 1000, canStartScan));
  }
  void finish() {
    assert(control.stop() && control.quiescent());
    queue.stop();
    assert(control.releaseAfterHostStop());
  }
};

void queuedStart() {
  Fixture f;
  f.scan.holdCompletion();
  assert(!f.scan.isScanning());
  bool started = false;
  std::thread loop([&] { started = f.control.start(); });
  f.queue.waitForSubmissions(1);
  assert(!f.control.quiescent());
  assert(f.scan.starts == 0 && f.scan.freed == 0 && f.scan.clears == 0);
  f.scan.releaseCompletion();
  loop.join();
  assert(started && f.scan.starts == 1 && f.scan.completions == 1 && f.scan.freed == 1);
  assert(!f.control.quiescent());
  f.finish();
}

void queuedStop() {
  Fixture f;
  assert(f.control.start());
  f.scan.holdCompletion();
  const size_t previous = f.queue.submissions();
  bool stopped = false;
  std::thread loop([&] { stopped = f.control.stop(); });
  f.queue.waitForSubmissions(previous + 1);
  assert(!f.control.quiescent() && f.scan.stops == 0 && f.scan.freed == 0);
  f.scan.releaseCompletion();
  loop.join();
  assert(stopped && f.control.quiescent() && f.scan.stops == 1 && f.scan.freed == 1);
  f.finish();
}

void pendingStartTimeout() {
  Fixture f;
  f.scan.holdCompletion();
  uint32_t before = millis();
  assert(!f.control.start() && millis() - before >= 2000);
  assert(f.queue.submissions() == 1 && !f.control.quiescent());
  assert(!f.control.initialize(&f.scan, 1000, canStartScan));
  assert(!f.control.releaseAfterHostStop());
  assert(!f.control.stop());
  assert(f.queue.submissions() == 1 && f.scan.starts == 0 && f.scan.stops == 0);
  f.scan.releaseCompletion();
  // stop() must wait for the old START acknowledgement, then queue STOP;
  // overwriting the pending command would incorrectly leave starts at zero.
  assert(f.control.stop());
  assert(f.scan.starts == 1 && f.scan.stops == 1 && f.control.quiescent());
  {
    std::lock_guard<std::mutex> lock(f.queue.mutex);
    assert(f.queue.submitted.size() == 2);
    assert(f.queue.submitted[0] == f.queue.submitted[1]);
  }
  f.finish();
}

void lateStopAcknowledgement() {
  Fixture f;
  assert(f.control.start());
  f.scan.holdCompletion();
  assert(!f.control.stop() && !f.control.quiescent());
  assert(f.queue.submissions() == 2 && f.scan.stops == 0);
  assert(!f.control.releaseAfterHostStop());
  f.scan.releaseCompletion();
  // A late successful acknowledgement permits RF handoff without needing to
  // reset the coordinator or release an event still owned by the host.
  for (int i = 0; i < 5000 && !f.control.quiescent(); ++i) delay(1);
  assert(f.control.quiescent() && f.scan.stops == 1 && f.scan.freed == 1);
  f.finish();
}

void failedStop() {
  Fixture f;
  assert(f.control.start());
  f.scan.stopOk = false;
  assert(!f.control.stop() && !f.control.quiescent());
  // A failed STOP remains unsafe even if GAP already reports inactive.
  f.scan.scanning = false;
  assert(!f.control.stop() && !f.control.quiescent());
  assert(!f.control.releaseAfterHostStop());
  f.scan.stopOk = true;
  assert(f.control.stop() && f.control.quiescent());
  f.finish();
}

void rfPriority() {
  Fixture f;
  f.scan.holdCompletion();
  bool started = true;
  std::thread loop([&] { started = f.control.start(); });
  f.queue.waitForSubmissions(1);
  rfRequested = true;
  f.scan.releaseCompletion();
  loop.join();
  assert(!started && f.scan.starts == 0 && !f.scan.isScanning());
  assert(f.control.quiescent());
  f.finish();
}

void failedStartAndAlreadyActive() {
  Fixture f;
  f.scan.startOk = false;
  assert(!f.control.start() && f.control.quiescent());
  f.scan.startOk = true;
  assert(f.control.start() && !f.control.quiescent());
  int starts = f.scan.starts;
  assert(f.control.start() && f.scan.starts == starts);
  f.finish();
}

void lifecycleCycles() {
  Fixture f;
  const int initialized = eventInitializations;
  const int released = eventReleases;
  for (int i = 0; i < 100; ++i) {
    assert(f.control.start());
    assert(f.control.stop() && f.control.quiescent());
    f.queue.stop();
    assert(f.control.releaseAfterHostStop());
    assert(f.control.quiescent());
    assert(f.control.releaseAfterHostStop()); // Idempotent, no second deinit.
    f.queue.start();
    assert(f.control.initialize(&f.scan, 1000, canStartScan));
  }
  assert(eventInitializations == initialized + 100);
  assert(eventReleases == released + 100);
  f.finish();
}

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  const std::vector<std::pair<std::string, void (*)()>> cases = {
    {"queued-start", queuedStart}, {"queued-stop", queuedStop},
    {"start-timeout", pendingStartTimeout}, {"late-stop", lateStopAcknowledgement},
    {"failed-stop", failedStop}, {"rf-priority", rfPriority},
    {"start-failure", failedStartAndAlreadyActive}, {"cycles", lifecycleCycles}
  };
  bool matched = false;
  for (const auto& item : cases) {
    if (mode == "all" || mode == item.first) {
      matched = true;
      item.second();
      std::printf("Scan coordinator scenario passed: %s\n", item.first.c_str());
    }
  }
  assert(matched);
}
'''


class BleScanControlTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('g++'):
            raise unittest.SkipTest('g++ is required for native host-task fixtures')
        cls.temporary = tempfile.TemporaryDirectory(prefix='radioclock-scan-control-')
        directory = Path(cls.temporary.name)
        unit = directory / 'test.cpp'
        unit.write_text(FIXTURE)
        cls.binary = directory / 'test'
        cls.sanitized = directory / 'test-asan'
        command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                   '-I', str(FIRMWARE), str(unit)]
        subprocess.run(command + ['-o', str(cls.binary)], check=True)
        subprocess.run(command + ['-g', '-O1', '-fsanitize=address,undefined',
                                  '-fno-omit-frame-pointer', '-no-pie',
                                  '-o', str(cls.sanitized)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_case(self, mode):
        subprocess.run([str(self.binary), mode], check=True, timeout=15)

    def test_start_waits_until_completion_releases_its_device(self):
        self.run_case('queued-start')

    def test_stop_is_a_host_queue_barrier_when_gap_is_inactive(self):
        self.run_case('queued-stop')

    def test_timeout_retains_event_and_start_until_acknowledged(self):
        self.run_case('start-timeout')

    def test_late_stop_acknowledgement_restores_quiescence(self):
        self.run_case('late-stop')

    def test_failed_stop_blocks_rf_handoff_even_with_inactive_gap(self):
        self.run_case('failed-stop')

    def test_host_rechecks_rf_priority_before_queued_start(self):
        self.run_case('rf-priority')

    def test_failed_start_and_already_active_scan(self):
        self.run_case('start-failure')

    def test_event_survives_one_hundred_host_restart_cycles(self):
        self.run_case('cycles')

    def test_all_scenarios_with_address_and_undefined_behavior_sanitizers(self):
        environment = os.environ.copy()
        environment['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'
        environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        subprocess.run([str(self.sanitized), 'all'], env=environment, check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
