# Running Mux with WPE WebKit

The browser frame path is:

    WPE WebKit -> WPEBuffer -> damage copy -> POSIX shared memory -> Kitty image edit

There are no PNG encodes and no polling screenshot loop. WebKit submits frames
when page content changes. Mux applies a configurable presentation ceiling and
otherwise sleeps without a redraw timer.

## Requirements

- Linux
- Kitty
- Meson and Ninja
- GLib 2.74 or newer
- WPE WebKit 2.52 or newer
- WPEPlatform enabled in WPE WebKit with `-DENABLE_WPE_PLATFORM=ON`
- pkg-config modules wpe-webkit-2.0 and wpe-platform-2.0

WPEPlatform is optional in the 2.52 release line. A distro package can contain
WPE WebKit while still omitting wpe-platform-2.0; the launcher checks for that
case before configuring the build.

## Start

From the repository root:

    ./mux https://example.com

The first invocation configures an incremental release build under
`XDG_CACHE_HOME`, ensures the central `muxd` service and profile `mux-engine`
are available, then launches a dedicated Kitty window. Later invocations only
compile changed files.

Use `./setup` instead when dependencies have not yet been installed.

## Controls

- Ctrl+L edits the URL bar. Plain words with spaces become a DuckDuckGo query.
- Ctrl+R reloads.
- Alt+Left and Alt+Right navigate history.
- Ctrl+Shift+I toggles Web Inspector.
- Ctrl+Shift+V opens profile clipboard history.
- Ctrl+Q exits the pane.
- Ctrl+Shift+Enter opens another browser split.
- Ctrl+Shift+T opens another browser tab.
- Ctrl+Shift+N opens another Kitty OS window.

Mouse input uses Kitty SGR pixel coordinates, so WPE receives pane-relative
pixel positions rather than guessed terminal-cell positions.

## Frame behavior

`MUX_MAX_FPS` controls the presentation ceiling and defaults to 60. Values up
to 240 are accepted. This is a ceiling, not a redraw request: static pages
remain at zero frames per second.

The adapter keeps exactly one WPE buffer pending until its presentation
deadline. It copies only WebKit's damaged area, edits the existing Kitty image
in place, then reports the WPE buffer rendered and released. Kitty receives raw
RGBA from a short-lived POSIX shared-memory object that is unlinked after use.

## Runtime architecture

`muxd` owns the control plane, layers, pane routing, and clipboard broker. A
profile-specific `mux-engine` owns the WPE display, WebKit network session, and
all page views for that profile. Each Kitty window runs a thin `mux-pane` that
forwards input and presents the selected view's frames.

The same engine-to-pane extension channel carries trusted browser UI requests:
script dialogs, permissions, authentication, file selection, downloads,
option menus, context menus, popup attachment, and crash recovery. Clipboard
traffic uses an atomic binary protocol so all MIME variants remain related.

## Current release gate

The source implementation is complete enough for an end-to-end Linux exercise.
Before treating it as released, run it interactively with WPE WebKit 2.52 and
Kitty and cover navigation, animated damage, splits, layers, clipboard history,
downloads, prompts, popups, and renderer termination. The GitHub workflow proves
compilation only; it has no interactive Kitty terminal.
