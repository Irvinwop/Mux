# Installing Mux v0.1

Mux v0.1 is a personal Linux preview. A usable installation needs Kitty 0.45
or newer and WPE WebKit built with WPEPlatform, with both pkg-config modules at
2.52.5 or newer. Versions before 2.52.5 are affected by the official
[WSA-2026-0004 advisory](https://www.webkitgtk.org/security/WSA-2026-0004.html).
WPE WebKit 2.52.5 is the corresponding
[stable release](https://wpewebkit.org/release/wpewebkit-2.52.5.html).

## Release-qualified path: x86_64 Arch Linux

Run setup from the repository root as the ordinary user who will own the
installation:

```sh
./setup
```

The script performs these steps:

1. It elevates only `pacman -Syu --needed` using `sudo`, `doas`, or `su`.
2. It returns to the invoking user and runs `doctor`.
3. It creates a release build below the user's XDG cache.
4. It installs a relocatable prefix below `~/.local` by default.
5. It launches the newly installed `mux` unless `--no-launch` was requested.

Do not run `sudo ./setup`. Setup rejects root so compilation, Meson
installation, profile creation, and Mux runtime processes cannot accidentally
become root-owned. Review the package transaction before accepting it:
`pacman -Syu` is a full system upgrade and may update the kernel or unrelated
packages.

Useful setup forms are:

```sh
./setup https://example.com
./setup --no-launch
./setup --deps-only
./setup --prefix "$HOME/.local/opt/mux" --no-launch
./setup --development https://example.com
```

`--deps-only` stops after dependency reconciliation and diagnostics.
`--development` launches directly from the checkout and does not install.
`--skip-packages` is available when dependencies were supplied manually.

## Installed layout

For a prefix `PREFIX`, Meson installs:

| Path | Contents |
| --- | --- |
| `PREFIX/bin/mux` | Relocatable launcher |
| `PREFIX/libexec/mux/muxd` | Control daemon |
| `PREFIX/libexec/mux/muxctl` | Control client |
| `PREFIX/libexec/mux/mux-bar` | Global bar |
| `PREFIX/libexec/mux/mux-layer` | Layer launcher |
| `PREFIX/libexec/mux/mux-engine` | Shared WPE engine |
| `PREFIX/libexec/mux/mux-pane` | Kitty pane process |
| `PREFIX/share/mux/kitty/kitty.conf` | Shared Kitty appearance |
| `PREFIX/share/mux/kitty/wpe.conf` | WPE pane bindings and control ACL |

The launcher resolves symlinks and derives `PREFIX` from its own physical
location. It prepends `PREFIX/libexec/mux` to `PATH` because the runtime
processes intentionally spawn each other by name. No installed file contains a
checkout or build-cache path.

With the default prefix, ensure `$HOME/.local/bin` is on `PATH`. A dedicated
prefix can itself be moved:

```sh
./setup --prefix "$HOME/.local/opt/mux" --no-launch
mv "$HOME/.local/opt/mux" "$HOME/Applications/mux"
"$HOME/Applications/mux/bin/mux" --print-install-paths
```

Moving or deleting the source checkout does not affect either default or
custom-prefix installations.

## Reinstall and uninstall

Re-running setup with the same prefix is idempotent: Meson rebuilds changed
sources and replaces the same nine installed files. Profiles and runtime state
are not part of the program prefix.

Remove only Mux program files with:

```sh
mux --uninstall
```

For a custom prefix, invoke that prefix's launcher:

```sh
"$HOME/Applications/mux/bin/mux" --uninstall
```

Uninstall removes the launcher, six runtime programs, and two Kitty configs.
It removes only now-empty Mux-owned subdirectories. It does not remove XDG
profile data, caches, source builds, or packages installed by pacman.
`./setup --uninstall --prefix /absolute/path` is an equivalent convenience
while the checkout remains available.

## Arch Linux ARM

Arch Linux ARM is not included in the automatic Arch recipe. Its official
repositories do not provide `wpewebkit`, so `pacman` cannot satisfy Mux's
required `wpe-webkit-2.0` and `wpe-platform-2.0` modules.

If those modules are supplied from a separately maintained source, an ARM user
can request only environment checking and installation with:

```sh
./setup --skip-packages --no-launch
```

That is an unqualified source path. The flake evaluates for `aarch64-linux` and
can become the preferred ARM path after its full custom WPE build and runtime
are qualified; evaluation alone is not that qualification.

## Other Linux source installations

There is no maintained automatic apt or dnf recipe. Install these requirements
using the distribution, a custom prefix, or a containerized development
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

Then choose the conventional install or checkout development path:

```sh
./setup --skip-packages --no-launch
"$HOME/.local/bin/mux" https://example.com

./mux https://example.com
./mux ctl status
```

`./mux` is intentionally checkout-bound: it uses source configs and stores a
build under
`$XDG_CACHE_HOME/mux/checkouts/<checkout-sha256>/wpe-kitty`. The installed
launcher is intentionally checkout-independent.

## Package staging with DESTDIR

`DESTDIR` is honored without being compiled into the launcher and implies
`--no-launch`. For example:

```sh
stage="$(mktemp -d)"
DESTDIR="$stage" ./setup --skip-packages --prefix=/usr --no-launch
find "$stage/usr" -type f
```

After packaging moves `$stage/usr` to `/usr`, the launcher resolves `/usr`.
The focused portable test stages the same layout with stub runtime programs,
moves the prefix, resolves through a symlink, exercises `mux ctl status`
without a GUI, repeats the install, and checks bounded uninstall:

```sh
scripts/test-user-install.sh
```

## Nix flake

The lock file pins the flake inputs, and the flake has passed evaluation checks
for `x86_64-linux` and `aarch64-linux`. Stock Nix does not enable the modern
command and flake interfaces by default. Enable both experimental features for
each invocation:

```sh
nix --extra-experimental-features 'nix-command flakes' run . -- https://example.com
nix --extra-experimental-features 'nix-command flakes' build .#runtime
nix --extra-experimental-features 'nix-command flakes' develop
```

Alternatively, enable `nix-command flakes` in the user's `nix.conf` and use
the shorter commands.

The Nix path is not release-qualified. A complete custom WPE build has not
finished locally or in CI. Its evaluated closure requires roughly 1.5 GiB of
downloads and 6.1 GiB unpacked, so successful evaluation is not evidence of a
successful build. The Nix app includes common GStreamer plugin sets and remains
Linux-only.

## Keyboard terminology

`Super` is canonical in the installed Kitty config. Kitty names this modifier
`super`; in a macOS-hosted Kitty session it is the Command key, not Ctrl.
`Ctrl` bindings are explicit Linux compatibility aliases. This terminology
does not claim that the local WPE runtime runs on macOS.

## Full-stack runtime smoke

The release gate needs Weston, Mesa's software stack, Python 3, D-Bus, and
procps in addition to the normal Mux dependencies. On x86_64 Arch Linux:

```sh
sudo pacman -Syu --needed weston mesa dbus python procps-ng
```

Build through checkout development mode, then run the smoke as the ordinary
desktop user:

```sh
./mux ctl status
./runtime-smoke
```

`runtime-smoke` computes the exact checkout-specific build directory used by
`./mux`. A separately built tree can be selected explicitly:

```sh
MUX_BIN_DIR="$PWD/build/runtime-smoke" ./runtime-smoke
```

The command owns a 120-second default timeout and bounded cleanup. It creates
0700 XDG, profile, cache, and runtime directories; uses an ephemeral profile;
starts a D-Bus session when needed; and runs Weston headless with Pixman.
Loopback HTTP pages are used instead of `data:` URIs because Mux rejects that
scheme.

The gate starts foreground muxd and mux-engine processes followed by a real
Kitty -> mux-layer -> mux-pane and mux-bar stack. It proves JavaScript execution
through title metadata, process topology, physical focus and layer changes,
global URL entry through encrypted `Super+L` plus Kitty `send-text`, targeted
navigation, and `Super+Q` closure. Its opt-in smoke telemetry stores only
frame/view identifiers and content hashes, then requires positive Kitty
graphics acknowledgements for delivered page frames. Production launches do
not write this telemetry, and the production remote-control ACL is unchanged.

The acknowledgement proves that an immutable frame reached Kitty and its
load/place request was accepted. It does not prove physical monitor scanout or
qualify a real GPU, live sites, media, desktop keyboard conflicts, or endurance.

Do not use `sudo ./runtime-smoke`. Normal Linux use must be non-root. The CI
root path is limited to its explicit disposable privileged container opt-in.

## Resource qualification

Run the companion gate against the same checkout build with:

```sh
./resource-smoke
```

It exercises an active multi-pane phase, an idle phase, a hidden-pane phase,
and bounded shutdown. It enforces architecture-specific active CPU limits plus
idle CPU, RSS/PSS, process-count, named `/dev/shm`, hidden frame-suspension, and
cleanup-latency limits. JSON and TSV artifacts make each measurement auditable.
See [Resource qualification](resource-qualification.md) for the exact budgets,
environment overrides, and methodology.

## Diagnostics and packaging boundary

`./doctor` is read-only. Hard failures include the exact missing command or
pkg-config module and a repair hint. A missing Wayland/X11 display is only a
warning so checks can run headlessly. In containers, provide a writable
`/dev/shm` with enough space for raw frames.

Meson owns all conventional program installation: six native runtime
executables, the launcher, and both Kitty configs. The launcher assumes the
standard relative `bin`, `libexec`, and `share` layout selected by `setup`.
Distribution packages can select another absolute prefix while preserving
those relative directories.
