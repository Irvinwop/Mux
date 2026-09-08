# Browser-path and clipboard runtime qualification

`runtime-smoke` exercises clipboard behavior through real WPE pages displayed by
Kitty. The fixture server is
`scripts/runtime-smoke-browser-clipboard-fixture.py`; it binds an ephemeral
loopback port and accepts reports only from its exact origin through a random
per-run capability.

The qualification covers:

- A trusted copy followed by navigation and paste in the same pane.
  Mux's user-facing macOS bindings are Super/Command-first. This Linux-only
  harness deliberately sends `Ctrl+C` and `Ctrl+V` to qualify their documented
  compatibility aliases.
- A trusted copy in one Kitty/WPE pane followed by a paste in another pane of
  the same profile.
- A passive `navigator.clipboard.readText()` attempt without a user gesture.
  Rejection is preferred; an unavailable API is recorded separately and is
  accepted as non-exposure. Any resolved read fails the run.
- An ephemeral profile's Kitty-native history picker must not display the
  synthetic clipboard marker copied by another profile.
- An explicit trusted paste may transfer the current system clipboard into the
  isolated profile. This distinguishes global current-clipboard behavior from
  profile-scoped or disabled private history.

Every source and destination page reaches a title checkpoint backed by the
existing hashed `KITTY_FRAME_ACK` telemetry. Clipboard reports require trusted
copy, paste, and input events, exact byte length, and matching SHA-256 digests.
No clipboard payload is written to telemetry or logs.

New panes are identified by taking a complete `muxctl list` snapshot before a
single launch and requiring exactly one new view ID afterward. The isolated
view must also report the expected loopback fixture URL. This avoids depending
on profile metadata that `muxctl list` intentionally does not expose.

Headless Weston runs with its compatibility fake seat so Kitty receives the
keyboard-capable Wayland seat that owns clipboard selection state in a physical
session. The production Kitty configuration remains unchanged. The harness copies it
into an owner-only temporary directory and grants `send-key`, `send-text`, and
`get-text` only to that disposable Kitty instance. The outer timeout remains
authoritative, each poll has a shorter fixed deadline, and teardown terminates
all panes first. Profile engines are allowed to remain registered after their
last view closes while muxd is present. The harness then calls `muxctl stop`,
requires both profile engines and muxd to exit and be reaped within the bounded
lifecycle deadline, and only afterward terminates Kitty, Weston, and the
fixture server before checking that runtime sockets disappeared.
