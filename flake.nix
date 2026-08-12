{
  description = "Mux v0.1: a personal Kitty-native WPE browser multiplexer";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          gstPlugins = with pkgs.gst_all_1; [
            gst-plugins-base
            gst-plugins-good
            gst-plugins-bad
            gst-plugins-ugly
            gst-libav
          ];
          gstPluginPath = pkgs.lib.makeSearchPath "lib/gstreamer-1.0" gstPlugins;
          baseFontConfig = pkgs.makeFontsConf {
            fontDirectories = [
              pkgs.dejavu_fonts
              pkgs.roboto
            ];
          };
          fontConfig = pkgs.writeText "mux-fonts.conf" ''
            <?xml version="1.0"?>
            <!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
            <fontconfig>
              <include>${baseFontConfig}</include>
              <alias binding="strong">
                <family>sans-serif</family>
                <prefer>
                  <family>Roboto</family>
                  <family>DejaVu Sans</family>
                </prefer>
              </alias>
              <alias binding="strong">
                <family>system-ui</family>
                <prefer><family>Roboto</family></prefer>
              </alias>
              <alias binding="strong">
                <family>Arial</family>
                <prefer><family>Roboto</family></prefer>
              </alias>
              <alias binding="strong">
                <family>Google Sans</family>
                <prefer><family>Roboto</family></prefer>
              </alias>
              <alias binding="strong">
                <family>serif</family>
                <prefer><family>DejaVu Serif</family></prefer>
              </alias>
              <alias binding="strong">
                <family>monospace</family>
                <prefer><family>DejaVu Sans Mono</family></prefer>
              </alias>
            </fontconfig>
          '';

          # nixpkgs 2.52.5 only packages the GTK port. Reuse its shared
          # WebKit compiler, patch, and native-tool policy, while replacing
          # every port-specific input, flag, source, and package contract.
          wpeWebKit =
            (pkgs.webkitgtk_6_0.override {
              enableGeoLocation = false;
              withLibsecret = false;
            }).overrideAttrs
              (_finalAttrs: previousAttrs: {
                pname = "wpewebkit";
                version = "2.52.5";
                name = "wpewebkit-2.52.5";

                src = pkgs.fetchurl {
                  url = "https://wpewebkit.org/releases/wpewebkit-2.52.5.tar.xz";
                  hash = "sha256-vPxskdt2WdzyT2/3mtJ6werhvGHcoNv+4VRwaSZ0Czs=";
                };

                # These are the common WebKit dependencies from the pinned
                # GTK derivation plus the WPEPlatform DRM/Wayland stack.
                # GTK, X11, libsecret, and GTK geolocation dependencies are
                # deliberately not inherited.
                buildInputs = with pkgs; [
                  at-spi2-core
                  cairo
                  enchant
                  expat
                  flite
                  fontconfig
                  freetype
                  glib
                  gnutls
                  gst_all_1.gstreamer
                  gst_all_1.gst-plugins-base
                  gst_all_1.gst-plugins-bad
                  harfbuzzFull
                  harfbuzzFull.dev
                  hyphen
                  icu
                  icu.dev
                  lcms2
                  libGL
                  libGLU
                  libavif
                  libbacktrace
                  libdrm
                  libepoxy
                  libgbm
                  libgcrypt
                  libgpg-error
                  libidn
                  libinput
                  libintl
                  libjpeg
                  libjxl
                  libmanette
                  libpng
                  libpthread-stubs
                  libseccomp
                  libsysprof-capture
                  libtasn1
                  libwebp
                  libwpe
                  libxkbcommon
                  libxml2
                  libxslt
                  nettle
                  openjpeg
                  p11-kit
                  pcre2
                  pcre2.dev
                  sqlite
                  systemdLibs
                  util-linux
                  util-linux.dev
                  wayland
                  wayland-protocols
                  woff2
                  zlib
                ];

                # These modules occur in the public .pc files. The FDO
                # backend is retained as the default implementation for
                # consumers of the enabled legacy libwpe API.
                propagatedBuildInputs = with pkgs; [
                  glib
                  libsoup_3
                  libwpe
                  pkgs."libwpe-fdo"
                ];

                cmakeFlags = [
                  "-DPORT=WPE"
                  "-DENABLE_INTROSPECTION=ON"
                  "-DENABLE_DOCUMENTATION=ON"
                  "-DENABLE_EXPERIMENTAL_FEATURES=OFF"
                  "-DENABLE_MINIBROWSER=OFF"
                  "-DENABLE_WPE_1_1_API=OFF"
                  "-DENABLE_WPE_LEGACY_API=ON"
                  "-DENABLE_WPE_PLATFORM=ON"
                  "-DENABLE_WPE_PLATFORM_DRM=ON"
                  "-DENABLE_WPE_PLATFORM_HEADLESS=ON"
                  "-DENABLE_WPE_PLATFORM_WAYLAND=ON"
                  "-DENABLE_WPE_QT_API=OFF"
                  "-DBWRAP_EXECUTABLE=${pkgs.lib.getExe pkgs.bubblewrap}"
                  "-DDBUS_PROXY_EXECUTABLE=${pkgs.lib.getExe pkgs.xdg-dbus-proxy}"
                ];

                meta = (builtins.removeAttrs previousAttrs.meta [ "mainProgram" ]) // {
                  description = "Embeddable Web content engine, WPE port";
                  homepage = "https://wpewebkit.org/";
                  pkgConfigModules = [
                    "wpe-webkit-2.0"
                    "wpe-platform-2.0"
                    "wpe-web-process-extension-2.0"
                  ];
                  platforms = pkgs.lib.platforms.linux;
                  broken = false;
                };
              });

          runtime = pkgs.stdenv.mkDerivation {
            pname = "mux-wpe-kitty";
            version = "0.1.0";
            src = ./spikes/wpe-kitty;
            strictDeps = true;

            nativeBuildInputs = [
              pkgs.meson
              pkgs.ninja
              pkgs.pkg-config
            ];
            buildInputs = [
              pkgs.glib
              pkgs.libxkbcommon
              wpeWebKit
            ];

            mesonBuildType = "release";
            mesonFlags = [ "-Dwerror=true" ];
            doCheck = true;
            checkPhase = ''
              runHook preCheck
              meson test --print-errorlogs --no-rebuild
              runHook postCheck
            '';

            # Keep the Nix runtime output explicit even though Meson also
            # defines install targets for these six executables.
            installPhase = ''
              runHook preInstall
              mkdir -p "$out/bin"
              for program in muxd muxctl mux-bar mux-layer mux-engine mux-pane; do
                install -Dm755 "$program" "$out/bin/$program"
              done
              runHook postInstall
            '';

            meta = {
              description = "WPE WebKit runtime processes for Mux";
              platforms = pkgs.lib.platforms.linux;
            };
          };

          mux = pkgs.writeShellApplication {
            name = "mux";
            runtimeInputs = [
              pkgs.coreutils
              pkgs.glib-networking
              pkgs.kitty
            ];
            text = ''
              if [[ "$(uname -s)" != Linux ]]; then
                echo "Mux requires Linux and WPEPlatform." >&2
                exit 1
              fi

              export PATH="${runtime}/bin:$PATH"
              export GIO_EXTRA_MODULES="${pkgs.glib-networking}/lib/gio/modules''${GIO_EXTRA_MODULES:+:$GIO_EXTRA_MODULES}"
              export FONTCONFIG_FILE="${fontConfig}"
              export FONTCONFIG_PATH="${pkgs.fontconfig.out}/etc/fonts"
              if [[ -n "''${GST_PLUGIN_SYSTEM_PATH_1_0:-}" ]]; then
                export GST_PLUGIN_SYSTEM_PATH_1_0="${gstPluginPath}:$GST_PLUGIN_SYSTEM_PATH_1_0"
              else
                export GST_PLUGIN_SYSTEM_PATH_1_0="${gstPluginPath}"
              fi

              if [[ $# -gt 0 && $1 == ctl ]]; then
                shift
                if [[ $# -gt 0 && $1 != stop ]]; then
                  muxd --ensure
                fi
                exec muxctl "$@"
              fi

              if [[ -z "''${WAYLAND_DISPLAY:-}" && -z "''${DISPLAY:-}" ]]; then
                echo "Mux needs a graphical Linux session (Wayland or X11)." >&2
                exit 1
              fi

              export MUX_LAYER=main
              export MUX_GLOBAL_BAR=1
              uid="$(${pkgs.coreutils}/bin/id -u)"
              if [[ -n "''${XDG_RUNTIME_DIR:-}" ]]; then
                runtime_parent="$XDG_RUNTIME_DIR"
                if [[ "$runtime_parent" != /* ]]; then
                  echo "XDG_RUNTIME_DIR must be an absolute path: $runtime_parent" >&2
                  exit 1
                fi
                if [[ -L "$runtime_parent" ]]; then
                  echo "XDG_RUNTIME_DIR must not be a symlink: $runtime_parent" >&2
                  exit 1
                fi
                if [[ ! -e "$runtime_parent" ]]; then
                  echo "XDG_RUNTIME_DIR does not exist: $runtime_parent" >&2
                  exit 1
                fi
                if [[ ! -d "$runtime_parent" ]]; then
                  echo "XDG_RUNTIME_DIR is not a directory: $runtime_parent" >&2
                  exit 1
                fi
                runtime_owner="$(${pkgs.coreutils}/bin/stat -c %u -- "$runtime_parent")"
                if [[ "$runtime_owner" != "$uid" ]]; then
                  echo "XDG_RUNTIME_DIR is not owned by the current user: $runtime_parent" >&2
                  exit 1
                fi
                runtime_mode="$(${pkgs.coreutils}/bin/stat -c %a -- "$runtime_parent")"
                if (( (8#$runtime_mode & 077) != 0 )); then
                  echo "XDG_RUNTIME_DIR must not grant group or other access: $runtime_parent" >&2
                  exit 1
                fi
                kitty_runtime="$runtime_parent/mux"
              else
                kitty_runtime="/tmp/mux-$uid"
              fi

              umask 077
              if [[ -L "$kitty_runtime" ]]; then
                echo "Refusing symlinked runtime directory: $kitty_runtime" >&2
                exit 1
              fi
              ${pkgs.coreutils}/bin/mkdir -p -- "$kitty_runtime"
              if [[ ! -d "$kitty_runtime" || -L "$kitty_runtime" ||
                    "$(${pkgs.coreutils}/bin/stat -c %u -- "$kitty_runtime")" != "$uid" ]]; then
                echo "Mux runtime directory is not owned by the current user." >&2
                exit 1
              fi
              ${pkgs.coreutils}/bin/chmod 700 -- "$kitty_runtime"

              kitty_socket="$kitty_runtime/kitty-$$.sock"
              if [[ ''${#kitty_socket} -ge 100 ]]; then
                echo "Kitty control socket path is too long: $kitty_socket" >&2
                exit 1
              fi
              ${pkgs.coreutils}/bin/rm -f -- "$kitty_socket"
              export KITTY_LISTEN_ON="unix:$kitty_socket"

              cleanup_kitty_socket() {
                ${pkgs.coreutils}/bin/rm -f -- "$kitty_socket"
              }
              trap cleanup_kitty_socket EXIT

              muxd --ensure

              kitty \
                --config ${./kitty}/wpe.conf \
                --listen-on "$KITTY_LISTEN_ON" \
                --title Mux \
                mux-layer "$@"
            '';
          };
        in
        {
          default = mux;
          inherit mux runtime;
          wpewebkit = wpeWebKit;
        }
      );

      apps = forAllSystems (
        system:
        {
          default = {
            type = "app";
            program = "${self.packages.${system}.default}/bin/mux";
            meta.description = "Launch the Mux Kitty-native WPE browser multiplexer";
          };
        }
      );

      checks = forAllSystems (
        system:
        {
          runtime = self.packages.${system}.runtime;
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.runtime ];
            packages = [
              pkgs.clang-tools
              pkgs.gdb
              pkgs.kitty
            ];
          };
        }
      );
    };
}
