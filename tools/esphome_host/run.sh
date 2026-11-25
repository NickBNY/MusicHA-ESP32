#!/usr/bin/env bash
# Host harness for the ESPHome API code: compiles include/esphome_*.h into a
# loopback server and drives it with the real aioesphomeapi client and
# python-zeroconf. Run from the repository root (or anywhere: it cds there).
#
#   bash tools/esphome_host/run.sh [hold-seconds [quiet-hold-seconds]]
#   defaults: 25 s hold with pushes, 45 s hold on the idle (--quiet) run
#
# Everything generated (venv, binary, logs) lives in /tmp, never in the repo.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

HOLD="${1:-25}"
QUIET_HOLD="${2:-45}"
VENV=/tmp/esphome-venv
PY="$VENV/bin/python"
BIN=/tmp/esphome_host_server
LOGDIR=/tmp/esphome_host_logs
UV="$(command -v uv || echo /opt/homebrew/bin/uv)"

mkdir -p "$LOGDIR"

VERSION="$(sed -n 's/^#define FIRMWARE_VERSION "\(.*\)"/\1/p' include/ui_common.h | head -n 1)"
if [ -z "$VERSION" ]; then
    echo "FAIL: cannot read FIRMWARE_VERSION from include/ui_common.h" >&2
    exit 1
fi

if [ ! -x "$PY" ] || ! "$PY" -c "import aioesphomeapi, zeroconf" 2>/dev/null; then
    echo "[run] creating $VENV"
    "$UV" venv --python 3.12 "$VENV"
    "$UV" pip install --python "$PY" aioesphomeapi zeroconf
fi
"$PY" -c "import aioesphomeapi, zeroconf; print('[run] aioesphomeapi', aioesphomeapi.__file__.split('/')[-2], 'zeroconf', zeroconf.__version__)"
"$UV" pip list --python "$PY" 2>/dev/null | grep -i -E '^(aioesphomeapi|zeroconf) ' || true

echo "[run] compiling host_server (FIRMWARE_VERSION=$VERSION)"
g++ -std=gnu++17 -Wall -Wextra -Iinclude "-DFIRMWARE_VERSION=\"$VERSION\"" \
    -o "$BIN" tools/esphome_host/host_server.cpp

free_port() {
    "$PY" -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1])"
}

SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# run_mode <label> <extra server args...>
run_mode() {
    local label="$1"
    shift
    local tcp udp out err
    tcp="$(free_port)"
    udp="$(free_port)"
    out="$LOGDIR/server-$label.out"
    err="$LOGDIR/server-$label.err"
    : >"$out"
    : >"$err"
    "$BIN" "$tcp" --udp-port "$udp" "$@" >"$out" 2>"$err" &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        grep -q '^READY$' "$out" && break
        kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL: server exited early"; cat "$err"; return 1; }
        sleep 0.1
    done
    grep -q '^READY$' "$out" || { echo "FAIL: server never printed READY"; return 1; }

    echo "=== mode: $label ==="
    local hold="$HOLD"
    # aioesphomeapi only pings when nothing arrived for a whole keepalive
    # interval (20 s) after a quiet interval, so the first Ping of an idle
    # connection comes at about 40 s.
    [ "$label" = quiet ] && hold="$QUIET_HOLD"
    "$PY" tools/esphome_host/check_client.py api --port "$tcp" --version "$VERSION" --hold "$hold" "$@"
    "$PY" tools/esphome_host/check_client.py mdns --udp-port "$udp" --tcp-port "$tcp"
    if [ "$label" = quiet ]; then
        local first
        first="$(sed -n 's/^\[harness\] ping 1 at +\([0-9]*\) ms$/\1/p' "$err" | head -n 1)"
        if [ -z "$first" ] || [ "$first" -lt 30000 ]; then
            echo "  FAIL real client keepalive Ping not seen at about 40 s (first Ping at +${first:-none} ms)"
            return 1
        fi
        echo "  ok   server received the real client keepalive Ping at +${first} ms and the client stayed connected"
    fi
    cleanup
    SERVER_PID=""
}

run_mode no-debug
run_mode debug --debug
run_mode quiet --quiet
echo "ALL PASS: no-debug (6 entities), debug (30 entities), keepalive Ping, mDNS reply"
