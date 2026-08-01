#!/bin/sh
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
packaging=$repo_root/spikes/wpe-kitty/packaging
meson_file=$repo_root/spikes/wpe-kitty/meson.build

fail() {
    printf 'user-install test: %s\n' "$1" >&2
    exit 1
}

assert_line() {
    output=$1
    expected=$2
    printf '%s\n' "$output" | grep -Fqx "$expected" ||
        fail "missing output line: $expected"
}

cmp "$repo_root/kitty/kitty.conf" "$packaging/kitty/kitty.conf" ||
    fail "packaged kitty.conf differs from checkout kitty.conf"
cmp "$repo_root/kitty/wpe.conf" "$packaging/kitty/wpe.conf" ||
    fail "packaged wpe.conf differs from checkout wpe.conf"

install_dir_count=$(
    awk '/install_dir: mux_runtime_install_dir/ { count++ }
         END { print count + 0 }' "$meson_file"
)
[ "$install_dir_count" -eq 6 ] ||
    fail "Meson must install exactly six programs into mux_runtime_install_dir"
grep -Fq "input: 'packaging/mux'" "$meson_file" ||
    fail "Meson does not install the launcher"
if ! awk '
    /install_subdir\(/ {
        in_install_subdir = 1
        source_matches = 0
        destination_matches = 0
    }
    in_install_subdir && /packaging\/kitty/ {
        source_matches = 1
    }
    in_install_subdir && /install_dir: get_option/ && /datadir/ && /mux/ {
        destination_matches = 1
    }
    in_install_subdir && /^[[:space:]]*\)[,]?[[:space:]]*$/ {
        if (source_matches && destination_matches) {
            found = 1
        }
        in_install_subdir = 0
    }
    END { exit found ? 0 : 1 }
' "$meson_file"; then
    fail "Meson does not install packaging/kitty under the mux data directory"
fi

tmp_parent=${TMPDIR:-/tmp}
tmp_parent=$(CDPATH='' cd -P -- "$tmp_parent" && pwd -P) ||
    fail "could not resolve the temporary directory"
tmp_root=$tmp_parent/mux-user-install-test.$$
umask 077
mkdir -p "$tmp_root"
trap 'rm -rf "$tmp_root"' EXIT HUP INT TERM

DESTDIR=$tmp_root/destdir
prefix=/opt/mux-v0.1-test
install_root=$DESTDIR$prefix
runtime_programs='muxd muxctl mux-bar mux-layer mux-engine mux-pane'

cat > "$tmp_root/runtime-stub" <<'EOF'
#!/bin/sh
name=${0##*/}
case $name in
    muxd)
        : "${MUX_TEST_LOG:?}"
        printf 'muxd:%s\n' "$*" >> "$MUX_TEST_LOG"
        ;;
    muxctl)
        printf 'muxctl:%s\n' "$*"
        ;;
esac
EOF
chmod 755 "$tmp_root/runtime-stub"

stage_fixture() {
    mkdir -p \
        "$install_root/bin" \
        "$install_root/libexec/mux" \
        "$install_root/share/mux/kitty"
    install -m 755 "$packaging/mux" "$install_root/bin/mux"
    for program in $runtime_programs; do
        install -m 755 "$tmp_root/runtime-stub" \
            "$install_root/libexec/mux/$program"
    done
    install -m 644 "$packaging/kitty/kitty.conf" \
        "$install_root/share/mux/kitty/kitty.conf"
    install -m 644 "$packaging/kitty/wpe.conf" \
        "$install_root/share/mux/kitty/wpe.conf"
}

stage_fixture
stage_fixture

paths=$("$install_root/bin/mux" --print-install-paths)
assert_line "$paths" "mode=installed"
assert_line "$paths" "prefix=$install_root"
assert_line "$paths" "libexec=$install_root/libexec/mux"
assert_line "$paths" "kitty_config=$install_root/share/mux/kitty/wpe.conf"

relocated=$tmp_root/relocated-prefix
mv "$install_root" "$relocated"

mkdir -p "$tmp_root/path-bin"
ln -s "$relocated/bin/mux" "$tmp_root/path-bin/mux"
paths=$("$tmp_root/path-bin/mux" --print-install-paths)
assert_line "$paths" "prefix=$relocated"
assert_line "$paths" "libexec=$relocated/libexec/mux"
assert_line "$paths" "kitty_config=$relocated/share/mux/kitty/wpe.conf"

mkdir -p "$tmp_root/tools"
cat > "$tmp_root/tools/uname" <<'EOF'
#!/bin/sh
printf 'Linux\n'
EOF
cat > "$tmp_root/tools/id" <<'EOF'
#!/bin/sh
if [ "${1:-}" = -u ]; then
    printf '1000\n'
    exit 0
fi
exit 1
EOF
chmod 755 "$tmp_root/tools/uname" "$tmp_root/tools/id"

log=$tmp_root/launcher.log
ctl_output=$(
    PATH="$tmp_root/tools:$PATH" MUX_TEST_LOG="$log" \
        "$relocated/bin/mux" ctl status
)
[ "$ctl_output" = 'muxctl:status' ] ||
    fail "headless ctl dispatch returned: $ctl_output"
grep -Fqx 'muxd:--ensure' "$log" ||
    fail "launcher did not ensure muxd before muxctl"

stop_output=$(
    PATH="$tmp_root/tools:$PATH" MUX_TEST_LOG="$log" \
        "$relocated/bin/mux" ctl stop
)
[ "$stop_output" = 'muxctl:stop' ] ||
    fail "headless stop dispatch returned: $stop_output"
[ "$(wc -l < "$log" | tr -d ' ')" -eq 1 ] ||
    fail "launcher started muxd before an idempotent stop"

saved_mux_share=$tmp_root/real-mux-share
outside_share=$tmp_root/outside-share
mv "$relocated/share/mux" "$saved_mux_share"
mkdir -p "$outside_share/kitty"
printf 'outside\n' > "$outside_share/kitty/kitty.conf"
ln -s "$outside_share" "$relocated/share/mux"
if "$relocated/bin/mux" --uninstall >"$tmp_root/unsafe-uninstall.log" 2>&1; then
    fail "uninstall followed a symlinked intermediate directory"
fi
[ "$(cat "$outside_share/kitty/kitty.conf")" = outside ] ||
    fail "refused uninstall modified a file outside the prefix"
rm "$relocated/share/mux"
mv "$saved_mux_share" "$relocated/share/mux"

printf 'preserve\n' > "$relocated/share/mux/keep.txt"
"$relocated/bin/mux" --uninstall >/dev/null

[ ! -e "$relocated/bin/mux" ] ||
    fail "uninstall left the launcher"
for program in $runtime_programs; do
    [ ! -e "$relocated/libexec/mux/$program" ] ||
        fail "uninstall left runtime program: $program"
done
[ ! -e "$relocated/share/mux/kitty/kitty.conf" ] ||
    fail "uninstall left kitty.conf"
[ ! -e "$relocated/share/mux/kitty/wpe.conf" ] ||
    fail "uninstall left wpe.conf"
[ -f "$relocated/share/mux/keep.txt" ] ||
    fail "bounded uninstall removed an unrelated file"

printf 'user-install test: ok\n'
