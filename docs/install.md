# Installing Mux v0.1

Mux v0.1 is a personal Linux preview. A usable installation needs Kitty 0.45 or
newer and a WPE WebKit build that exposes the WPEPlatform 2.52 API. WPE
WebKit's version number alone is not sufficient.

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
- WPE WebKit 2.52 or newer built with WPEPlatform enabled
- GStreamer plugins needed by the sites you use
- General-purpose fonts

Confirm the dependency boundary directly:

```sh
pkg-config --atleast-version=2.52 wpe-webkit-2.0
pkg-config --atleast-version=2.52 wpe-platform-2.0
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

The Linux workflow runs the doctor in an Arch container, configures the real
WPE project as a warning-free release build, compiles all Meson targets, and
runs every registered native test with GLib warnings made fatal. It does not
build the flake's custom WPE derivation, launch Kitty, exercise graphics
presentation, or prove compatibility with live websites.
