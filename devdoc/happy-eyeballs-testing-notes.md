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

The feature-enabled Berkeley path now has an **uncommitted**, staggered
asynchronous-open flow:

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

- Closing during `IO_STATE_OPENING` now reports exactly one
  `IO_OPEN_CANCELLED` callback and leaves no stale open callback for later
  `socketio_dowork` calls.
- A failed asynchronous open now returns the handle to the closed state, so the
  same handle supports an immediate reopen without an intervening close.
- A recorded successful attempt now completes before deadline evaluation, so
  success is not changed into a timeout when work runs at or past the deadline.
- Closing or destroying a handle with multiple active attempts closes every
  attempt
  descriptor. Close reports exactly one cancellation callback, destroy reports
  none, and later work cannot produce a stale callback.

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
until polling at or after the deadline is rejected. Cancellation with
exactly-once/no-stale-callback behavior and immediate retry after asynchronous
failure remain persistently covered as well.

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

The following coverage remains pending:

- live real-network stress and controlled dual-stack black-hole integration;
  and
- a specific unit test for the interface-binding failure path.

This is not a claim of full project completion or complete RFC 8305 support.
Carbon wiring, the broader integration matrix, and broader documentation remain
pending.
