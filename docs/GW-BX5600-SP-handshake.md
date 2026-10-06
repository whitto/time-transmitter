# GW-BX5600 SP handshake correction (BX1)

The reported pre-V3.5 log reaches characteristic discovery, then fails an SP_DATA
Write Request. Disconnect reason 534 is NimBLE's HCI error base 512 plus 0x16,
“Connection Terminated By Local Host.” It describes cleanup after failure; the
old log does not identify the failed step, negotiated MTU, or ATT status.

## Packet evidence

Public official-app btsnoop captures in
[izivkov/gshock_api at ae817310](https://github.com/izivkov/gshock_api/tree/ae817310dd9551d5e6e53ee61d89af4e8d954767/test_data)
contain four complete exchanges across these files:

- `btsnoop_hci_bx.log`
- `btsnoop_hci-bx2.log`
- `btsnoop_hci_bx3_official_app.log`

Reassembling HCI ACL/L2CAP continuations before decoding ATT gives:

| Step | Notification | Old SP_DATA write | BX1 SP_DATA write |
| --- | --- | --- | --- |
| Settings | 101 bytes: two 1D records, three 24 city records | 101 bytes, leading 02 | 35 bytes: leading 02, two 1D records |
| DST/cities | 28 bytes: three 1E records | 28 bytes, leading 06 | 94 bytes: leading 06, three 1E records plus the saved city records |
| Alarms | 133 bytes: six 1F records | 133 bytes unchanged | 133 bytes unchanged |

The first byte matching record count is a structural interpretation supported
by every captured request, response, and write. Records have a little-endian
length followed by tag and slot. BX1 checks exact response size, count, record
lengths, tags, and slots before constructing any write. Unknown setting bytes,
city data, DST data, and alarm values are preserved.

The official captures also change two bytes in the second 1D record from 00 to
FF and sometimes update home-city coordinates. Their meaning and necessity for
time-only delivery have not been established. BX1 preserves those existing
values. These captures include surrounding initialization activity and do not
establish the exact watch button mode or prove this time-only variant works on
the user's watch.

## ATT transport and diagnostics

SP_REQUEST remains Write Without Response. SP_DATA and TIME remain Write With
Response. BX1 subscribes specifically to SP_DATA and ignores other notifying
characteristics when assembling its responses.

BX1 requests preferred MTU 517 and waits for negotiated MTU of at least 136,
which accommodates the largest 133-byte value plus three ATT bytes. The public
captures negotiate 512. Each protocol write uses a single ATT Write Request or
Command; insufficient MTU rejects the complete packet. This avoids NimBLE
2.5.1's long-write fallback, which can retry a truncated prefix after
`Attribute Not Long` and report success for that prefix.

Writes log stage, UUID on failure, payload length/header, negotiated MTU, numeric
host/ATT status and ATT error handle. Status in 0x100–0x1ff also logs the raw ATT
error byte. The callback captures the write's actual result; client
`getLastError()` does not expose this result. Authentication errors may trigger
one secure-connection retry of the complete original packet.

Callback storage stays alive until ATT completes or GAP cancellation releases
the waiting task. The independent host monitor still terminates BLE for RF
demand. Retained client lifetime, teardown barrier, and controller-off checks
continue to protect the RF/BLE ownership handoff.

## Device check

Startup identifies this build as `firmware V3.5 build BX1`. An exchange should
show MTU at least 136, response sizes 101/28/133, and SP_DATA write sizes
35/94/133 before the 11-byte TIME command. Confirm the watch's actual displayed
time and success indicator. If it fails, retain the complete log from connection
through disconnect; the new stage and ATT status distinguish framing, MTU,
authentication, timeout, and other rejection paths.

Host regression tests and an ESP32 compile can verify implementation behavior;
watch acceptance requires a physical retry.
