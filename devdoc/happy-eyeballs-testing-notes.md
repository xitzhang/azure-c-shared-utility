# Happy Eyeballs Testing Notes

## Candidate ordering

Think of resolved addresses as a connection lineup. The operating system picks who
goes first, and the helper keeps that preference while alternating address
families when possible:

- IPv6-first input `v6a, v6b, v4a, v4b` becomes
  `v6a, v4a, v6b, v4b`.
- IPv4-first input `v4a, v4b, v6a, v6b` becomes
  `v4a, v6a, v4b, v6b`.

In short: the OS still gets first pick, then IPv4 and IPv6 take turns—no family
cuts the line. This ordering only prepares the lineup; it does **not** by itself
race connection attempts.

## Candidate-ordering mini-test

The mini-test included the production `adapters/socketio_berkeley.c` helper
directly in the same translation unit. It was built and run on the Linux
`client-vm` with `enable_dual_stack_connection_racing=ON` and AddressSanitizer
enabled.

| Case | Input | Expected and observed result | Status |
| --- | --- | --- | --- |
| IPv6-first | `v6a, v6b, v4a, v4b` | `v6a, v4a, v6b, v4b` | Pass |
| IPv4-first | `v4a, v4b, v6a, v6b` | `v4a, v6a, v4b, v6b` | Pass |
| IPv6-only | `v6a, v6b` | `v6a, v6b` | Pass |
| IPv4-only | `v4a, v4b` | `v4a, v4b` | Pass |
| Uneven families | `v6a, v6b, v6c, v4a` | `v6a, v4a, v6b, v6c` | Pass |
| Invalid/non-IP skipped | `invalid, v6a, non-IP, v4a` | `v6a, v4a`; unsupported families and entries without an address are skipped | Pass |
| Zero usable candidates | `invalid, non-IP` | Candidate preparation fails cleanly | Pass |

All mini-test cases passed, with no AddressSanitizer findings.

## Current asynchronous-open implementation record

The feature-enabled Berkeley path has a staggered asynchronous-open flow:

- RFC 8305 does not mandate a two-attempt cap. The implementation dynamically
  allocates attempt and poll state for the complete ordered candidate list.
- The first ordered candidate starts immediately.
- Later candidates start one per eligible `socketio_dowork` call at actual
  250 ms intervals. A delayed work call does not burst-start missed intervals.
- Pending attempts remain active while later candidates start, so a third or
  subsequent candidate can win.
- `socketio_dowork` uses nonblocking, zero-timeout `poll` across all active
  sockets and reads completion through `SO_ERROR`.
- If multiple attempts succeed together, the lowest candidate-order attempt
  wins deterministically; all loser descriptors are closed.
- The overall connection deadline uses `CLOCK_MONOTONIC`.

A success recorded before a `socketio_dowork` call completes before deadline
evaluation. In contrast, a success first observed by polling at or after the
deadline is rejected as `ETIMEDOUT`.

### Completed async-open fixes

- Closing Berkeley `socketio` during `IO_STATE_OPENING` cleans up the race and
  drops the pending leaf open callback. It does not report
  `IO_OPEN_CANCELLED`; this matches the current upstream Berkeley close
  contract and prevents layered callers from receiving duplicate completion.
- Closing TLS-over-Berkeley during `IO_STATE_OPENING` reports exactly one
  application-level `IO_OPEN_CANCELLED` from the TLS layer. The Berkeley layer
  performs only its cleanup and close completion, and later
  `socketio_dowork` calls cannot emit a stale open callback.
- A failed asynchronous open now returns the handle to the closed state, so the
  same handle supports an immediate reopen without an intervening close.
- A recorded successful attempt now completes before deadline evaluation, so
  success is not changed into a timeout when work runs at or past the deadline.
- Closing or destroying a Berkeley handle with multiple active attempts closes
  every attempt descriptor. Direct Berkeley close and destroy report no open
  cancellation callback, and later work cannot produce a stale callback.

### Persistent deterministic regression target

The feature-ON-only target
`tests/socketio_berkeley_async_open_ut` compiles the production Berkeley
adapter into the test translation unit and uses deterministic hooks. Its
coverage retains the earlier cancellation, reopen, deadline, and ordering
checks and verifies:

- four pending candidates start at 0, 250, 500, and 750 ms and reach four
  simultaneously active attempts;
- the third candidate can win before the fourth starts;
- a large time jump starts only one new attempt per eligible work call, without
  a burst;
- an immediately failed attempt still leaves the next candidate subject to the
  250 ms stagger;
- simultaneous completion of more than two attempts selects the lowest
  candidate index and closes every loser;
- timeout, cancellation, and destroy clean up more than two active attempts
  without duplicate or stale callbacks;
