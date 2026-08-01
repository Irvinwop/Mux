# Installing Mux v0.1

Mux v0.1 is a personal Linux preview. A usable installation needs Kitty 0.45 or
newer and WPE WebKit built with WPEPlatform, with both pkg-config modules at
2.52.5 or newer. Versions before 2.52.5 are affected by the official
[WSA-2026-0004 advisory](https://www.webkitgtk.org/security/WSA-2026-0004.html).
WPE WebKit 2.52.5 is the corresponding
[stable release](https://wpewebkit.org/release/wpewebkit-2.52.5.html).

## Release-qualified path: Arch family

From the repository root:

```sh
./setup
```

`setup` runs `pacman -Syu --needed` interactively, checks the environment,
creates an incremental release build outside the checkout, and launches the
real WPE implementation in Kitty. Review the package transaction before
accepting it: `pacman -Syu` is a full system upgrade and may update the kernel
or packages unrelated to Mux. The build portion is safe to resume after an
interruption.

Open an initial page or stop after dependency checks with:

```sh
./setup https://example.com
./setup --deps-only
```

The automatic Arch package set includes the compiler toolchain, GLib, Kitty,
Meson, Ninja, WPE WebKit, common GStreamer codecs, and Noto fonts.

## Other Linux distributions

There is no maintained automatic apt or dnf recipe. Install these requirements
using your distribution, a custom prefix, or a containerized development
environment:

- A C17 compiler
- Kitty 0.45 or newer with the `kitten` helper
- Meson, Ninja, and pkg-config
- GLib and GIO 2.74 or newer, including development metadata
- WPE WebKit and WPEPlatform 2.52.5 or newer
- GStreamer plugins needed by the sites you use
- General-purpose fonts

Confirm the dependency boundary directly:

```sh
pkg-config --atleast-version=2.52.5 wpe-webkit-2.0
pkg-config --atleast-version=2.52.5 wpe-platform-2.0
./doctor
```

If `doctor` passes, build and launch with:

```sh
./mux https://example.com
```

Non-Arch source installations are useful for development, but they are not a
release-qualified v0.1 path.

## Nix flake

The lock file pins the flake inputs, and the flake has passed evaluation checks
for `x86_64-linux` and `aarch64-linux`. The default app is intended to build the
custom WPE derivation and Mux runtime, then launch them in Kitty:

```sh
nix run . -- https://example.com
```

This path is not release-qualified. A complete custom WPE build has not
finished locally or in CI. Its evaluated closure requires roughly 1.5 GiB of
downloads and 6.1 GiB unpacked, so successful evaluation is not evidence of a
successful build.

Build only the runtime package or enter the development shell with:

```sh
nix build .#runtime
nix develop
```

The Nix app includes common GStreamer plugin sets in the runtime environment.
It does not make Mux portable to macOS: the implementation and flake outputs
remain Linux-only.

## Normal source launches

After dependencies are ready, use:

```sh
./mux
./mux https://example.com
./mux ctl status
```

The launcher configures a release build under the XDG cache directory, compiles
only changed files, ensures the control daemon is running, and opens a dedicated
Kitty instance. It does not install files globally or write build products into
the checkout.

## Full-stack runtime smoke

The release gate needs Weston, Mesa's software stack, Python 3, D-Bus, and
procps in addition to the normal Mux dependencies. On Arch Linux:

```sh
sudo pacman -Syu --needed weston mesa dbus python procps-ng
```

Build through the normal launcher, then run the smoke as your ordinary desktop
user:

```sh
./mux ctl status
./runtime-smoke
```

`runtime-smoke` computes the exact
`$XDG_CACHE_HOME/mux/checkouts/<checkout-sha256>/wpe-kitty` directory used by
`./mux`. A separately built tree can be selected explicitly:

```sh
MUX_BIN_DIR="$PWD/build/runtime-smoke" ./runtime-smoke
```

The command owns a 120-second default timeout and bounded cleanup. It creates
0700 XDG, profile, cache, and runtime directories; uses an ephemeral profile;
starts a D-Bus session when the caller has none; and runs Weston headless with
Pixman. Loopback HTTP pages are used instead of `data:` URIs because Mux rejects
that scheme.

The gate starts foreground muxd and mux-engine processes followed by a real
Kitty -> mux-layer -> mux-pane and mux-bar stack. It proves JavaScript page load
through title metadata in `muxctl list`, process presence, physical focus and
layer changes, targeted navigation to a second fixture, and graceful `Super+Q`
for both panes. Kitty commands use encrypted blank-password remote control with
the `KITTY_PUBLIC_KEY` read from the pane environment. Only the temporary Kitty
config copy gains `send-key` and `get-text`, which are used for the close path
and failure diagnostics.

This is not a pixel oracle. Mux does not expose Kitty's graphics `FRAME_ACK`, so
the gate cannot prove that Kitty displayed a frame. It also does not cover a
real GPU, live sites, media, desktop keyboard conflicts, endurance, or resource
budgets.

Do not use `sudo ./runtime-smoke`. Normal Linux use must be non-root. The GitHub
job is the sole root path: its disposable Arch container is privileged and uses
the host user namespace so WebKit's nested bubblewrap sandbox remains enabled,
and it opts in with both `CI=true` and `MUX_SMOKE_ALLOW_ROOT_CI=1`.

## Diagnostics

Run:

```sh
./doctor
```

The doctor is read-only. Hard failures include the exact missing command or
pkg-config module and a repair hint. A missing Wayland/X11 display is only a
warning so the same check can run in headless CI; starting Kitty still requires
a graphical session. In containers, provide a writable `/dev/shm` with enough
space for raw frames.

## Packaging boundary

Meson installs the six runtime executables. The Nix derivation still copies
them explicitly so its app wrapper can provide Kitty configuration and media
plugin paths without a global install. A conventional distribution package
must additionally install an appropriate launcher and both Kitty configuration
files.

## CI boundary

The Linux workflow retains its Arch warning-free build and fatal-GLib native
test job. A separate `Runtime smoke` job builds again in a privileged Arch
container with host user namespaces and 2 GiB of shared memory, then runs
`./runtime-smoke`. The outer privilege is required for nested WebKit bubblewrap;
the gate refuses the environment variable that disables WebKit sandboxing.

CI now launches real Weston, Kitty, WPE, and every Mux runtime process against
loopback fixtures. It still does not build the flake's custom WPE derivation,
observe a Kitty pixel acknowledgement, use a physical display/GPU, or prove
compatibility with live websites.
