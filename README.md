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
and URI-input components. A bounded headless gate also runs real Weston,
Kitty, WPE, muxd, the shared engine, a pane, and the global bar end to end. The
interactive browser paths have not all been validated on a Linux desktop.
Treat v0.1 as personal experimental software, not as a secure daily-driver
browser.

## Requirements

- Linux on x86-64, or ARM64 with WPE supplied outside Arch Linux ARM
- A Wayland or X11 desktop session
- Kitty 0.45 or newer, including `kitten`
- A C17 compiler, Meson, Ninja, and pkg-config
- GLib and GIO 2.74 or newer
- WPE WebKit and WPEPlatform 2.52.5 or newer
- Writable POSIX shared memory at `/dev/shm`

The decisive dependency check is that pkg-config can find both
`wpe-webkit-2.0 >= 2.52.5` and `wpe-platform-2.0 >= 2.52.5`. Versions before
2.52.5 are affected by
[WSA-2026-0004](https://www.webkitgtk.org/security/WSA-2026-0004.html); see the
official [WPE WebKit 2.52.5 release](https://wpewebkit.org/release/wpewebkit-2.52.5.html).

## Release-qualified user install

The release-qualified v0.1 path is an ordinary-user installation on x86_64
Arch Linux:

```sh
./setup
./setup https://example.com
```

`setup` elevates only the interactive `pacman -Syu --needed` transaction. It
then returns to the invoking user, runs the environment doctor, builds in the
user's XDG cache, and installs below `~/.local` by default. It rejects running
the script itself as root.

The installed layout is:

- `~/.local/bin/mux`: relocatable launcher
- `~/.local/libexec/mux`: `muxd`, `muxctl`, `mux-bar`, `mux-layer`,
  `mux-engine`, and `mux-pane`
- `~/.local/share/mux/kitty`: `kitty.conf` and `wpe.conf`

The launcher resolves its prefix from its own path. It does not refer to the
checkout or build cache, so the checkout can be renamed or removed after
installation. Re-running `./setup` safely updates the same paths. Use
`./setup --no-launch` for an install-only run, `--prefix /absolute/path` for a
different movable prefix, and `mux --uninstall` for a bounded removal that
leaves profiles, caches, build trees, and system packages untouched.

The package step is a full system upgrade and may update the kernel or
unrelated packages. Review the transaction before accepting it.

Official Arch Linux ARM repositories do not provide `wpewebkit`. Therefore
Arch Linux ARM is not part of the automatic or release-qualified Arch path.
The flake's custom WPE, runtime, native tests, and default app have been fully
realized on `aarch64-linux`; its graphical launch remains unqualified.

## Checkout development mode

The checkout launcher remains explicit and incremental:

```sh
./mux
./mux https://example.com
./mux ctl status
./setup --development https://example.com
```

It builds below
`$XDG_CACHE_HOME/mux/checkouts/<checkout-sha256>/wpe-kitty` and deliberately
uses the Kitty configs from the checkout. This is the development path, not
the installed-user path.

On other Linux distributions, install the requirements yourself and use
`./setup --skip-packages --no-launch` for the conventional user install, or
`./mux` for development. Those source environments are not release-qualified
v0.1 package recipes.

## Nix path

The flake is pinned and evaluation-checked for `x86_64-linux` and
`aarch64-linux`. Stock Nix requires the `nix-command` and `flakes`
experimental features; enable them for the invocation with:

```sh
nix --extra-experimental-features 'nix-command flakes' run . -- https://example.com
```

The `aarch64-linux` path has completed a serialized custom WPE 2.52.5 build,
the 146-step Mux build, all 16 native suites, the runtime output, and the
default app output. `x86_64-linux` remains evaluation-checked rather than fully
realized. A graphical launch through the Nix wrapper is still unqualified, so
the automatic x86_64 Arch install remains the v0.1 release path.

Run `./doctor` for a read-only dependency report. See
[Installation](docs/install.md) for prefix, staging, uninstall, Arch Linux ARM,
and Nix details.

## Headless full-stack runtime gate

After the source build has produced the six executables, run the gate as an
ordinary user on Linux:

```sh
./mux ctl status
./runtime-smoke
```

It derives the same checkout-specific cache build directory as `./mux`. To use
another build explicitly:

```sh
MUX_BIN_DIR="$PWD/build/runtime-smoke" ./runtime-smoke
```

The gate creates private XDG and profile directories, starts a D-Bus session
when needed, and uses Weston headless with its Pixman renderer. It serves only
loopback HTTP fixtures because Mux intentionally rejects `data:` navigation.
Through a real Kitty process it proves JavaScript title execution, the
Kitty-to-pane process topology, muxd and engine connectivity, muxctl focus,
physical layer switching, global URL entry through encrypted `Super+L` plus
Kitty `send-text`, targeted navigation, and `Super+Q` closure. Test-only,
hash-based frame telemetry requires a positive Kitty graphics response for
delivered page frames; production mode writes no such telemetry and retains
the narrower production ACL.

The acknowledgement proves that Mux sent an immutable frame and Kitty accepted
its load/place request. It does not prove physical monitor scanout, a real GPU,
live-site compatibility, media behavior, or desktop shortcut compatibility.
Weston, Python 3, D-Bus, and `pgrep` from procps are additional smoke-only
dependencies. Root execution is rejected except for the explicit privileged
container path used by CI.

Run `./resource-smoke` for the companion bounded-resource gate. It records
active and idle CPU, RSS/PSS, process count, named shared-memory use, hidden-pane
frame suspension, and cleanup latency. See
[Resource qualification](docs/resource-qualification.md) for the budgets and
artifact format.

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
has passed interactive testing.

## Intended controls

`Super` is canonical for Mux-owned shortcuts. Kitty calls the modifier
`super`; in a macOS-hosted Kitty session this is the Command key, not Ctrl.
The `Ctrl` mappings are explicit Linux compatibility aliases. This naming does
not make the WPE runtime portable to macOS.

| Key | Action |
| --- | --- |
| `Super+L` (`Ctrl+L` Linux alias) | Edit the global URL bar. |
| `Super+R` (`Ctrl+R` Linux alias) | Reload the focused page. |
| `Alt+Left` / `Alt+Right` | Navigate page history. |
| `Super+Shift+V` (`Ctrl+Shift+V` Linux alias) | Open profile clipboard history. |
| `Super+Shift+P` (`Ctrl+Shift+P` Linux alias) | Open commands, bookmarks, history, and recently closed pages. |
| `Super+D` (`Ctrl+D` Linux alias) | Add or remove a bookmark for the current page. |
| `Super+Shift+Enter` (`Ctrl+Shift+Enter` Linux alias) | Open another browser split. |
| `Super+Shift+T` (`Ctrl+Shift+T` Linux alias) | Open another browser layer/tab. |
| `Super+Q` (`Ctrl+Q` Linux alias) | Ask the page to close; press again to force it. |

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
  supported runtime targets.
- Automatic dependency installation is maintained only for x86_64 Arch Linux.
- Arch Linux ARM lacks an official `wpewebkit` package.
- Workspace URL and layer metadata is persisted for recovery, but offline panes
  and Kitty layouts are not relaunched automatically.
- In-flight trusted UI requests are cancelled when an engine crashes.
- One dedicated Kitty OS window is supported; layers are tabs in that window.
- Browser shortcuts and trusted overlays still require interactive coverage.
- Site compatibility, media codecs, downloads, uploads, permissions, popups,
  notifications, clipboard bridging, and renderer recovery are not yet
  release-qualified end to end.

## Runtime storage

Installed program files live entirely under the selected prefix and can move
with it. Profile state follows XDG data and cache directories. Authenticated
local sockets use `$XDG_RUNTIME_DIR`; when it is absent, Mux uses an owner-only
fallback below `/tmp`. Checkout development builds remain in the
checkout-keyed XDG cache path described above.

See [Running the WPE implementation](docs/running-wpe.md) for the runtime
boundary and [Architecture](docs/architecture.md) for design rationale.
