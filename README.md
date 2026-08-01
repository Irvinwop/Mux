# Mux

Mux v0.1 is a personal preview of a Kitty-native browser multiplexer for
Linux. Kitty supplies tabs, splits, windows, layouts, and graphics
presentation. WPE WebKit supplies the browser engine. Mux connects them with a
small control daemon, one shared engine per profile, and a thin process in each
browser pane.

This is not a replacement browser engine and it does not stream screenshots.
The WPE path copies damaged raw RGBA regions through short-lived POSIX shared
memory and updates an existing Kitty graphics image.

## Status

The real WPE implementation builds as a collection of native processes and has
unit tests for its clipboard, protocol, local transport, extension routing,
and URI-input components. The interactive browser paths have not all been
validated end to end on a Linux desktop. Treat v0.1 as personal experimental
software, not as a secure daily-driver browser.

## Requirements

- Linux on x86-64 or ARM64
- A Wayland or X11 desktop session
- Kitty, including `kitten`
- A C17 compiler, Meson, Ninja, and pkg-config
- GLib and GIO 2.74 or newer
- WPE WebKit 2.52 or newer, built with WPEPlatform enabled
- Writable POSIX shared memory at `/dev/shm`

The decisive dependency check is that pkg-config can find both
`wpe-webkit-2.0 >= 2.52` and `wpe-platform-2.0 >= 2.52`.

## Start in one command

On Arch Linux or Arch Linux ARM, `setup` reconciles the required packages,
runs the environment doctor, builds the WPE implementation incrementally, and
opens its dedicated Kitty window:

```sh
./setup
./setup https://example.com
```

On other distributions, install the requirements yourself and use the same
command. The script will not guess package names for an unmaintained distro
recipe.

With Nix flakes, the default app builds and runs the same WPE implementation,
not the old synthetic Kitty transport:

```sh
nix run . -- https://example.com
```

After source dependencies are installed, normal starts use the incremental
launcher:

```sh
./mux
./mux https://example.com
./mux ctl status
```

Run `./doctor` for a read-only, actionable dependency report. See
[Installation](docs/install.md) for package and Nix details.

## Current implementation

The v0.1 code currently wires these paths:

- A central `muxd` control process with logical layers and pane routing.
- A profile-specific WPE engine that can own multiple page views.
- Kitty pane processes for frame presentation and input forwarding.
- A focus-following global URL/status bar.
- URL navigation and search-query resolution.
- Keyboard, pointer, focus, resize, and high-DPI input transport.
- Clipboard synchronization and bounded, profile-scoped clipboard history.
- Trusted UI requests for dialogs, permissions, authentication, file choices,
  downloads, menus, popups, notifications, and renderer crashes.

These are implemented code paths, not a claim that every site or interaction
has passed interactive testing. Clipboard snapshots can retain related text,
image, and binary MIME variants, but Mux can only record clipboard traffic it
actually observes.

## Intended controls

| Key | Action |
| --- | --- |
| `Ctrl+L` | Edit the global URL bar. |
| `Ctrl+R` | Reload the focused page. |
| `Alt+Left` / `Alt+Right` | Navigate page history. |
| `Ctrl+Shift+V` | Open profile clipboard history. |
| `Ctrl+Shift+P` | Open commands, bookmarks, history, and recently closed pages. |
| `Ctrl+D` | Add or remove a bookmark for the current page. |
| `Ctrl+Shift+Enter` | Open another browser split. |
| `Ctrl+Shift+T` | Open another browser layer/tab. |
| `Ctrl+Q` | Ask the page to close; press again to force it. |

These bindings still need a full conflict and behavior pass against stock and
user-customized Kitty configurations.

## Process model

| Process | Responsibility |
| --- | --- |
| `muxd` | Control socket, logical layers, pane routing, and clipboard broker. |
| `mux-engine` | WPE display, WebKit session, network state, and page ownership for one profile. |
| `mux-pane` | Input forwarding, raw-frame presentation, clipboard link, and trusted overlays for one Kitty pane. |
| `mux-bar` | URL and status bar that follows the focused pane. |
| `mux-layer` | Starts a logical collection of browser panes. |
| `muxctl` | Scriptable local control client. |
| Kitty | Tabs, splits, OS windows, layouts, and graphics presentation. |

WebKit continues to use its own sandboxed web, network, and GPU processes.
Browser content does not execute inside `muxd`.

## Known v0.1 limitations

- Linux and Kitty are mandatory; macOS and other terminal emulators are not
  supported.
- Automatic dependency installation is maintained only for Arch-family
  systems.
- Workspace URL and layer metadata is persisted for recovery, but offline panes
  and Kitty layouts are not relaunched automatically.
- In-flight trusted UI requests are cancelled when an engine crashes.
- One dedicated Kitty OS window is supported; layers are tabs in that window.
- A physical layer move requires a live destination pane in the same Kitty
  instance; otherwise it fails without changing logical state.
- The first close request uses WebKit's before-unload flow. A second request or
  the two-minute safety timeout forces teardown.
- Browser shortcuts and trusted overlays still require interactive coverage.
- Site compatibility, media codecs, downloads, uploads, permissions, popups,
  notifications, clipboard bridging, and renderer recovery are not yet
  release-qualified end to end.

## Runtime storage

The source launcher keeps its release build under
`$XDG_CACHE_HOME/mux/wpe-kitty` and recompiles only changed sources. Profile
data follows XDG data and cache directories. Authenticated local sockets use
`$XDG_RUNTIME_DIR`; when it is absent, Mux uses an owner-only fallback below
`/tmp`.

See [Running the WPE implementation](docs/running-wpe.md) for the runtime
boundary and [Architecture](docs/architecture.md) for design rationale.
