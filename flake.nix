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
              pkgs.wpewebkit
            ];

            mesonBuildType = "release";
            mesonFlags = [ "-Dwerror=true" ];
            doCheck = true;
            checkPhase = ''
              runHook preCheck
              meson test -C build --print-errorlogs --no-rebuild
              runHook postCheck
            '';

            # The WPE Meson project does not define install targets yet.
            installPhase = ''
              runHook preInstall
              mkdir -p "$out/bin"
              for program in muxd muxctl mux-bar mux-layer mux-engine mux-pane; do
                install -Dm755 "build/$program" "$out/bin/$program"
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
              pkgs.kitty
            ];
            text = ''
              if [[ "$(uname -s)" != Linux ]]; then
                echo "Mux requires Linux and WPEPlatform." >&2
                exit 1
              fi

              export PATH="${runtime}/bin:$PATH"
              if [[ -n "''${GST_PLUGIN_SYSTEM_PATH_1_0:-}" ]]; then
                export GST_PLUGIN_SYSTEM_PATH_1_0="${gstPluginPath}:$GST_PLUGIN_SYSTEM_PATH_1_0"
              else
                export GST_PLUGIN_SYSTEM_PATH_1_0="${gstPluginPath}"
              fi

              muxd --ensure

              if [[ $# -gt 0 && $1 == ctl ]]; then
                shift
                exec muxctl "$@"
              fi

              if [[ -z "''${WAYLAND_DISPLAY:-}" && -z "''${DISPLAY:-}" ]]; then
                echo "Mux needs a graphical Linux session (Wayland or X11)." >&2
                exit 1
              fi

              export MUX_LAYER=main
              export MUX_GLOBAL_BAR=1
              export KITTY_LISTEN_ON="unix:@mux-kitty-$$"

              exec kitty \
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
        }
      );

      apps = forAllSystems (
        system:
        {
          default = {
            type = "app";
            program = "${self.packages.${system}.default}/bin/mux";
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
