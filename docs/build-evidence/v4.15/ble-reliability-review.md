### BLE recovery and final clock trust (findings 2 and 4)

The production shutdown path has one 60-second recovery episode covering scan acknowledgements, disconnected/no-client cases, retained-client teardown, partial initialization and controller deinit errors. Attempts and loop heartbeats never reset this deadline. The episode clears only after the host is stopped and the physical ESP controller is confirmed Idle. A settings change or a new manual request cannot reopen work while idle shutdown is recovering. An irrecoverable episode requests the existing safe restart after forcing RF off; this is fault recovery, not a periodic maintenance reboot.

Scan-command timeouts and failed low-memory stop requests retain the static queued command and request the same shutdown path. No scan event, client or callback storage is freed on a timeout. RF/BLE ownership remains closed throughout recovery. The existing independent loop monitor still covers a synchronous SDK deinit that never returns.

The real active-transaction cancellation predicate now rejects lost clock confidence, so the independent NimBLE host callout requests normal GAP termination even while loop is blocked inside ATT. BX and standard/analogue paths explicitly recheck clock trust before sampling final TIME. writeBt checks the active predicate immediately before ATT submission. The stack-backed ATT wait remains FOREVER: its normal callback/GAP cancellation completes before the stack context expires. Font, battery and packet formats are unchanged.

Reliability review: fixed loop-owned two-field deadline state adds no heap/flash writes. Unsigned 32-bit subtraction bounds the 60-second episode across millis rollover. The host only reads the existing lock-protected clock-trust snapshot; queue/controller teardown ownership and retained-client cache lifetimes stay unchanged. All post-failure RF gates remain closed until verified physical Off. An independent reviewer confirmed these paths before publication.

Focused evidence:
- `/tmp/radioclock-v415-ble-workflow.log`: real firmware workflow plus first-success/manual/profile/automatic/late ACK regressions, no-client unacknowledged scan-stop, repeated controller/partial-controller/idle shutdown errors, new work during recovery and deadline rollover; two unittest cases passed.
- `/tmp/radioclock-v415-ble-scan-control.log`: nine native scan-coordinator tests including sanitizer/leak run and 100 host cycles passed; no unsafe event replacement or device cleanup.
- `/tmp/radioclock-v415-ble-lifecycle.log`: ordinary and ASan/UBSan lifecycle tests passed, including blocked ATT clock-loss cancellation with callback completion before write return and 1,000 retained-client cycles.
- `/tmp/radioclock-v415-ble-focused.log`: captured font/TIME, bounded battery notifications and actual BX/standard final-trust checks passed. Injected trust loss during battery, font, connect and final clock sample withheld TIME and disconnected normally.

Physical limitation: host mocks prove software decisions and lifetimes, not ESP32 scheduler/radio behaviour. Node32s/watch repeated scan/connect/disconnect/controller recovery and clock-confidence-loss acceptance remain hardware tests. A permanently hung host/controller cannot be repaired safely by freeing live SDK objects; safe restart is the last resort.

### New RAM-only four-attempt watch history

BtRecentSyncs.h holds four events for each of four profiles in exactly 224 bytes. It stores UTC/outcome/protocol with the bound address/protocol identity in fixed arrays. Loop-only ownership matches synchronous API rendering and result handling; host callbacks never mutate the ring. Newest-first indexing/count stay bounded; boot construction starts empty. Binding changes clear only the affected profile. Failed replacement pairing cannot attach another watch's failure to the old binding. Unknown/untrusted failure timestamps are zero, not invented local dates.

The hook runs only after an actual perform-sync result; passive scanning, missing watches, entry-time blocks and RF deferrals never become history rows. Confirmed successes use the existing successful sync epoch; failures use the trusted clock or unknown. Existing LED, battery/font protocol and daily durable history policy are unchanged. The new ring has no storage writer or restore integration, no String allocation, no task allocation and no flash writes.

`/tmp/radioclock-v415-recent-syncs.log` passed both tests under ASan/UBSan/leak checks: one million fixed-size inserts, newest-four order, profile and address/protocol isolation, case-insensitive address reconciliation, replacement/invalid identities, unknown time, boot-empty state and actual firmware result-hook filtering. With existing daily persistence disabled, repeated real hook successes/failures leave the mock flash empty; confirmed replacement only queues the existing required binding-config save.
