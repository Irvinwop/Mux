# Mux

Mux is a personal, Kitty-native browser multiplexer. Kitty supplies tabs,
splits, layouts, keyboard routing, and trusted overlays. WPE WebKit remains the
real browser engine, while Mux owns the browser control plane and presents each
page inside a Kitty pane.

Mux does not encode screenshots or stream PNG files. WebKit damage is copied as
raw RGBA through short-lived POSIX shared-memory objects and applied to one
Kitty graphics image in place.

## Quick start

Mux currently targets a local Linux desktop with Kitty, GLib 2.74, and WPE
WebKit 2.52 or newer.

```sh
./setup
```

`setup` installs packages using `pacman`, `apt-get`, or `dnf`, runs the
environment doctor, builds the project, and launches its dedicated Kitty
instance. Pass an initial page directly:

```sh
./setup https://example.com
```

After dependencies are installed, normal starts are incremental:

```sh
./mux
./mux https://example.com
```

Run `./doctor` for a read-only dependency report. Detailed package information
is in [docs/install.md](docs/install.md).

## Process model

| Process | Responsibility |
| --- | --- |
| `muxd` | Control socket, logical layers, pane routing, and profile clipboard histories. |
| `mux-engine` | Profile-specific WPE display, WebKit session, network state, and page ownership. |
| `mux-pane` | One Kitty pane's input, raw-frame presentation, clipboard link, and trusted UI overlay. |
| `mux-bar` | Global URL and status bar that follows the focused pane. |
| `mux-layer` | Creates and restores a logical collection of browser panes. |
| `muxctl` | Scriptable control client for navigation and layout operations. |
| Kitty | Tabs, splits, OS windows, layouts, and graphics presentation. |

WebKit still uses its sandboxed web, network, and GPU processes. Centralizing
browser state does not collapse web content into `muxd` or `mux-engine`.

## Browser behavior

The current implementation includes:

- Persistent and ephemeral WebKit profiles.
- Multiple page views sharing one profile engine.
- Logical layers across Kitty tabs and windows.
- A focus-aware global URL bar.
- Keyboard, pointer, focus, resize, and high-DPI input forwarding.
- Script dialogs, permission decisions, HTTP authentication, and file choosers.
- Permission-gated desktop notifications with click and close reporting.
- Downloads, popup/new-window attachment, native option menus, and context menus.
- Web-process crash prompts with reload and close actions.
- Full-MIME clipboard synchronization and profile-isolated clipboard history.
- Pin, delete, clear, select, and fuzzy-pick clipboard-history operations.

Clipboard snapshots retain every observed MIME representation atomically,
including images and arbitrary binary data. History is memory-only and bounded;
pages can access only the current clipboard through WebKit's normal permission
rules and cannot enumerate history.

## Default controls

| Key | Action |
| --- | --- |
| `Ctrl+L` | Edit the global URL bar. |
| `Ctrl+R` | Reload the focused page. |
| `Alt+Left` / `Alt+Right` | Navigate page history. |
| `Ctrl+Shift+V` | Open clipboard history. |
| `Ctrl+Shift+I` | Toggle Web Inspector. |
| `Ctrl+Shift+Enter` | Open another split. |
| `Ctrl+Shift+T` | Open another browser layer/tab. |
| `Ctrl+Shift+N` | Open another Kitty OS window. |
| `Ctrl+Q` | Close the focused pane. |

## Runtime and storage

The root `mux` launcher configures a release build under
`$XDG_CACHE_HOME/mux/wpe-kitty`, compiles only changed sources, ensures `muxd`
is running, and starts Kitty with [kitty/wpe.conf](kitty/wpe.conf). The checkout
does not accumulate build artifacts.

Profile data follows XDG data and cache directories. Authenticated Unix sockets
use `$XDG_RUNTIME_DIR`; when it is unavailable, Mux creates an owner-only
fallback below `/tmp`.

## Project status

The browser/control source is implemented. The remaining release gate is an
interactive Linux exercise against WPE WebKit 2.52 and Kitty, including frame
presentation, splits, clipboard traffic, menus, downloads, and process restart.
The CI workflow compiles every Meson target in an Arch Linux container but does
not claim an interactive Kitty graphics test.

The design rationale is in [docs/architecture.md](docs/architecture.md), and
the current WPE runtime path is documented in
[docs/running-wpe.md](docs/running-wpe.md).
