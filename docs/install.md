# Installing Mux v0.1

Mux v0.1 is a personal Linux preview. A usable installation needs Kitty and a
WPE WebKit build that exposes the WPEPlatform 2.52 API. WPE WebKit's version
number alone is not sufficient.

## Fast path on Arch Linux

From the repository root:

```sh
./setup
```

`setup` runs an idempotent `pacman -Syu --needed` reconciliation, checks the
environment, creates an incremental release build outside the checkout, and
launches the real WPE implementation in Kitty. It is safe to rerun after an
interrupted build or package update.

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
- Kitty with the `kitten` helper
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

If `doctor` passes, the one-command build and launch path is still:

```sh
./setup https://example.com
```

## Nix flake

The flake supports `x86_64-linux` and `aarch64-linux`. Its default app builds
the WPE Meson project, runs the registered native tests during the Nix build,
packages the actual runtime processes, and launches them in Kitty:

```sh
nix run . -- https://example.com
```

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
launch Kitty, exercise graphics presentation, or prove compatibility with live
websites.
