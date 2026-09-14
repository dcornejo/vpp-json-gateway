<!--
Copyright 2026 David Cornejo
SPDX-License-Identifier: Apache-2.0
-->

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

## Large messages (2026-09-14)

- Native VPP 26.06 test on dev-linux-1: uploaded and committed a parameter object
  containing a 300,000-byte string, resolved it via `params_ref`, and dispatched
  the generated interface dump through VAPI successfully. The original list,
  mutation, replay, 3,001-interface dump, crash and restart tests also passed.
  Evidence: `validation/large-vpp.log`.
- Live testing exposed short writes in VAPI's nonblocking Unix socket send path.
  Generated calls now temporarily use a five-second blocking send timeout, then
  restore socket settings. Failed/partial submissions remain outcome-unknown.
- Redis regression: a 1 MiB whitespace-padded valid parameter object is consumed
  successfully; the C++ client reassembles a 2.1 MB Unicode response delivered by
  a protocol fixture through more than one 32-frame Redis window.
- Large-message unit tests validate actual spool fragmentation, exact Unicode
  and escaped-content reconstruction, missing fragments, malformed base64,
  checksum failure, partial-record quota failure, uploaded parameters, and the
  independent native payload limit. All three test suites pass under ASan/UBSan.
- Google-style cpplint and whitespace checks pass.

Limits are intentional: 16 MiB per serialized logical response or uploaded JSON
parameter object, 1 MiB per encoded native payload, and existing spool/upload
quotas. These tests do not establish arbitrary-size VPP messages or variable
native reply support. Individual JSON values are materialized in memory.

## Expanded API coverage (2026-09-14)

The default interface/vlib/ip catalog supports 85 of 93 services. Native builds
against VPP 26.06 headers pass, including payload-size assertions. Evidence:
`validation/coverage-vpp.log`.

Live tests exercise `show_threads` with a variable thread array, IP table
creation/dump/deletion, boolean schema defaults, explicit TX stream completion
errors, and rejection of caller-supplied cursors. All prior live interface,
large-parameter, crash, replay and restart checks also pass.

The Linux generated-callback test covers zero-length ordinary replies, variable
TX arrays, internal array counts, separate detail/completion callbacks,
continuation status retention, and rejecting an oversized count before copying.
All four Linux tests pass; the three portable suites pass under ASan/UBSan.

The fixture has no hardware TX queues. Successful multi-page hardware traversal
is implemented but not live-validated; its generated detail/completion callbacks
are tested with synthetic native payloads. Event subscriptions and nested
variable reply layouts remain explicitly unsupported. Coverage counts describe
supported bindings, not exhaustive behavioral validation of every operation.

## Nested variable replies (2026-09-14)

Support expands to 91 of 93 default services through recursive sizing of
trailing scalar structs containing variable arrays/strings. Live tests create
IPv4 and IPv6 routes in disposable tables, validate route dump/lookup path arrays,
check next-hop union selection against the enclosing protocol, and remove the
routes/tables. The existing full live suite also passes. Evidence:
`validation/nested-vpp.log`.

Native callback tests check both empty and one-element nested route arrays.
Portable sanitizer suites pass. Variable-sized array elements and non-trailing
variable layouts remain unsupported; non-IP FIB next hops are explicitly
rejected. The six newly supported layouts are not all behaviorally tested:
live coverage focuses on route dumps and lookups, not multicast/punt behavior.
