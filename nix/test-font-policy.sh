#!/usr/bin/env bash
set -euo pipefail

: "${out:?This script is run by the font-policy Nix check}"
: "${MUX_FONTCONFIG_FILE:?}"
: "${MUX_FONT_ROOTS_FILE:?}"
: "${MUX_POISON_FONT_ROOT:?}"
: "${MUX_NIX_STORE_DIR:?}"
: "${MUX_FONT_POLICY_VERSION:?}"

fail() {
    printf 'font-policy: %s\n' "$*" >&2
    exit 1
}

work="$(mktemp -d "${TMPDIR:-/tmp}/mux-font-policy.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT
export LC_ALL=C
export HOME="$work/home"
export XDG_DATA_HOME="$work/xdg-data"
export XDG_CONFIG_HOME="$work/xdg-config"
export XDG_CACHE_HOME="$work/xdg-cache"
export XDG_DATA_DIRS="$work/host/share"
export XDG_CONFIG_DIRS="$work/host/etc/xdg"
export FONTCONFIG_PATH="$work/fontconfig"

mkdir -p "$out/coverage" "$XDG_CONFIG_HOME/fontconfig/conf.d" \
    "$HOME/.fonts.conf.d" "$FONTCONFIG_PATH/conf.d" \
    "$XDG_CONFIG_DIRS/fontconfig" "$XDG_CACHE_HOME"

foreign_font="$(find -L "$MUX_POISON_FONT_ROOT" -type f \
    \( -iname '*.ttf' -o -iname '*.otf' \) -print -quit)"
[[ -n "$foreign_font" ]] || fail 'The pinned Inconsolata fixture has no font file'
poison_name="mux-foreign-font.${foreign_font##*.}"
fixture_directories=(
    "$XDG_DATA_HOME/fonts"
    "$HOME/.fonts"
    "$HOME/.local/share/fonts"
    "$HOME/.nix-profile/share/fonts"
    "$work/host/share/fonts"
)
for directory in "${fixture_directories[@]}"; do
    mkdir -p "$directory"
    cp -- "$foreign_font" "$directory/$poison_name"
done

# A positive control proves that the planted files are valid and discoverable.
# Copies of this configuration also poison common automatic include locations.
control_config="$work/poison.conf"
{
    printf '<?xml version="1.0"?>\n<fontconfig>\n<reset-dirs/>\n'
    for directory in "${fixture_directories[@]}"; do
        printf '<dir>%s</dir>\n' "$directory"
    done
    printf '<cachedir>%s/control-cache</cachedir>\n</fontconfig>\n' "$work"
} > "$control_config"
for destination in \
    "$XDG_CONFIG_HOME/fontconfig/fonts.conf" \
    "$XDG_CONFIG_HOME/fontconfig/conf.d/99-mux-poison.conf" \
    "$HOME/.fonts.conf" \
    "$HOME/.fonts.conf.d/99-mux-poison.conf" \
    "$FONTCONFIG_PATH/fonts.conf" \
    "$FONTCONFIG_PATH/conf.d/99-mux-poison.conf" \
    "$XDG_CONFIG_DIRS/fontconfig/fonts.conf"; do
    cp -- "$control_config" "$destination"
done

FONTCONFIG_FILE="$control_config" fc-list --format '%{file}\n' \
    | sort -u > "$work/positive-control-font-files.txt"
for directory in "${fixture_directories[@]}"; do
    grep -Fxq -- "$directory/$poison_name" "$work/positive-control-font-files.txt" \
        || fail "Positive control did not discover $directory/$poison_name"
done
poison_family="$(FONTCONFIG_FILE="$control_config" \
    fc-scan --format '%{family[0]}' "$foreign_font")"
[[ -n "$poison_family" ]] || fail 'The foreign font has no discoverable family'