- all candidates failing produces exactly one final open failure;
- candidate counts, allocation sizes, and poll-count conversions are checked
  safely; and
- dynamic allocation failures are independently injected at allocation calls
  1 through 4, each ending with zero outstanding allocations and zero invalid
  frees.

The target also retains the late-success-at-deadline regression: an already
recorded success wins before deadline evaluation, while success not observed
until polling at or after the deadline is rejected. Direct Berkeley close
verifies zero open callbacks and no stale callback. When OpenSSL is available,
the same target composes the production TLS and Berkeley layers and verifies
that closing TLS while Berkeley is opening reports exactly one
application-level `IO_OPEN_CANCELLED`. Immediate retry after asynchronous
failure remains persistently covered as well.

## Linux build and configuration checks

| Configuration | Result |
| --- | --- |
| `enable_dual_stack_connection_racing=OFF` | `aziotsharedutil` compiled successfully |
| `enable_dual_stack_connection_racing=ON`, `use_socketio=ON` | `aziotsharedutil` compiled successfully |
| `enable_dual_stack_connection_racing=ON`, `use_socketio=OFF` | Configuration rejected as intended because the feature requires socket support |

## Final Linux validation

Standard validation on the Linux `client-vm` produced these results:

- The plain feature-ON Linux build passed.
- The feature-ON targeted CTest passed: 1/1 test.
- The AddressSanitizer and LeakSanitizer targeted run passed: 1/1 test.
- A plain feature-OFF build passed with the race test target absent.
- `git diff --check` passed.

An independent review found no runtime defect. It identified a gap in allocation
failure coverage, which was fixed by the four independent injection cases
described above.

## Critical deterministic gate before the Carbon commit

These checks exercise the current implementation without requiring controlled
DNS, routing, or public endpoints. They are the minimum local/CI gate before
moving the Carbon pin forward:

- build the utility with racing enabled and run
  `socketio_berkeley_async_open_ut`;
- run the same Berkeley target with AddressSanitizer and LeakSanitizer;
- build the utility with racing disabled and verify that the feature-only
  Berkeley target is absent;
- run `httpapi_curl_dual_stack_ut` in curl-backed configurations;
- configure and build Carbon with the pinned utility in external-default,
  external-OpenSSL-3, inline, and explicit-opt-out configurations;
- verify that both Carbon external utility variants receive
  `enable_dual_stack_connection_racing=ON` by default on Linux;
- verify that the inline build inherits the Linux default and that explicit
  opt-out removes `DUAL_STACK_CONNECTION_RACING_ENABLED`; and
- run the existing Carbon PAL/networking CTests discovered in those
  configurations, with explicit native exit codes.

Before committing, `git diff --check` must pass in the utility repository and
`git diff --cached --check` must pass in Carbon. Carbon must contain only its
intended CMake wiring and the exact utility gitlink.

### Additional critical unit tests completed

The deterministic Berkeley target now covers these racer error paths:

| Test | Assertions covered |
| --- | --- |
| Initial monotonic-clock failure | Open fails synchronously with the clock error; DNS, candidate, attempt, and poll allocations are released; no descriptor or saved callback remains; immediate retry succeeds. |
| Clock failure during `socketio_dowork` | More than one active descriptor closes; one open-error callback reports the clock error; all race allocations are released; later work emits no callback; immediate retry is possible. |
| Candidate socket creation failure | No invalid descriptor is closed; the next candidate remains paced by 250 ms and can win; exhaustion reports the final meaningful socket error once. |
| Nonblocking `fcntl` failure | An owned descriptor closes exactly once; the next candidate remains paced by 250 ms and can win; exhaustion reports a meaningful error even if `errno` was zero. |
| `poll` system-call failure | Readiness is not interpreted after the failed call; the race reports one open error, closes all attempts, releases all allocations, and emits no stale callback. |
| `getsockopt(SO_ERROR)` system-call failure | Only the affected attempt fails; another active attempt can win; exhaustion reports the syscall error once. |
| Configured interface-binding failure | The test enters the configured-interface path; the failed descriptor closes, the later candidate observes 250 ms pacing and can win, and no callback or allocation leaks. |
| Resolver success with a null address list | Open fails cleanly without calling `freeaddrinfo(NULL)`, starting a socket, or retaining callback/race state; immediate retry succeeds. |

Tests should use the existing deterministic hooks, track descriptor ownership
and race allocations, and finish by asserting no outstanding allocations,
invalid frees, duplicate closes, or stale callbacks. Production hooks must
remain inert outside the focused test target.

### Current Carbon test status

The four Linux Carbon PAL build configurations have passed with the utility
gitlink at `06df39501dfc65fbfba1c23a4e4f195d02069c24`.

