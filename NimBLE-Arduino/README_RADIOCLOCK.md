# RadioClock pinned NimBLE source

This is **NimBLE-Arduino 2.5.1-radioclock.1**, based on the official Arduino
Library Manager 2.5.1 archive, SHA-256
`8fb09b1cde59f4ca5e6172d620c379968db959a883209289a9315b03eec27fe2`.
The original Apache-2.0 `LICENSE`, `NOTICE` and authors' credits are retained.
No compiled library is included.

RadioClock V4.14 build R2 requires this source instead of unmodified 2.5.1.
Install the entire `NimBLE-Arduino` folder in the Arduino sketchbook's `libraries`
directory, replacing the previous NimBLE-Arduino installation. Alternatively,
use **Sketch → Include Library → Add .ZIP Library** with the project's
`nimble-v2.5.1-radio-r2` library ZIP. Remove other competing NimBLE copies and
restart Arduino IDE. The sketch deliberately rejects an unpatched library.
Do not update this dependency through Library Manager until a reviewed release
includes these corrections.

The changes are limited to nine upstream files:

- Apply upstream commit
  [e0c8f5a558893197ae60c3606ded01a2dbb88863](https://github.com/h2zero/NimBLE-Arduino/commit/e0c8f5a558893197ae60c3606ded01a2dbb88863):
  stop the host timer without deinitializing a callback whose event may already
  be queued. The existing final host teardown still deinitializes the timer.
- Allocate the retained scanner and client objects using `std::nothrow` and
  handle a missing scanner at every library dereference.
- Check the client's connection-timer allocation, reject a failed client without
  occupying a factory slot, and only stop/delete a successfully initialized timer.
- Add a separate checked NPL timer initializer for the application monitor and
  client constructor. It returns allocation failure with an empty callout.
  Ordinary host timer initialization keeps the upstream abort/fail-stop behavior;
  its other callers are not silently given a new return-value contract.
- Mark this source with its version and compile-time patch identifier.

`RADIOCLOCK_SOURCE_MANIFEST.json` records every original file's SHA-256 and the
exact hashes of the nine modified files. `RADIOCLOCK_HOST_TIMER_FIX.patch` is the
original upstream patch. `scripts/validate-nimble-source.py` verifies those hashes
and the absence of compiled libraries; the cloud installer runs it before and
after installing the dependency.

The application also checks free internal heap and its largest available block
before starting BLE/scans/connections. Its independent host monitor requests
disconnect after a 45-second transaction deadline, including synchronous service
discovery, subscription and ATT writes. Synchronous SDK waits unwind through
their normal callbacks before stack storage expires. A stalled teardown remains
quarantined and requests RF-safe recovery after 60 seconds.

These checks do not make every SDK or STL allocation failure recoverable. The
host's generic timer/queue initialization and discovery/advertisement vector
allocations retain upstream behavior. The application reserves memory before
BLE work and monitors stalled tasks; real low-memory, repeated RF/BLE teardown
and long-running board tests remain required.
