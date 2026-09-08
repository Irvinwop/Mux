#!/bin/sh
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
# Socket-path tests need a short parent even on macOS's long default TMPDIR.
tmp_root=$(mktemp -d /tmp/mux-tor-launcher.XXXXXXXX)
trap 'rm -rf -- "$tmp_root"' EXIT HUP INT TERM
umask 077
mkdir -p "$tmp_root/prefix/bin" "$tmp_root/prefix/libexec/mux" \
    "$tmp_root/prefix/share/mux/kitty" "$tmp_root/tools" "$tmp_root/runtime"
cp "$repo_root/spikes/wpe-kitty/packaging/mux" "$tmp_root/prefix/bin/mux"
chmod 755 "$tmp_root/prefix/bin/mux"
: > "$tmp_root/prefix/share/mux/kitty/wpe.conf"

fail() {
    printf 'tor-launcher test: %s\n' "$1" >&2
    exit 1
}

cat > "$tmp_root/runtime-stub" <<'EOF'
#!/bin/sh
name=${0##*/}
printf '%s:%s\n' "$name" "$*" >> "$MUX_TEST_LOG"
if [ "$name" = mux-engine ]; then
    [ "$*" = --check-network ] || exit 2
    [ "${MUX_NETWORK_MODE:-}" = tor ] || exit 2
    [ "${MUX_TEST_NETWORK_FAIL:-0}" = 0 ] || exit 1
fi
if [ "$name" = muxd ] && [ "$*" = --ensure-tor-policy ]; then
    [ "${MUX_TEST_DAEMON_POLICY_FAIL:-0}" = 0 ] || exit 1
fi
EOF
for program in muxd muxctl mux-bar mux-layer mux-engine mux-pane; do
    cp "$tmp_root/runtime-stub" "$tmp_root/prefix/libexec/mux/$program"
    chmod 755 "$tmp_root/prefix/libexec/mux/$program"
done
cat > "$tmp_root/tools/uname" <<'EOF'
#!/bin/sh
printf 'Linux\n'
EOF
cat > "$tmp_root/tools/id" <<'EOF'
#!/bin/sh
printf '1000\n'
EOF
cat > "$tmp_root/tools/stat" <<'EOF'
#!/bin/sh
case $2 in
    %u) printf '1000\n' ;;
    %a) printf '700\n' ;;
    *) exit 1 ;;
esac
EOF
MUX_TEST_REAL_CHMOD=$(command -v chmod)
export MUX_TEST_REAL_CHMOD
cat > "$tmp_root/tools/chmod" <<'EOF'
#!/bin/sh
# Linux accepts this post-mode option terminator; BSD chmod does not.
mode=$1
shift
if [ "$mode" = 700 ] && [ "${1:-}" = -- ]; then
    shift
fi
exec "$MUX_TEST_REAL_CHMOD" "$mode" "$@"
EOF
cat > "$tmp_root/tools/kitty" <<'EOF'
#!/bin/sh
if [ "${1:-}" = --version ]; then
    printf 'kitty 0.45.0\n'
    exit 0
fi
printf 'kitty:%s\n' "$*" >> "$MUX_TEST_LOG"
printf 'mode=%s\nprofile=%s\nephemeral=%s\nproxy=%s\nsocket=%s\ndata=%s\ncache=%s\npopup=%s\n' \
    "${MUX_NETWORK_MODE-unset}" "${MUX_PROFILE-unset}" "${MUX_EPHEMERAL-unset}" \
    "${MUX_TOR_SOCKS_PROXY-unset}" "${MUX_ENGINE_SOCKET-unset}" \
    "${MUX_PROFILE_DATA_DIR-unset}" "${MUX_PROFILE_CACHE_DIR-unset}" \
    "${MUX_POPUP_TOKEN-unset}" > "$MUX_TEST_ENV"
EOF
cat > "$tmp_root/tools/kitten" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 755 "$tmp_root/tools/"*

PATH=$tmp_root/tools:$PATH
XDG_RUNTIME_DIR=$tmp_root/runtime
DISPLAY=:fixture
MUX_TEST_LOG=$tmp_root/launch.log
MUX_TEST_ENV=$tmp_root/environment
export PATH XDG_RUNTIME_DIR DISPLAY MUX_TEST_LOG MUX_TEST_ENV
unset MUX_NETWORK_MODE MUX_TOR_SOCKS_PROXY MUX_PROFILE MUX_EPHEMERAL
unset MUX_ENGINE_SOCKET MUX_PROFILE_DATA_DIR MUX_PROFILE_CACHE_DIR MUX_POPUP_TOKEN
launcher=$tmp_root/prefix/bin/mux

assert_env() {
    rg -F -x -- "$1" "$MUX_TEST_ENV" >/dev/null || fail "missing environment: $1"
}

: > "$MUX_TEST_LOG"
if ! "$launcher" https://direct.test/ >"$tmp_root/direct-launch.log" 2>&1; then
    cat "$tmp_root/direct-launch.log" >&2
    fail 'direct fixture launch failed'
