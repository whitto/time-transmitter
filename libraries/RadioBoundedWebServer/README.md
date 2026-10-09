# RadioBoundedWebServer 3.3.12-radioclock.1

Required source dependency for RadioClock V4.15. Install this folder as an
Arduino library alongside NimBLE-Arduino and RadioCrashDumpGate. The unique
`RadioBoundedWebServer.h` header/class prevents accidental selection of the
ESP32 core's unchanged WebServer. No binaries or board flash are included.

The 18 upstream `src` files and metadata were downloaded from Espressif's
Arduino-ESP32 **3.3.12**, commit
`94afccf35fb1e401facddbcf9e13bcf7c76a31d8`, and compared byte-for-byte with
the retained pinned toolchain. Original hashes are in
`RADIOCLOCK_UPSTREAM_SHA256.json`. Upstream source is LGPL 2.1 or later;
original author notices are retained and the licence accompanies the source.
Ivan Grokhotkov, Hristo Gochkov and Espressif contributors wrote the original
server. Time Transmitter contributors added the bounds described below.

The namespace/class/header rename applies throughout the copied source.
`RADIOCLOCK_PATCH.diff` records behavioral changes against the renamed
upstream copy. `RADIOCLOCK_SOURCE_MANIFEST.json` records every supplied file;
`scripts/validate-bounded-webserver-source.py` rejects missing, modified,
additional or binary files before installation and packaging.

## RadioClock HTTP limits

- Body: at most **4096 bytes**, strict decimal Content-Length validated before
  any body allocation. Duplicate lengths, arithmetic overflow and
  Transfer-Encoding/chunked requests are rejected before allocation.
- Request line/headers: at most **1024 bytes per line**, **4096 total bytes**,
  512-byte URI and 64 parsed query/form arguments. A complete line must arrive
  within two seconds; all request headers within five seconds.
- The whole header/body receive phase has one **10-second absolute budget**.
  Body fragments do not restart it. The body uses one checked allocation of
  the admitted length plus its terminator, and reads exact remaining bytes.
- Existing browser FormData is retained. Ordinary multipart fields are
  parsed from the already bounded body in RAM: at most 32 fields, 64 bytes per
  name, 70-byte boundary and 1024 bytes of per-part headers. UTF-8, empty
  fields and embedded newlines survive. File uploads/raw callbacks are
  unsupported; JSON and URL-encoded forms remain supported.
- Multipart and URL argument candidates are complete before handler dispatch;
  allocation/decoding failures reject the request. Protocol line allocation
  failures are checked. Existing settings are not applied from partial fields.
- `checkedArg()` verifies handler-side String copies against the admitted
  original. The Wi-Fi handler checks both credential copies and validates
  their byte lengths before changing either live value.
- No SDK unbounded `client.clear()` runs after parsing. The single request
  uses the existing `Connection: close` response and then releases its client.
- Header/body response writes share an absolute **five-second** socket-send
  budget; partial sends do not restart it. Failed writes stop the client.
  Error responses use the same bounded writer plus a 50-ms bounded RX drain.

Fault fixtures execute the shipped SDK-derived `_parseRequest()`, body reader,
argument parser, multipart parser and response writer under native ASan/UBSan.
They cover the former 313.6-second slow-body reproduction, combined budgets,
rollover, maximum body/excess input, header/body allocation failure,
multipart compatibility and partial-send failure. This does not establish
ESP32 TCP timing or stack headroom under a real long-duration hardware soak.
The unused upstream streaming-upload implementation is removed so it cannot
accidentally reopen an unbounded path.
