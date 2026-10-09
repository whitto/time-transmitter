#pragma once
#include <atomic>

// Ownership spans the entire controller lifetime, including initialization and
// teardown, and every RF envelope (including its reduced/zero-carrier slots).
// Only radioTask releases RF, after writing duty=0. Only loopTask releases BLE,
// after verifying the host and controller are stopped. Failed shutdown keeps
// BLE ownership and therefore keeps RF off.
class RadioBleArbiter {
public:
  enum class Owner : unsigned char { Idle, Ble, Rf };

  bool acquireBle() {
    if (rfDemand_.load()) return false;
    Owner expected = Owner::Idle;
    if (!owner_.compare_exchange_strong(expected, Owner::Ble)) return false;
    // A concurrent RF request can never acquire while BLE holds ownership.
    if (rfDemand_.load()) {
      owner_.store(Owner::Idle); // Initialization has not started yet.
      return false;
    }
    return true;
  }

  bool requestRf() {
    rfDemand_.store(true);
    Owner expected = Owner::Idle;
    return owner_.compare_exchange_strong(expected, Owner::Rf) ||
           expected == Owner::Rf;
  }

  bool finishBleShutdown(bool controllerOff) {
    if (!controllerOff) return false;
    Owner expected = Owner::Ble;
    return owner_.compare_exchange_strong(expected, Owner::Idle) ||
           expected == Owner::Idle;
  }

  // Caller must physically silence RF BEFORE relinquishing ownership.
  void releaseRf() {
    Owner expected = Owner::Rf;
    owner_.compare_exchange_strong(expected, Owner::Idle);
    rfDemand_.store(false);
  }

  Owner owner() const { return owner_.load(); }
  bool rfRequested() const { return rfDemand_.load(); }
  bool rfOwned() const { return owner() == Owner::Rf; }
  bool bleOwned() const { return owner() == Owner::Ble; }

private:
  std::atomic<Owner> owner_{Owner::Idle};
  std::atomic<bool> rfDemand_{false};
};
