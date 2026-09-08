#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    printf 'usage: %s MUX_LAYER_PATH\n' "$0" >&2
    exit 2
fi
mux_layer=$(CDPATH='' cd -- "$(dirname -- "$1")" && pwd -P)/$(basename -- "$1")
[ -x "$mux_layer" ] || exit 2
tmp_root=$(mktemp -d /tmp/mux-layer-policy.XXXXXXXX)
trap 'rm -rf -- "$tmp_root"' EXIT HUP INT TERM
umask 077
mkdir "$tmp_root/bin"

fail() {
    printf 'mux-layer policy test: %s\n' "$1" >&2
    exit 1
}

cat > "$tmp_root/bin/kitten" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$MUX_TEST_LAYER_ARGS"
EOF
cat > "$tmp_root/bin/mux-pane" <<'EOF'
#!/bin/sh
printf 'mode=%s\nprofile=%s\nephemeral=%s\nproxy=%s\nsocket=%s\ndata=%s\ncache=%s\npopup=%s\nlayer=%s\n' \
    "${MUX_NETWORK_MODE-unset}" "${MUX_PROFILE-unset}" "${MUX_EPHEMERAL-unset}" \
    "${MUX_TOR_SOCKS_PROXY-unset}" "${MUX_ENGINE_SOCKET-unset}" \
    "${MUX_PROFILE_DATA_DIR-unset}" "${MUX_PROFILE_CACHE_DIR-unset}" \
    "${MUX_POPUP_TOKEN-unset}" "${MUX_LAYER-unset}" > "$MUX_TEST_LAYER_CONTENT"
i=0
while [ ! -f "$MUX_TEST_LAYER_ARGS" ]; do
    i=$((i + 1))
    [ "$i" -le 100 ] || exit 1
    sleep 0.01
done
EOF
chmod 755 "$tmp_root/bin/kitten" "$tmp_root/bin/mux-pane"

PATH=$tmp_root/bin:$PATH
KITTY_WINDOW_ID=701
MUX_LAYER=source-layer
MUX_TEST_LAYER_ARGS=$tmp_root/arguments
MUX_TEST_LAYER_CONTENT=$tmp_root/content
export PATH KITTY_WINDOW_ID MUX_LAYER MUX_TEST_LAYER_ARGS MUX_TEST_LAYER_CONTENT
unset MUX_NETWORK_MODE MUX_TOR_SOCKS_PROXY MUX_PROFILE MUX_EPHEMERAL
unset MUX_ENGINE_SOCKET MUX_PROFILE_DATA_DIR MUX_PROFILE_CACHE_DIR MUX_POPUP_TOKEN

assert_override() {
    awk -v expected="$1" '
        previous == "--env" && $0 == expected { found = 1 }
        { previous = $0 }
        END { exit !found }
    ' "$MUX_TEST_LAYER_ARGS" || fail "missing explicit override: $1"
}

assert_content() {
    awk -v expected="$1" '$0 == expected { found = 1 } END { exit !found }' \
        "$MUX_TEST_LAYER_CONTENT" || fail "missing content policy: $1"
}

for split in vsplit hsplit; do
    "$mux_layer" "--split=$split" https://direct.test/
    assert_override 'MUX_NETWORK_MODE=direct'
    assert_override 'MUX_TOR_SOCKS_PROXY'
    assert_override 'MUX_PROFILE=default'
    assert_override 'MUX_EPHEMERAL=0'
    assert_override 'MUX_LAYER=source-layer'

    MUX_NETWORK_MODE=tor MUX_TOR_SOCKS_PROXY=socks5://127.0.0.1:9050 \
    MUX_PROFILE=tor-private-aaaaaaaa MUX_EPHEMERAL=0 \
    MUX_ENGINE_SOCKET=/tmp/stale-direct.sock MUX_PROFILE_DATA_DIR=/tmp/stale-data \
    MUX_PROFILE_CACHE_DIR=/tmp/stale-cache MUX_POPUP_TOKEN=stale-token \
        "$mux_layer" "--split=$split" https://tor.test/
    assert_override 'MUX_NETWORK_MODE=tor'
    assert_override 'MUX_TOR_SOCKS_PROXY=socks5://127.0.0.1:9050'
    assert_override 'MUX_PROFILE=tor-private-aaaaaaaa'
    assert_override 'MUX_EPHEMERAL=1'
    assert_override 'MUX_ENGINE_SOCKET'
    assert_override 'MUX_PROFILE_DATA_DIR'
    assert_override 'MUX_PROFILE_CACHE_DIR'
    assert_override 'MUX_POPUP_TOKEN'
done

rm -f "$MUX_TEST_LAYER_ARGS" "$MUX_TEST_LAYER_CONTENT"
MUX_NETWORK_MODE=tor MUX_TOR_SOCKS_PROXY=socks5://127.0.0.1:9050 \
MUX_PROFILE=tor-private-aaaaaaaa MUX_EPHEMERAL=0 \
MUX_ENGINE_SOCKET=/tmp/stale-direct.sock MUX_PROFILE_DATA_DIR=/tmp/stale-data \
MUX_PROFILE_CACHE_DIR=/tmp/stale-cache MUX_POPUP_TOKEN=stale-token \
    "$mux_layer" --new https://tor.test/
assert_content 'mode=tor'
assert_content 'profile=tor-private-aaaaaaaa'
assert_content 'ephemeral=1'
assert_content 'proxy=socks5://127.0.0.1:9050'
assert_content 'socket=unset'
assert_content 'data=unset'
assert_content 'cache=unset'
assert_content 'popup=unset'
assert_override 'MUX_NETWORK_MODE=tor'
assert_override 'MUX_TOR_SOCKS_PROXY=socks5://127.0.0.1:9050'
assert_override 'MUX_PROFILE=tor-private-aaaaaaaa'
assert_override 'MUX_EPHEMERAL=1'
layer=$(sed -n 's/^layer=//p' "$MUX_TEST_LAYER_CONTENT")
case $layer in layer-????????) ;; *) fail 'new layer was not generated' ;; esac
assert_override "MUX_LAYER=$layer"

expect_rejection() {
    rm -f "$MUX_TEST_LAYER_ARGS" "$MUX_TEST_LAYER_CONTENT"
    if "$@" "$mux_layer" --split=vsplit https://rejected.test/ \
        >"$tmp_root/failure" 2>&1; then
        fail 'invalid or lost network policy was accepted'
    fi
    [ ! -e "$MUX_TEST_LAYER_ARGS" ] && [ ! -e "$MUX_TEST_LAYER_CONTENT" ] ||
        fail 'invalid policy started Kitty or content'
}

expect_rejection env MUX_PROFILE=tor-private-aaaaaaaa
expect_rejection env MUX_NETWORK_MODE=direct MUX_PROFILE=tor-private-aaaaaaaa
expect_rejection env MUX_NETWORK_MODE=tor MUX_PROFILE=default \
    MUX_TOR_SOCKS_PROXY=socks5://127.0.0.1:9050
expect_rejection env MUX_NETWORK_MODE=tor MUX_PROFILE=tor-private-short \
    MUX_TOR_SOCKS_PROXY=socks5://127.0.0.1:9050
expect_rejection env MUX_NETWORK_MODE=tor MUX_PROFILE=tor-private-aaaaaaaa \
    MUX_TOR_SOCKS_PROXY=socks5://localhost:9050
expect_rejection env MUX_NETWORK_MODE=direct MUX_TOR_SOCKS_PROXY=
printf 'mux-layer policy test: ok\n'
