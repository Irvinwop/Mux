{ pkgs }:
let
  policyVersion = "mux-fonts-v1";

  # All sources and versions come from the repository's pinned nixpkgs input.
  # Keep additions explicit: this list defines the installed-font environment.
  fontPackages = with pkgs; [
    dejavu_fonts
    roboto
    noto-fonts
    noto-fonts-cjk-sans
    noto-fonts-color-emoji
    stix-two
  ];
  fontDirectories = map (package: "${package}/share/fonts") fontPackages;
  fontRoots = pkgs.writeText "${policyVersion}-roots.txt" (
    pkgs.lib.concatStringsSep "\n" fontDirectories + "\n"
  );

  # Do not use makeFontsConf here. Its defaults include host directories and
  # its generated XML includes an XDG font directory even with empty impure
  # directories. No external configuration or font directory is included here.
  config = pkgs.writeText "${policyVersion}.conf" ''
    <?xml version="1.0"?>
    <fontconfig>
      <reset-dirs/>
      ${pkgs.lib.concatMapStringsSep "\n" (directory: "<dir>${directory}</dir>") fontDirectories}

      <!-- A writable metadata cache is not an additional font directory. -->
      <cachedir prefix="xdg">fontconfig/${policyVersion}</cachedir>

      <alias binding="strong">
        <family>sans-serif</family>
        <prefer>
          <family>Roboto</family>
          <family>DejaVu Sans</family>
          <family>Noto Sans</family>
        </prefer>
      </alias>
      <alias binding="strong">
        <family>system-ui</family>
        <prefer>
          <family>Roboto</family>
          <family>DejaVu Sans</family>
          <family>Noto Sans</family>
        </prefer>
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
        <prefer>
          <family>DejaVu Serif</family>
          <family>Noto Serif</family>
        </prefer>
      </alias>
      <alias binding="strong">
        <family>monospace</family>
        <prefer>
          <family>DejaVu Sans Mono</family>
          <family>Noto Sans Mono</family>
        </prefer>
      </alias>
      <alias binding="strong">
        <family>math</family>
        <prefer><family>STIX Two Math</family></prefer>
      </alias>
      <alias binding="strong">
        <family>emoji</family>
        <prefer><family>Noto Color Emoji</family></prefer>
      </alias>
    </fontconfig>
  '';
in
{
  inherit config fontPackages fontDirectories policyVersion;

  # This check intentionally does not depend on the WPE build or a display.
  check = pkgs.runCommand "${policyVersion}-check" {
    nativeBuildInputs = with pkgs; [
      bash
      coreutils
      findutils
      fontconfig
      gnugrep
    ];
    MUX_FONTCONFIG_FILE = config;
    MUX_FONT_ROOTS_FILE = fontRoots;
    MUX_POISON_FONT_ROOT = "${pkgs.inconsolata}/share/fonts";
    MUX_NIX_STORE_DIR = builtins.storeDir;
    MUX_FONT_POLICY_VERSION = policyVersion;
  } ''
    bash ${./test-font-policy.sh}
  '';
}
