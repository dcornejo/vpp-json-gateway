# Validation record

Implementation target: VPP 26.06, Ubuntu 26.04.
Initial portable validation: macOS / Apple Clang.
Live target validation: Ubuntu 26.04.1 / GCC 15.2.0 on `dev-linux-1`.

## Passed

- CMake C++20 debug build of gateway, C++ client and core tests, with
  `-Wall -Wextra -Wpedantic -fno-exceptions`; no compiler warnings.
- Core tests: envelope validation, nesting rejection, SHA-256 known vector,
  symbolic interface changes, bounded spooling/error termination, upload
  sequence checking, checksum rejection, session isolation, committed parameter
  resolution, and durable journal/transfer reopening.
- Core tests rebuilt and run with AddressSanitizer and UndefinedBehaviorSanitizer.
- Real Redis 7.4.2 integration tests, using the mock VPP backend:
  - Exclusive namespace ownership across separate state directories.
  - Registration and capability discovery for two clients.
  - Symbolic mutations and invalid parameters/resources.
  - Retained response replay without repeating the original mutation.
  - 3,000-record dump exceeding one 256 KiB frame, contiguous sequence numbers
    and explicit completion.
  - A paused client stays at 32 unacknowledged frames while another client
    receives its response.
  - A 1 MiB upload in bounded chunks, commit/checksum verification, private
    transfer namespaces and the adapter's separate parameter-size limit.
  - Small committed JSON parameters applied through `params_ref`.
  - Conflicting request IDs and malformed envelopes.
  - Recovery from a persisted `running` marker returns `outcome_unknown`;
    committed uploads survive server restart.
  - C++ client registration and retry under the same request ID.
- `clang-format --dry-run --Werror` using the checked-in Google configuration.
- `cpplint` with repository-relative header guards; only the copyright-header
  rule was disabled because no copyright owner/license was supplied.

## Live VPP validation — passed on September 11, 2026

Host: `dev-linux-1`, Ubuntu 26.04.1 LTS, x86-64.
Compiler: GCC/G++ 15.2.0 (Ubuntu 15.2.0-16ubuntu1).
Libraries: hiredis 1.2.0, SQLite 3.46.1, OpenSSL 3.5.5.
Test Redis: 7.4.2, built into the isolated validation directory.

VPP source commit: `c3200b88dc46bd380f00a49ca3392a102cc1980b`, confirmed to
be the commit referenced by the annotated `v26.06` release tag. The running
instance reports `vpp v26.06-release`. VPP was built in C11 mode because GCC
15's default C23 mode rejects a const conversion in this VPP source. No VPP
source-code fix was needed. The C/C++ development components were installed
under `/home/dave/vpp-json-validation/vpp-install`, not system-wide.

The gateway compiled and linked with `-DWITH_VAPI=ON` against that installation,
with no compiler warnings. Linux core tests and the full Redis regression suite
also passed. Runtime application code remains C++20.

The first live call caught and led to a fix for VAPI's reserved request slot:
`vapi_get_max_request_count()` returns `requests_size - 1`. Allocating one slot
made every generated operation return `VAPI_EAGAIN`. The adapter now allocates
two internal slots while retaining one operation in flight. The successful live
listing is the regression check for this failure.

Passed live scenarios:

- Enumerated `local0` and a newly created loopback through JSON/VAPI.
- Set `loop0` up and down, independently confirming each state with `vppctl`.
- Replayed the earlier successful up response after setting the interface down;
  the interface stayed down, proving replay did not reexecute the mutation.
- Returned `resource_not_found` for an unknown interface name.
- Created 3,000 loopbacks in the disposable VPP instance and received all 3,001
  interface records, with contiguous sequences and a final completion frame.
  Encoded output exceeded one 256 KiB frame. A paused consumer was limited to
  32 outstanding frames while another client obtained its response.
- Paused the disposable VPP, killed the gateway after its durable running marker
  appeared, resumed VPP and restarted the gateway. The request recovered as
  `outcome_unknown`; the mutation was not automatically replayed.
- Stopped VPP, observed a symbolic backend error, started a fresh instance and
  created `loop9000`. The same gateway recovered its connection, listed only
  the fresh interfaces, rejected the old name, and successfully changed the
  new interface's state (confirmed through the CLI).

The first large fixture exhausted its deliberately small 16 MiB statistics
segment while creating interfaces. The successful fixture uses 128 MiB for
statistics and a 512 MiB main heap. This was a fixture resource adjustment,
not a gateway change.

All disposable VPP, gateway and Redis processes were stopped after the tests.
No host network interfaces were used. Build artifacts and logs remain under
`/home/dave/vpp-json-validation` for reproduction.

Evidence:

- `validation/live-vpp.log`: successful live run.
- `validation/linux-redis.log`: Linux Redis regression results.
- `tests/live_vpp_test.py`: repeatable live harness.

## Not covered

Production Redis ACL selectors and persistence-loss behavior, active-active
failover, concurrent interface lifecycle changes by other VPP clients, hardware
packet processing, and sustained production load. These tests establish the
implemented interface operations and recovery behavior on the requested VPP/OS
combination; they do not establish universal VPP API coverage. The mock backend's
interface state is not durable across restart; the gateway journal and uploads
are durable.

## Generated bindings validation

The default VPP 26.06 interface catalog generates 35 of 37 services. Event
subscriptions and explicit TX-placement streaming are reported unsupported.
The generated native wrappers compile on dev-linux-1 against the installed VPP
headers, with static checks of packed payload sizes.

The extended live harness verifies generated method discovery, loopback
creation returning a symbolic name, flag mutation independently checked through
VPP CLI, rejection of numeric interface/flag parameters, CIDR address assignment,
symbolic interface dump and loopback deletion. The existing 3,001-interface
stream, crash/replay and VPP restart checks also pass with the generated adapter.
Evidence is in `validation/generated-vpp.log`.

Portable core and schema tests pass with AddressSanitizer and
UndefinedBehaviorSanitizer. Codec checks cover derived array counts, truncation,
integer overflow, exact 64-bit decimal strings, flags, MAC addresses and interface
sentinels. The Redis integration suite and Google-style cpplint checks pass.
Other selectable VPP modules have not received live validation.
