{
  description = "Mux: a Kitty-native browser shell";

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

          kittyTransport = pkgs.stdenv.mkDerivation {
            pname = "mux-kitty-transport";
            version = "0.1.0";
            src = ./spikes/kitty-transport;
            strictDeps = true;
            nativeBuildInputs = [
              pkgs.meson
              pkgs.ninja
              pkgs.pkg-config
            ];
            buildInputs = [ pkgs.glib ];
          };

          mux = pkgs.writeShellApplication {
            name = "mux";
            runtimeInputs = [
              pkgs.kitty
              kittyTransport
            ];
            text = ''
              if [[ -z "''${WAYLAND_DISPLAY:-}" && -z "''${DISPLAY:-}" ]]; then
                echo "Mux needs a Linux graphical session (Wayland or X11)." >&2
                exit 1
              fi

              exec ${pkgs.kitty}/bin/kitty \
                --config ${./kitty/kitty.conf} \
                --title Mux \
                ${kittyTransport}/bin/mux-kitty-transport "$@"
            '';
          };
        in
        {
          default = mux;
          inherit mux;
          kitty-transport = kittyTransport;
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

      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            packages = [
              pkgs.clang-tools
              pkgs.gdb
              pkgs.glib
              pkgs.kitty
              pkgs.meson
              pkgs.ninja
              pkgs.pkg-config
            ];
          };
        }
      );
    };
}