fi
assert_env 'mode=direct'
assert_env 'profile=unset'
rg -F -x 'muxd:--ensure' "$MUX_TEST_LOG" >/dev/null ||
    fail 'direct launch changed legacy daemon ensure behavior'
if rg -q '^mux-engine:' "$MUX_TEST_LOG"; then
    fail 'direct launch unexpectedly probes or starts an engine'
fi

long_runtime=$tmp_root/$(printf '%090d' 0)
mkdir -p "$long_runtime"
: > "$MUX_TEST_LOG"
if XDG_RUNTIME_DIR=$long_runtime "$launcher" https://direct.test/ \
    >"$tmp_root/long-path.log" 2>&1; then
    fail 'overlong control socket path was accepted'
fi
rg -F 'Kitty control socket path is too long' "$tmp_root/long-path.log" >/dev/null ||
    fail 'overlong socket path failed for an unexpected reason'
if rg -q '^kitty:' "$MUX_TEST_LOG"; then
    fail 'overlong socket path reached Kitty'
fi

for attempt in first second; do
    : > "$MUX_TEST_LOG"
    MUX_PROFILE=default MUX_EPHEMERAL=0 MUX_ENGINE_SOCKET=/tmp/direct.sock \
    MUX_PROFILE_DATA_DIR=/tmp/direct-data MUX_PROFILE_CACHE_DIR=/tmp/direct-cache \
    MUX_POPUP_TOKEN=direct-popup \
        "$launcher" --tor --tor-socks-proxy socks5://127.0.0.1:9050 \
        https://tor.test/ >/dev/null 2>&1
    assert_env 'mode=tor'
    assert_env 'ephemeral=1'
    assert_env 'proxy=socks5://127.0.0.1:9050'
    assert_env 'socket=unset'
    assert_env 'data=unset'
    assert_env 'cache=unset'
    assert_env 'popup=unset'
    profile=$(sed -n 's/^profile=//p' "$MUX_TEST_ENV")
    case $profile in tor-private-????????????) ;; *) fail 'Tor profile is not freshly namespaced' ;; esac
    [ "$(sed -n '1p' "$MUX_TEST_LOG")" = 'mux-engine:--check-network' ] ||
        fail 'Tor did not preflight before daemon/Kitty launch'
    [ "$(sed -n '2p' "$MUX_TEST_LOG")" = 'muxd:--ensure-tor-policy' ] ||
        fail 'Tor launch did not require daemon policy capability'
    rg -q '^kitty:.*--title Mux Tor SOCKS' "$MUX_TEST_LOG" ||
        fail 'Tor mode has no visible window title'
    if [ "$attempt" = first ]; then
        first_profile=$profile
    else
        [ "$profile" != "$first_profile" ] || fail 'separate Tor launches reused a profile'
    fi
done

: > "$MUX_TEST_LOG"
if MUX_TEST_DAEMON_POLICY_FAIL=1 "$launcher" --tor \
    --tor-socks-proxy socks5://127.0.0.1:9050 >"$tmp_root/failure" 2>&1; then
    fail 'legacy daemon policy failure launched successfully'
fi
if rg -q '^kitty:' "$MUX_TEST_LOG"; then
    fail 'legacy daemon policy failure reached Kitty'
fi
rg -F 'restart the Mux daemon to use Tor' "$tmp_root/failure" >/dev/null ||
    fail 'legacy daemon failure did not explain the required restart'

: > "$MUX_TEST_LOG"
if MUX_TEST_NETWORK_FAIL=1 "$launcher" --tor \
    --tor-socks-proxy socks5://127.0.0.1:9050 >"$tmp_root/failure" 2>&1; then
    fail 'failed SOCKS preflight launched successfully'
fi
if rg -q '^(kitty|muxd):' "$MUX_TEST_LOG"; then
    fail 'failed SOCKS preflight reached the daemon or Kitty'
fi
if "$launcher" --tor >"$tmp_root/failure" 2>&1; then
    fail 'missing Tor endpoint was accepted'
fi
if "$launcher" --tor-socks-proxy socks5://127.0.0.1:9050 >"$tmp_root/failure" 2>&1; then
    fail 'proxy without Tor mode was accepted'
fi
if MUX_NETWORK_MODE=unknown "$launcher" >"$tmp_root/failure" 2>&1; then
    fail 'unknown network mode was accepted'
fi
if "$launcher" --tor --tor-socks-proxy socks5://127.0.0.1:9050 ctl status \
    >"$tmp_root/failure" 2>&1; then
    fail 'Tor flags were silently applied to global control'
fi

MUX_NETWORK_MODE=tor MUX_TOR_SOCKS_PROXY=socks5://127.0.0.1:9150 \
    "$launcher" https://environment.test/ >/dev/null 2>&1
assert_env 'mode=tor'
assert_env 'proxy=socks5://127.0.0.1:9150'
printf 'tor-launcher test: ok\n'
