# Connect matrix

`run_connect_matrix.sh` drives `socketio_open` against real destinations and
reports the platform error code and the elapsed time for each attempt.

## Why this exists alongside the unit tests

`tests/socketio_win32_ut` mocks `getaddrinfo`, `connect` and `select`, so it can
assert exactly which candidates the connect loop attempts and in which order.
That is the right place for the logic.

What it cannot show is the part that is a *timing* behaviour:

- that a genuinely blackholed address is abandoned when its per-address grant expires,
  rather than running to the OS timeout - about 21 s on Windows;
- that an immediate refusal really does move to the next candidate immediately;
- that a reachable candidate sitting behind blackholed ones is still reached.

This harness measures those directly, on a real stack.

## Running

```
cmake <repo> -DCMAKE_BUILD_TYPE=Release -Dskip_samples=ON -Duse_openssl=ON -Drun_unittests=OFF
make aziotsharedutil
tests/manual/connect_matrix/run_connect_matrix.sh <build-dir>
```

Needs `gcc`, `python3` and `unshare`. The multi-address cases supply a private
`/etc/hosts` inside a mount namespace, so the host's own `/etc/hosts` is never
written; the script verifies its checksum afterwards and reports any leaked
mount.

Set `BLACKHOLE_PREFIX` if `fd00:c8de:3ffb:9999::` is not inside a range your
host routes. The addresses must *route* and not answer - an address that is
rejected with `ENETUNREACH` returns immediately and does not exercise the
budget at all.

Pick a port that is not intercepted. Behind a corporate proxy, 443 is often
accepted for any address, which makes a blackhole look like an instant success.
The script uses 8081.

## Measured behaviour on this branch

Measured on Ubuntu 24.04, from `09560c1bcb` ("Ask for AI_ADDRCONFIG in the
Berkeley resolver hint"), on two hosts with opposite address-family preference.
Identical results on both.

Single candidate:

```
host=192.0.2.1          port=8081  result=OPEN_ERROR code=110  elapsed_ms=10000
host=203.0.113.1        port=8081  result=OPEN_ERROR code=110  elapsed_ms=10000
host=127.0.0.1          port=9     result=OPEN_ERROR code=111  elapsed_ms=0
host=::1                port=9     result=OPEN_ERROR code=111  elapsed_ms=0
host=::ffff:127.0.0.1   port=9     result=OPEN_ERROR code=111  elapsed_ms=0
```

`110` is `ETIMEDOUT`, `111` is `ECONNREFUSED`. The blackholes stop at exactly
`CONNECT_TIMEOUT_PER_ADDRESS_MS`, which matches what `socketio_win32.c` now
does - Windows measured 10002 ms against 21075 ms for a plain blocking connect
to the same address. The bound is at parity across the two adapters.

`::ffff:127.0.0.1` is *accepted* here and reaches the IPv4 stack: against a port
with a listener it returns `OPEN_OK`. Both adapters now clear `IPV6_V6ONLY` for
AF_INET6 sockets, so Windows and Linux have the same intended behavior.

Preferred family blackholed, with a reachable IPv4 candidate behind it:

| Blackholed IPv6 candidates | Result | IPv4 attempted |
| --- | --- | --- |
| 1 | `OPEN_OK` after about 10000 ms | yes |
| 2 | `OPEN_OK` after about 20000 ms | yes |
| 3 | `OPEN_OK` after about 30000 ms | yes |

Every candidate receives the full `CONNECT_TIMEOUT_PER_ADDRESS_MS` grant. The
total can therefore grow to approximately the number of timed-out candidates
multiplied by 10 seconds before a later healthy candidate succeeds.

This is sequential candidate fallback, not a shared family budget or Happy
Eyeballs race. The opt-in IPv6 setting controls hostname resolution: disabled
uses `AF_INET` for every host form, including an explicit IPv6 literal, and
enabled uses `AF_UNSPEC`.

## Note on running the unit tests here

The Berkeley and Windows unit suites cover the opt-in resolver family,
including explicit IPv6 literals. They also cover IPv4 fallback, per-address
grants, refusal, cleanup, and `IPV6_V6ONLY` where applicable.
