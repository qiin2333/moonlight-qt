# Transport policy Session integration

This developer driver exercises the production overlay menu inside a complete
streaming Session. It requires a Windows build with the existing
`MOONLIGHT_ENABLE_FUNCTION_TESTS=1` qmake environment option. Ordinary builds do
not compile the driver or expose its environment entry point.

Run a separately deployed portable application with `portable.dat`, a private
paired loopback host, and `MOONLIGHT_TRANSPORT_SESSION_TEST` set to the canonical
application directory. Never point the fixture at an existing installation or
reuse normal client settings, host state, credentials, or ports.

Place `transport-session-script.json` in that directory:

```json
{
  "steps": [
    {"action": "manual", "budgetKbps": 8000, "expectBitrate": false, "expectFec": false},
    {"action": "bitrate", "budgetKbps": 8000, "expectBitrate": true, "expectFec": false},
    {"action": "budget", "budgetKbps": 6000, "expectBitrate": true, "expectFec": false},
    {"action": "manual", "budgetKbps": 6000, "expectBitrate": false, "expectFec": false}
  ]
}
```

The current paired host advertises automatic FEC as unavailable. Verify its
disabled menu state and rejection of enable requests separately; a `fec` action
requires explicit host capability and cannot complete against this host.

The driver submits actual slider/gamepad menu events and records policy,
request revision, receipt phases, and menu state. A step requires the confirmed
mode and cap plus an SDK-applied and first-sent receipt in that control epoch.
An automatic successor may supply that receipt; the original request's phase
remains separately recorded. First send does not prove delivery or decoding.

Progress and final results use atomic JSON writes. Completion asks the Session
to exit normally. The runner must impose an outer timeout and clean up only
process handles it created. Missing or malformed results are failures.

PNG files run the same `paintContents()` method as the production QRasterWindow,
using its live state, layout, fonts, and device pixel ratio. Inspect them before
using them as layout evidence. This capture verifies painted layout; desktop
composition remains separate. Record Qt Quick backend and render loop separately
from the media decoder and renderer.

A `reconnect` action writes `transport-session-fault.json` with the old epoch
and network budget. The private runner may restart only the host process it
created. The driver waits for a different connection epoch, exact expected
startup budget/modes, and that epoch's SDK-applied and first-sent receipt before
continuing. Retain the old connection's records and audit each revision using
both connection epoch and revision. The overall driver timeout is 180 seconds.

Independently sample the paired host and verify revisions and immutable policy
fields against the driver. Also check negotiated packet control, legacy ABR
suppression, normal client exit, and actual decoder/render statistics. This
script does not cover arbitrary network faults, reliable notifications,
packet-budget auditing, latency, QoE, or device acceptance.

For native policy notification verification, use `"mode": "notifications"`
with passive `observe` steps at the actual initial host budget, then 6000 and
4000 Kbps. This fixture uses a 10000 Kbps client setting whose negotiated
initial legacy budget is 8656 Kbps; verify that value with paired HTTPS. Start with
packet feedback enabled and packet control disabled. The independent paired
runner changes the legacy budget; the Session driver performs no write. Each
step waits for the real query state and its SDK/first-send receipt and paints
the production menu before normal exit.

This mode saves `notification-events.jsonl` with accepted native metadata and
real paired query begin/end timestamps. The trace has a 512 KiB cap, serializes
file writes, and uses the same monotonic clock on the Session and HTTP worker.
The receive observation timestamp is taken before waking the worker. It records
no credentials or policy values beyond identity/revision and queried budget.
The worker captures only the validated portable path, never a Session pointer.
The production Controller keeps its one-second periodic interval and 250 ms
notification limit. Compare query timing against the preceding query end to
prove an early wake rather than assuming any new state came from a notice.
This mode does not establish reliable channel fault, resource cost, Android
device, desktop composition, or QoE acceptance.

For policy request faults, a developer build can set
`MOONLIGHT_TRANSPORT_FAULT_PROXY_PORT` to a private paired loopback TLS relay.
The port is used only with a validated Session driver, loopback address and
pinned host certificate. A policy action can set `"expectRequestError": true`.
It must observe an actual request error, wait for an actionable paired query,
then emit the same production menu action again to simulate an explicit retry.
The result retains `failedRequest` and `reconciledBeforeRetry` separately from
the retry's request revision and execution receipts. An absent error or a
second failed attempt fails the step. Production Controller retry behavior is
unchanged; this sequence belongs to the developer driver.

The private relay must obtain a real host 409 or withhold a real accepted
response through the HTTP deadline. Independently reconcile the original and
retry request IDs, expected revisions, immutable host policies and actual
SDK/first-send receipts. Keep failed fixtures, and do not replace responses or
receipts with synthetic success.