Carbon registers no CTest entries for this surface, so its focused Catch2
executable was run directly with tests enabled:

| Configuration | Focused tests | Result |
| --- | --- | --- |
| External utility, racing ON | `WebSocket DNS lookup failed`; `HTTP DNS lookup failed` | 2 cases, 17 assertions passed |
| Inline utility, racing ON | `WebSocket DNS lookup failed`; `HTTP DNS lookup failed` | 2 cases, 17 assertions passed |
| External utility, explicit opt-out | `WebSocket DNS lookup failed`; `HTTP DNS lookup failed` | 2 cases, 17 assertions passed |

The external racing-ON build compiled both normal and OpenSSL 3 utility/PAL
variants, and the OpenSSL 3 PAL was confirmed loaded by the focused run.
Configuration caches and compile commands confirmed the racing definition is
present in both default external variants and the inline build, and absent from
both external variants under explicit opt-out.

The local-server `basic` tests were not run because restoring
`test_server.dll` from the required private feed returned HTTP 401. This is an
environment/access blocker rather than a test failure and must not be recorded
as passing.

### Final deterministic error-path validation

On the Linux `client-vm`:

- the feature-ON plain build passed;
- `socketio_berkeley_async_open_ut` passed 1/1;
- the AddressSanitizer and LeakSanitizer run passed 1/1;
- the curl regression tests passed 2/2;
- the feature-OFF build passed with the Berkeley racer target absent; and
- `git diff --check` passed.

Eleven new cases cover the error paths summarized above. Real interface
enumeration remains mocked; the test exercises the configured binding branch
and its racer ownership/pacing behavior through an inert production hook.

## HTTPAPI/libcurl guarantee

With `enable_dual_stack_connection_racing=ON`, the curl HTTPAPI build requires
the selected target libcurl to be version 7.59.0 or newer.

- Native discovery through pkg-config or `find_package`, and a prepopulated
  `CURL_FOUND`, are each independently checked against the selected headers'
  `LIBCURL_VERSION`.
- Cross-compilation uses `find_path` and `find_library` under the toolchain's
  target-root rules. It does not invoke host pkg-config or `FindCURL`.
- On the first `HTTPAPI_Init`, runtime validation rejects a `NULL`
  `curl_version_info` result, libcurl older than 7.59.0, or a runtime without
  `CURL_VERSION_IPV6`. Every rejection balances global cleanup and leaves the
  HTTPAPI reference count at zero.
- `HTTPAPI_CreateConnection` sets
  `CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS` to `250L`. It does not set
  `CURLOPT_IPRESOLVE`, so libcurl remains free to use both address families.
- Failure to set the timeout fully cleans the curl easy handle and allocated
  connection state.

### Persistent focused curl tests

The feature-ON-focused target `tests/httpapi_curl_dual_stack_ut` directly
exercises:

- runtime rejection for `NULL` version data, runtime libcurl older than 7.59.0,
  and missing `CURL_VERSION_IPV6`;
- successful first-user validation and balanced multi-user reference counting;
- the `250L` Happy Eyeballs timeout option;
- complete cleanup after `curl_easy_setopt` failure; and
- the absence of `CURLOPT_IPRESOLVE`.

Its CMake validation covers rejection of preseeded 7.58 headers, acceptance of
preseeded 7.59 headers, target sysroot header/library selection while cross
compiling, and proof that the fake host pkg-config executable was not invoked.

### Curl validation on `client-vm`

- The native feature-ON build and curl CTests passed.
- A curl-only feature-ON build with `use_socketio=OFF` and its CTests passed.
- The feature-OFF build passed using legacy, unversioned curl discovery.
- The unsupported no-path negative test passed by rejecting the configuration
  as expected.
- Compile-time and runtime libcurl were both 8.5.0 with IPv6 support.
- `git diff --check` passed.

An independent final review reported no high-confidence findings.

## Remaining integration coverage

The following environment-dependent coverage remains pending and may be
outsourced after the deterministic gate:

- a dual A+AAAA hostname with both families healthy;
- an IPv6-preferred result where IPv6 is black-holed and IPv4 succeeds;
- an IPv4-preferred result where IPv4 is black-holed and IPv6 succeeds;
- forced IPv4-first and IPv6-first resolver ordering;
- several same-family addresses before and after the alternate-family address;
- many DNS addresses to exercise multiple paced active sockets;
- NAT64/DNS64;
- a live curl-backed fallback case; and
- repeated connection/cancellation stress with descriptor and memory
  monitoring.

Each live case must record resolved order, attempt start times, winning family,
elapsed connection time, callback count/result, descriptor cleanup, and the
equivalent feature-OFF control result.

This is not a claim of full project completion or complete RFC 8305 support.
The broader integration matrix and rollout documentation remain pending.
