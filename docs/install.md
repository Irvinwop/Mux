# Installation

Mux currently targets Linux. It uses Linux peer credentials, POSIX shared
memory, a custom WPEPlatform display, and Kitty's local graphics transport.

## Quick start

From the repository root:

    ./setup

On Arch Linux or Arch Linux ARM, this installs dependencies, checks their
versions, builds Mux incrementally through the root launcher, and opens Kitty.
Pass an initial URL directly:

    ./setup https://example.com

After the first setup, normal launches are:

    ./mux
    ./mux https://example.com

Build output lives under the XDG cache directory rather than the checkout.
Profiles use XDG data and cache directories. Runtime sockets and locks use
XDG_RUNTIME_DIR, falling back to an owner-only directory under /tmp.

## Supported package surfaces

### Arch Linux and Arch Linux ARM

Arch is the primary development target. The Arch and Arch Linux ARM wpewebkit
packages include WPE WebKit, WPEPlatform, the headless display, headers,
process executables, and all three required pkg-config files in one current
package:

    sudo pacman -Syu --needed base-devel kitty meson ninja pkgconf wpewebkit

Package metadata:
https://archlinux.org/packages/extra/x86_64/wpewebkit/

### Debian and Fedora

Current Debian sid and Fedora Rawhide packages can provide WPE WebKit 2.52
without providing the separate wpe-platform-2.0 API. The WebKit version alone
is therefore not enough to build Mux. The setup script fails before invoking
apt or dnf rather than installing a large dependency set that cannot compile
the project.

Custom WPE builds remain usable. Install Kitty, Meson, Ninja, pkg-config, a C17
compiler, GLib/GIO development files, and WPE WebKit built with WPEPlatform
2.52 or newer, then rerun:

    ./setup --deps-only

## Diagnostics

Run:

    ./doctor

It checks the compiler, Kitty and kitten, Meson, Ninja, pkg-config, GLib/GIO,
WPE WebKit, WPEPlatform, and shared-memory availability.
It does not build or modify the checkout.

## Optional media support

On Arch, the setup script installs common GStreamer good, bad, and libav plugin
sets. WPE uses GStreamer for web media, so codec availability affects streaming
sites even when the browser itself builds correctly.

## Continuous integration

The Linux workflow uses an Arch container and compiles every Meson target
against the current distribution WPE package. It intentionally does not claim a
graphics runtime test: GitHub's container has no interactive Kitty terminal.