# Normalize temporary paths in retained artifacts for reproducible check output.
while IFS= read -r font_file; do
    case "$font_file" in
        "$work"/*) printf 'fixture/%s\n' "${font_file#"$work"/}" ;;
        *) fail "Positive control unexpectedly exposed $font_file" ;;
    esac
done < "$work/positive-control-font-files.txt" > "$out/positive-control-font-files.txt"
printf '%s\n' "$poison_family" > "$out/foreign-font-family.txt"

allowed_roots=()
while IFS= read -r font_root || [[ -n "$font_root" ]]; do
    [[ -n "$font_root" ]] || continue
    canonical_root="$(realpath -e -- "$font_root")"
    [[ -d "$canonical_root" ]] || fail "Declared font root is not a directory: $font_root"
    case "$canonical_root" in
        "$MUX_NIX_STORE_DIR"/*) ;;
        *) fail "Declared font root is outside the Nix store: $canonical_root" ;;
    esac
    allowed_roots+=("$canonical_root")
    printf '%s\n' "$canonical_root" >> "$out/font-roots.txt"
done < "$MUX_FONT_ROOTS_FILE"
[[ ${#allowed_roots[@]} -gt 0 ]] || fail 'No immutable font roots were declared'

# A declared package can link individual fonts into another store output.
# Derive the exact permitted targets from declared roots, never from fc-list.
# pipefail and realpath -e make traversal or resolution errors fatal.
declare -A allowed_files=()
find -L "${allowed_roots[@]}" -type f -print0 \
    | sort -zu > "$work/declared-font-paths"
: > "$work/declared-font-source-targets.tsv"
while IFS= read -r -d '' declared_file; do
    canonical_file="$(realpath -e -- "$declared_file")"
    [[ -f "$canonical_file" ]] \
        || fail "Declared font entry is not a file: $declared_file"
    case "$canonical_file" in
        "$MUX_NIX_STORE_DIR"/*) ;;
        *) fail "Declared font entry resolves outside the Nix store: $declared_file ($canonical_file)" ;;
    esac
    allowed_files["$canonical_file"]=1
    printf '%s\t%s\n' "$declared_file" "$canonical_file" \
        >> "$work/declared-font-source-targets.tsv"
done < "$work/declared-font-paths"
[[ ${#allowed_files[@]} -gt 0 ]] || fail 'No files are reachable from the declared font roots'
printf '%s\n' "${!allowed_files[@]}" | sort > "$out/allowed-font-files.txt"
sort -u "$work/declared-font-source-targets.tsv" > "$out/declared-font-source-targets.tsv"

assert_allowed_file() {
    local candidate="$1" canonical
    [[ -n "$candidate" ]] || fail 'Fontconfig returned an empty font path'
    canonical="$(realpath -e -- "$candidate")"
    [[ -f "$canonical" ]] || fail "Fontconfig returned a non-file: $candidate"
    if [[ ${allowed_files["$canonical"]+present} == present ]]; then
        return 0
    fi
    fail "Fontconfig exposed a font not reachable from the declared roots: $candidate ($canonical)"
}

# Keep the hostile HOME/XDG/FONTCONFIG_PATH environment, changing only the
# absolute configuration file in the same way as the Nix launcher does.
export FONTCONFIG_FILE="$MUX_FONTCONFIG_FILE"
fc-list --format '%{file}\n' | sort -u > "$out/font-files.txt"
[[ -s "$out/font-files.txt" ]] || fail 'The closed font environment is empty'
while IFS= read -r font_file; do
    assert_allowed_file "$font_file"
done < "$out/font-files.txt"
fc-list --format '%{family[0]}\n' | sort -u > "$out/font-families.txt"
if grep -Fxq -- "$poison_family" "$out/font-families.txt"; then
    fail "The planted user font family is visible: $poison_family"
fi

check_alias() {
    local request="$1" expected="$2" match family font_file
    match="$(fc-match --format '%{family[0]}|%{file}' "$request")"
    family="${match%%|*}"
    font_file="${match#*|}"
    [[ "$family" == "$expected" ]] \
        || fail "Alias '$request' selected '$family', expected '$expected'"
    assert_allowed_file "$font_file"
    printf '%s\t%s\t%s\n' "$request" "$family" "$font_file" >> "$out/alias-matches.tsv"
}
check_alias 'sans-serif' 'Roboto'
check_alias 'system-ui' 'Roboto'
check_alias 'Arial' 'Roboto'
check_alias 'Google Sans' 'Roboto'
check_alias 'serif' 'DejaVu Serif'
check_alias 'monospace' 'DejaVu Sans Mono'
check_alias 'math' 'STIX Two Math'
check_alias 'emoji' 'Noto Color Emoji'

# These are representative declared glyphs, not browser shaping/rendering tests
# or a claim of complete coverage for any script.
while read -r sample codepoint; do
    fc-list --format '%{file}\n' ":charset=$codepoint" \
        | sort -u > "$out/coverage/$sample.txt"
    [[ -s "$out/coverage/$sample.txt" ]] \
        || fail "No declared font covers $sample (U+$codepoint)"
    while IFS= read -r font_file; do
        assert_allowed_file "$font_file"
    done < "$out/coverage/$sample.txt"
    printf '%s\tU+%s\n' "$sample" "$codepoint" >> "$out/glyph-coverage.tsv"
done <<'SAMPLES'
latin 0041
greek 03B1
cyrillic 0416
hebrew 05D0
arabic 0627
devanagari 0915
tamil 0B95
thai 0E01
han 4E00
hiragana 3042
hangul AC00
math 1D400
emoji 1F600
SAMPLES

cp -- "$MUX_FONTCONFIG_FILE" "$out/fonts.conf"
printf '%s\n' "$MUX_FONT_POLICY_VERSION" > "$out/policy-version.txt"
printf 'font-policy: closed Fontconfig paths, aliases, and representative glyph coverage passed\n'
