# Running Mux with WPE WebKit

Mux's real browser path is:

```text
WPE WebKit -> WPEBuffer damage -> POSIX shared memory -> Kitty image update
```

The implementation does not encode a PNG for each frame and does not poll the
page for screenshots. It is designed to copy damaged raw RGBA regions and
update one Kitty graphics image in place.

## Runtime requirements

- Linux
- Kitty in a Wayland or X11 session
- GLib and GIO 2.74 or newer
- WPE WebKit 2.52.5 or newer
- WPEPlatform enabled in the WPE build
- pkg-config modules `wpe-webkit-2.0` and `wpe-platform-2.0`, both version
  2.52.5 or newer
- Writable `/dev/shm`

Version 2.52.5 is the current stable security floor: the
[WPE release index](https://wpewebkit.org/release/) lists it as stable, and
[WSA-2026-0004](https://www.webkitgtk.org/security/WSA-2026-0004.html) affects
WPE WebKit versions before 2.52.5.

Run `./doctor` before investigating runtime failures. It distinguishes missing
build requirements from the expected no-display warning in headless shells.

## Start the browser

The source-checkout path builds incrementally and launches Kitty:

```sh
./setup https://example.com
```

After setup:

```sh
./mux https://example.com
```

The Nix path builds and packages the same WPE processes:

```sh
nix run . -- https://example.com
```

Both launch paths ensure `muxd` is running, put the WPE runtime processes on
`PATH`, set the initial logical layer and global-bar environment, and start a
dedicated Kitty instance with Mux's WPE configuration.

## Runtime architecture

`muxd` owns the local control plane, logical layers, pane routing, and clipboard
broker. One `mux-engine` owns a WPE display, WebKit session, and page views for
each profile. Each Kitty browser pane runs `mux-pane` to forward input and
present frames. `mux-bar` follows focus, and `mux-layer` creates the initial
pane collection.

WebKit still launches its own sandboxed web, network, and GPU processes. Mux
centralizes browser coordination; it does not merge untrusted web content into
the daemon.

## Frame pacing

`MUX_MAX_FPS` sets the intended presentation ceiling and defaults to 60. Values
up to 240 are accepted. It is a ceiling rather than a redraw request, so static
pages should not require a continuous stream of identical frames.

The adapter retains one submitted WPE buffer until its presentation deadline,
copies the damaged region into shared memory, updates the Kitty image, and then
reports the WPE buffer rendered and released. The shared-memory object is
short-lived and unlinked after use.

This design is implemented but still needs interactive measurement under
scrolling, video, animation, resize, high-DPI output, and background layers.

## Wired input and browser UI

The current code carries keyboard, pointer, focus, resize, and input-method
events between Kitty panes and WPE views. It also has trusted request paths for
dialogs, permission decisions, authentication, file selection, downloads,
menus, popups, notifications, clipboard operations, and renderer crashes.

The browser bindings use `Super` as the canonical Mux modifier. The listed
`Ctrl` forms are explicit Linux compatibility aliases:

- `Super+L` (`Ctrl+L`): edit the global URL bar
- `Super+R` (`Ctrl+R`): reload
- `Alt+Left` and `Alt+Right`: navigate page history
- `Super+Shift+V` (`Ctrl+Shift+V`): open profile clipboard history
- `Super+Shift+P` (`Ctrl+Shift+P`): open commands, bookmarks, history, and recently closed pages
- `Super+D` (`Ctrl+D`): toggle a bookmark for the current page
- `Super+Q` (`Ctrl+Q`): close the pane
- `Super+Shift+Enter` (`Ctrl+Shift+Enter`): open a split
- `Super+Shift+T` (`Ctrl+Shift+T`): open a layer in another Kitty tab

These paths are not all release-qualified. User Kitty mappings may override or
conflict with Mux bindings.

## Clipboard boundary

The clipboard broker can keep related MIME variants in one bounded,
memory-only snapshot and isolate history by profile. Text, HTML, images, and
arbitrary binary variants are representable. Pages can request only the current
clipboard through WebKit's permission path; they cannot enumerate history.

The trusted right-click menu can download an image, media resource, link target,
or current page directly to the file clipboard. Mux retains the download in an
owner-only temporary directory for the current memory-only session and
publishes desktop file-URI variants plus MIME bytes for files up to 16 MiB.
Repeated identical clipboard snapshots are deduplicated even if the user
presses the copy shortcut multiple times.

History records traffic observed by Mux, not every external desktop clipboard
change. Full Kitty-to-WPE behavior, large payload handling, and private-profile
isolation still require interactive validation.

## Known runtime limitations

- Workspace URL and layer metadata survives daemon restart, but offline panes
  and Kitty layouts are not relaunched automatically.
- In-flight trusted UI requests are cancelled when an engine is replaced.
- One dedicated Kitty OS window is supported. A physical layer move requires a
  live destination pane in the same Kitty instance.
- The first close request runs WebKit's before-unload flow. A second request
  while it is pending forces teardown; the two-minute timeout cancels the close.
- Browser UI paths such as uploads, downloads, popups, notifications, and
  permission prompts need live-site testing.
- File paste and file drag are separate platform operations. Download to
  clipboard does not yet emulate a drop for sites that reject pasted files.
- Shortcut routing has not been validated against a broad set of Kitty user
  configurations.
- Media support depends on the installed GStreamer plugins and site codec
  requirements.

## What CI proves

The Linux workflow configures the real WPE Meson project as a release build,
turns compiler warnings into errors, compiles every target, and runs the
registered native tests. Those tests cover data models and local protocols;
they do not open a graphical Kitty window or load live websites.

Before calling v0.1 daily-driver ready, exercise navigation, frame pacing,
splits, layers, clipboard history, uploads, downloads, prompts, popups,
permissions, media, and renderer termination on a real Linux desktop.
