#!/bin/sh
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Exercises the connect budget in the socketio adapters against real
# destinations and prints one line per attempt.
#
# The mocked unit tests prove the candidate loop calls connect the expected
# number of times. They cannot prove that a genuinely blackholed address is
# abandoned after the budget expires instead of running to the OS timeout, nor
# that a reachable candidate behind a blackholed one is still reached. That is
# what this harness measures.
#
#   ./run_connect_matrix.sh <path-to-build-dir>
#
# <path-to-build-dir> is a directory containing libaziotsharedutil.a, i.e. the
# output of:
#
#   cmake <repo> -DCMAKE_BUILD_TYPE=Release -Dskip_samples=ON -Duse_openssl=ON \
#                -Drun_unittests=OFF
#   make aziotsharedutil
#
# Requires gcc, python3 and unshare. Nothing outside this script's own
# temporary files is modified: the multi-address cases supply a private
# /etc/hosts inside a mount namespace, so the host's /etc/hosts is untouched.
# The script verifies that afterwards.

set -e

BUILD_DIR="$1"
if [ -z "$BUILD_DIR" ] || [ ! -f "$BUILD_DIR/libaziotsharedutil.a" ]; then
    echo "usage: $0 <build-dir containing libaziotsharedutil.a>" >&2
    exit 2
fi

REPO_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# A port that corporate proxies do not intercept. On a machine behind such a
# proxy, 443 is frequently accepted for *any* address, which makes a blackhole
# look like an instant success.
UNROUTED_PORT=8081
CLOSED_PORT=9
LISTEN_PORT=18080

cat > "$WORK/probe.c" <<'PROBE_EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/socketio.h"
#include "azure_c_shared_utility/xio.h"
#include "azure_c_shared_utility/tickcounter.h"

typedef struct { int completed; IO_OPEN_RESULT result; int code; } PROBE_RESULT;

static void on_open_complete(void* context, IO_OPEN_RESULT_DETAILED r)
{
    PROBE_RESULT* p = (PROBE_RESULT*)context;
    p->completed = 1; p->result = r.result; p->code = r.code;
}
static void on_bytes(void* c, const unsigned char* b, size_t s) { (void)c; (void)b; (void)s; }
static void on_err(void* c) { (void)c; }

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: probe <host> <port> [repeat]\n"); return 2; }
    const char* host = argv[1];
    int port = atoi(argv[2]);
    int repeat = (argc > 3) ? atoi(argv[3]) : 1;
    if (repeat < 1) repeat = 1;
    if (platform_init() != 0) { printf("platform_init failed\n"); return 2; }

    int opened = 0;
    for (int i = 0; i < repeat; i++)
    {
        SOCKETIO_CONFIG cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.hostname = host; cfg.port = port;
        PROBE_RESULT p; memset(&p, 0, sizeof(p));

        XIO_HANDLE io = xio_create(socketio_get_interface_description(), &cfg);
        if (io == NULL) { printf("host=%s port=%d result=CREATE_FAILED\n", host, port); continue; }

        TICK_COUNTER_HANDLE tc = tickcounter_create();
        tickcounter_ms_t t0 = 0, t1 = 0;
        if (tc) tickcounter_get_current_ms(tc, &t0);
        (void)xio_open(io, on_open_complete, &p, on_bytes, &p, on_err, &p);
        if (tc) { tickcounter_get_current_ms(tc, &t1); tickcounter_destroy(tc); }

        int ok = p.completed && p.result == IO_OPEN_OK;
        if (ok) opened = 1;
        printf("host=%-28s port=%-6d result=%-10s code=%-4d elapsed_ms=%u\n",
            host, port, ok ? "OPEN_OK" : (p.completed ? "OPEN_ERROR" : "NO_CALLBACK"),
            p.code, (unsigned int)(t1 - t0));
        fflush(stdout);
        xio_close(io, NULL, NULL);
        xio_destroy(io);
    }
    platform_deinit();
    return opened ? 0 : 1;
}
PROBE_EOF

gcc -O2 -o "$WORK/probe" "$WORK/probe.c" \
    -I"$REPO_DIR/inc" \
    "$BUILD_DIR/libaziotsharedutil.a" -lssl -lcrypto -lpthread -lm -luuid -lcurl

run_probe() {
    # The adapters log through xlogging on stdout; only the probe's own summary
    # line is wanted here.
    timeout 60 "$WORK/probe" "$1" "$2" 2>/dev/null | grep '^host='
}

echo "=================================================================="
echo " single candidate"
echo "=================================================================="
echo "# blackholed: bounded by CONNECT_TIMEOUT_MS, not by the OS timeout"
run_probe 192.0.2.1 $UNROUTED_PORT
run_probe 203.0.113.1 $UNROUTED_PORT
echo "# refused: immediate, and must not consume the budget"
run_probe 127.0.0.1 $CLOSED_PORT
run_probe ::1 $CLOSED_PORT
echo "# IPv4-mapped literal: accepted on Linux, refused on Windows"
run_probe ::ffff:127.0.0.1 $CLOSED_PORT

echo
echo "=================================================================="
echo " preferred family blackholed, reachable IPv4 candidate behind it"
echo "=================================================================="
echo "# Blackholes are taken from the host's own IPv6 range so that they route"
echo "# but never answer. Override with BLACKHOLE_PREFIX if fd00:: is not yours."
BLACKHOLE_PREFIX="${BLACKHOLE_PREFIX:-fd00:c8de:3ffb:9999::}"

python3 -m http.server $LISTEN_PORT --bind 127.0.0.1 >/dev/null 2>&1 &
LISTENER=$!
trap 'kill $LISTENER 2>/dev/null; rm -rf "$WORK"' EXIT
sleep 2

md5sum /etc/hosts > "$WORK/hosts.before"

for count in 1 2 3; do
    {
        echo "127.0.0.1 localhost"
        i=1
        while [ $i -le $count ]; do
            echo "${BLACKHOLE_PREFIX}${i} connectmatrix.test"
            i=$((i + 1))
        done
        echo "127.0.0.1 connectmatrix.test"
    } > "$WORK/hosts.$count"

    echo "--- $count blackholed IPv6 + 1 reachable IPv4 ---"
    unshare -m sh -c "
        mount --bind '$WORK/hosts.$count' /etc/hosts
        printf 'candidate order: '
        getent ahosts connectmatrix.test | grep STREAM | awk '{printf \"%s \", \$1}'
        echo
        timeout 90 '$WORK/probe' connectmatrix.test $LISTEN_PORT 2>/dev/null | grep '^host='
    "
done

kill $LISTENER 2>/dev/null || true
trap 'rm -rf "$WORK"' EXIT

echo
if md5sum -c "$WORK/hosts.before" >/dev/null 2>&1; then
    echo "/etc/hosts unchanged: OK"
else
    echo "/etc/hosts CHANGED - investigate" >&2
    exit 1
fi
echo "leaked mount namespaces: $(grep -c "$WORK" /proc/mounts || true)"
