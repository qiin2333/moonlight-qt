# moonlight-usb-agent

This target is the long-term process boundary for Remote USB. Build it with
`qmake usb-agent/usb_agent.pro` and start it with a private socket name and a
per-process token:

```sh
MOONLIGHT_USB_AGENT_TOKEN=<random> \
  moonlight-usb-agent --socket /tmp/moonlight-usb-agent.sock
```

The first line from Moonlight must be a `hello` request carrying the token.
After the `ready` event, commands are line-delimited JSON objects. The build
uses the pinned `moonlight-remote-usb-core` Rust submodule by default and runs
the shared Remote USB session, TLS channel, broker capability flow, and libusb
adapter inside the agent.

The production `start` request carries the selected device, paired host and
TLS identity, plus fresh stream/session/attachment/lease identifiers. All
64-bit identifiers and the request generation are decimal strings so JSON
cannot truncate them. The agent emits `opening` first and emits `opened` only
after Sunshine requests OPEN, the local device is claimed, and OPEN_OK is
queued. Every event echoes the request generation; stale events are ignored by
the Moonlight-side client.

The client private key crosses only the authenticated, owner-restricted local
socket. It is never placed on the command line, logged, or persisted by the
agent. A future privileged service split should replace it with an OS-backed
identity handle where the platform supports non-exportable keys.

The socket is owner-readable/writable on Unix. Moonlight must still treat the
token as a secret, rotate it whenever the agent is restarted, and close the
connection on any authentication or protocol error. The agent reads the token
only from `MOONLIGHT_USB_AGENT_TOKEN`, so it never appears in process listings.

Example runtime build:

```sh
git submodule update --init moonlight-remote-usb-core
qmake6 usb-agent/usb_agent.pro
make -j4
```

Packagers may set `MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR` and
`MOONLIGHT_REMOTE_USB_CORE_LIBRARY` to use a prebuilt Rust static library. The
header and library must come from the same core revision; startup also rejects
ABI or protocol versions that do not match the pinned v1 integration.
