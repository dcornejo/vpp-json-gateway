# VPP JSON gateway

A working first implementation in C++20, using the Google C++ Style Guide.
The live adapter targets **VPP 26.06 on Ubuntu 26.04**. The portable build
includes a deterministic mock backend and a C++ command-line client.

Implemented: client registration, two Redis Streams per session, symbolic
interface listing/state changes, durable request deduplication, explicit replay,
bounded disk-backed responses, resumable uploads with SHA-256 verification, and
session expiration. VAPI has one owner thread and one operation in flight.

This is the initial interface-oriented implementation, not a universal binding
for every VPP API. Live execution has been validated against VPP 26.06 on Ubuntu 26.04.1
on `dev-linux-1`, using a disposable instance and loopback interfaces. The mock is explicitly selected; the server never silently falls
back to it.

## Build and run

Dependencies: CMake 3.22+, a C++20 compiler, hiredis, SQLite 3, OpenSSL,
nlohmann/json 3.10+, and a standalone Redis server. Python 3 is needed only for
the integration test harness. Runtime application code and the client are C++.

On Ubuntu, install the ordinary build dependencies:

```sh
sudo apt-get install build-essential cmake pkg-config libhiredis-dev \
  libsqlite3-dev libssl-dev nlohmann-json3-dev redis-server python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Create `clients.json` with an independently generated secret of at least 32
characters for each client. This example value is deliberately a placeholder:

```json
{"demo":"REPLACE_WITH_AN_INDEPENDENT_RANDOM_SECRET"}
```

```sh
chmod 600 clients.json
./build/vpp-json-gateway --backend mock --mock-count 100 \
  --clients clients.json --state ./state
```

In another terminal, set `GATEWAY_TOKEN` to the same secret:

```sh
./build/gateway-client --client demo --session ./session.json \
  --method interface.list
./build/gateway-client --client demo --session ./session.json \
  --method interface.set_state --params '{"interface":"loop1","state":"up"}'
```

The client prints JSON frames to stdout and its request ID to stderr. Use
`--request-id` with the same session and unchanged parameters to retrieve a
retained result after interruption. Use one CLI process per session; this
sample client acknowledges and discards frames for other request IDs. A general
multiplexing client must route all responses to their respective consumers.

Use `--redis-host`, `--redis-port` and `--namespace` consistently on server and
client. `REDIS_USERNAME` and `REDIS_PASSWORD` enable Redis authentication.
The current hiredis wrapper uses TCP without TLS; use loopback or a protected
local tunnel. The application registration secret is separate from Redis ACL
credentials.

## VPP 26.06 / Ubuntu 26.04

Install or build VPP **26.06** and its matching development files for your
Ubuntu 26.04 host. The adapter needs `vapi/interface.api.vapi.h`, the included
VAPI/VPP headers, and `libvapiclient`. Do not mix headers from another VPP
release or substitute a different Ubuntu release's packages without validating
the resulting build. Validation used a source build of the `v26.06` release tag. Distribution
package availability was not required or validated.

```sh
cmake -S . -B build-vpp -DCMAKE_BUILD_TYPE=Release -DWITH_VAPI=ON
cmake --build build-vpp -j
./build-vpp/vpp-json-gateway --clients clients.json --state ./vpp-state \
  --backend vapi --vpp-socket /run/vpp/api.sock
```

For a nonstandard VPP installation, pass `CMAKE_PREFIX_PATH` or
`VAPI_INCLUDE_DIR` and `VAPI_LIBRARY` to CMake. `VPP_API_DIR` locates installed `.api.json` files. Enable VPP's socket API, for
example `socksvr { socket-name /run/vpp/api.sock }`, and grant the gateway user
access to that socket.

The adapter uses VPP's generated C bindings from C++, with RAII for the context.
The bindings handle numeric message IDs, network byte order, and dump completion
via control ping. VAPI 26.06 reserves a queue slot internally, so the adapter
allocates two slots to allow exactly one outstanding operation. Message availability is checked before dispatch. Public
operations include `interface.list`, `interface.set_state`, and generated
`vpp.<message_name>` methods. VAPI message IDs, context, byte order and array
counts stay internal. Interface-index types use names; enums and flags use symbols.

Names are resolved afresh before a state change. **A delete/recreate or rename
by another control-plane client between the lookup and update can still race.**
VPP does not supply an atomic name-based set operation here. Coordinate
interface lifecycle changes through a single owner if identity must remain
stable across that pair of calls. Dumps are not advertised as snapshots.

## Generated VPP methods

The C++ build-time generator reads the installed `interface.api.json`, emits
native VAPI wrappers and embeds the matching JSON catalog. The VPP 26.06
interface module has 37 services: 35 supported generated methods, plus two
explicitly unsupported services (event subscriptions and TX-placement streaming).
Compilation checks native payload sizes against schema sizes; runtime checks
verify that VPP offers the matching message schemas. The mock backend does not
provide generated methods.

Use `api.methods` with empty parameters to stream the catalog, including
`supported`, live `available`, and a reason for unsupported services. Use
`api.describe` with `{"method":"vpp.sw_interface_set_flags"}` for parameter and
reply schemas. Empty-parameter `api.describe` retains the gateway overview.

Example request parameters:

```json
{"id":"set-1","method":"vpp.sw_interface_set_flags","params":{"sw_if_index":"loop0","flags":["IF_STATUS_API_FLAG_ADMIN_UP"]}}
```

```json
{"id":"address-1","method":"vpp.sw_interface_add_del_address","params":{"sw_if_index":"loop0","is_add":true,"del_all":false,"prefix":"192.0.2.1/24"}}
```

Interface-index aliases accept interface names or the `@all` sentinel. IP/MAC
addresses and CIDR prefixes are strings, flags are arrays of names, and enums
are names. All 64-bit integers use decimal strings to preserve precision.
Array counts are derived and omitted from JSON. Other numeric fields retain
their schema meaning; untyped numeric resource identifiers are not automatically
converted to names. The friendly interface methods remain the simplest API.

Requests use generated VAPI endian conversion and dispatch. Dumps stream details
through the existing bounded spool. Successful ordinary replies omit `retval`;
VPP failures become `vpp_rejected`. Interface names are refreshed after replies
that return interface indexes, allowing creation to return the new name.
Generated calls are conservatively reported as `outcome_unknown` on transport
failure and are never automatically retried.

`VPP_API_MODULES` is a CMake list (default `interface`). Additional installed
modules can be selected, but only the default module has live validation here.
Variable-length replies, ambiguous unions, event subscriptions, and services
with a separate streaming completion message are rejected by generation and
identified in discovery. New layouts require adapter work and validation.

## Wire protocol

Each Redis Stream entry has one field, `json`, containing one UTF-8 JSON frame.
Maximum encoded JSON size is 262,144 bytes. Unknown envelope fields, deeply
nested JSON, numeric method names, and non-object parameters are rejected.

Registration is sent to `vpp:register`:

```json
{"id":"registration-1","method":"session.register","params":{"client":"demo","token":"...","protocol":1}}
```

Its response appears on the provisioned private bootstrap stream
`vpp:bootstrap:demo`. Repeating the registration ID during the session lifetime
returns the same session. The registration response contains capabilities,
lease expiry, and these assigned streams:

```text
vpp:session:{demo:<session-id>}:requests
vpp:session:{demo:<session-id>}:responses
```

The client name in the key permits Redis ACL isolation before a session is
created. Both directions share a Redis hash tag, but this implementation connects
to standalone Redis and does not implement Cluster redirects.

A normal call and reply:

```json
{"id":"request-1","method":"interface.set_state","params":{"interface":"loop1","state":"up"}}
{"id":"request-1","type":"result","seq":0,"result":{}}
```

Interface listing sends one complete record per chunk, then one terminal frame:

```json
{"id":"request-2","type":"chunk","seq":0,"items":[{"interface":"loop1","state":"up"}]}
{"id":"request-2","type":"complete","seq":1,"chunks":1,"items":1}
```

An empty dump has `complete`, `seq:0`, `chunks:0`, `items:0`. Do not infer
completion from a pause, timeout, socket closure, or a full delivery window.
Every dump is spooled before delivery in this version, so it does not expose
records before VPP finishes the operation. The file is read incrementally;
the complete result is never accumulated in memory.

Errors have symbolic codes:

```json
{"id":"request-3","type":"error","seq":0,"error":{"code":"resource_not_found","message":"Interface name did not resolve"}}
```

The `state` enum is `up` or `down`. Other supported methods:

- `api.describe`: no parameters; returns capabilities and limits.
- `session.renew`: no parameters; extends the lease by one hour.
- `api.replay`: `{"request":"original-request-id"}`; resets delivery of retained
  output without executing the operation. The frames use the original request
  ID. This is a delivery-control message and has no separate success reply.
  An unknown original ID produces an error addressed to the replay message ID.
- `transfer.begin`, `transfer.chunk`, `transfer.commit`, `transfer.status`:
  described below.

Requests are admitted round-robin, one per session per server turn. Admitted
jobs are executed in journal order. This provides a simple admission policy,
not strict latency fairness: a long VPP operation delays subsequent operations.
Ready response files are delivered independently of the VAPI worker.

## Acknowledgment, replay and durability

Read responses using `XRANGE` or `XREAD`. After processing an entry, acknowledge
it with `XDEL` on the assigned response stream and Redis entry ID. This version
uses explicit ownership and deletion rather than Redis consumer groups. There
must be one logical response consumer per session.

At most **32 unacknowledged frames** are published to each response stream.
Publication and its window check are one Lua operation. No approximate trimming
is used. Redis disconnection or a full window does not block VAPI: the worker
finishes into its bounded spool while the Redis thread retries separately.

A request is journaled before removal from Redis and before VAPI dispatch.
Submitting the same ID and canonical JSON again does not execute it again;
changing the body under that ID returns `id_conflict`. Use `api.replay` to
retrieve output already published. Delivery can duplicate a frame if a crash
occurs between Redis publication and the local delivery-offset update. Deduplicate
using `(session, request ID, seq)`, never the Redis entry ID. Replay starts at
sequence zero; discard already processed sequence numbers.

SQLite uses WAL and FULL synchronization. Completed responses are fsynced before
their journal state is marked complete. On restart, an unfinished running job
becomes `outcome_unknown`; queued jobs can still execute. A missing retained
response is also reported conservatively as unknown. This intentionally does
not claim exactly-once VPP execution. Do not automatically retry a mutation
under a new ID after an uncertain result; inspect and reconcile VPP state.

Request IDs and results are retained for the session lifetime. The session has
128 retained ordinary requests; uploads' bulk chunks are handled separately.
An expired session cannot be used to deduplicate old operations. Lease times
are UTC Unix seconds, and clients must track expiry rather than waiting forever
on an expired stream. Renewing a session does not reset its request quota.

Keep one gateway and one state directory for each Redis namespace/VPP instance.
A local file lock prevents sharing a state directory, and a persistent Redis
owner key prevents another journal from claiming the same namespace. Ownership
has no automatic lease takeover. After losing the journal, an operator must
stop the former owner, reconcile VPP, and deliberately remove the owner key;
do not just delete it to start another active instance. Preserve Redis ownership
keys using persistence and `noeviction`. Active-active failover is not provided.

## Large uploads

Upload a UTF-8 text value using these messages on the same session request
stream. `bytes` and the digest refer to the decoded UTF-8 bytes, before JSON
escaping in a chunk envelope.

```json
{"id":"begin-1","method":"transfer.begin","params":{"transfer":"upload-1","bytes":12345,"sha256":"<64 lowercase hex digits>"}}
{"id":"chunk-1","method":"transfer.chunk","params":{"transfer":"upload-1","seq":0,"data":"<first text segment>"}}
{"id":"commit-1","method":"transfer.commit","params":{"transfer":"upload-1"}}
```

Chunk at UTF-8 boundaries and keep the **whole encoded envelope** within the
frame limit. Each chunk reply contains `next_seq`; `transfer.status` takes only
`transfer` and reports the persisted offset and commit state. Retransmitting
the most recently accepted chunk is idempotent if its sequence and digest
match. Older chunks return `sequence_mismatch`; consult status and resume from
`next_seq`. Bulk chunks are identified by transfer/sequence, not retained in
the ordinary request journal. A client must await each chunk acknowledgment
before sending the next. Transfers expire ten minutes after creation; this
expiry is not extended by session renewal.

Commit validates declared length and SHA-256. Committed transfers are immutable
and session-private. A committed JSON object can be referenced as parameters:

```json
{"id":"request-4","method":"interface.set_state","params_ref":"upload-1"}
```

Uploads can be up to 64 MiB; the first interface adapter accepts at most 256 KiB
of JSON parameters (including its validation envelope). Staging a larger upload
therefore does not imply that this small interface API can consume it. Future
bulk adapters should read committed files incrementally. Transfer chunking does
not lift VPP's binary message limits or make a sequence of operations atomic.

Default resource limits are in `src/common.h` and checked in the server:

- 256 KiB encoded frame; 32 response frames outstanding per session.
- 64 MiB response spool budget per session, 256 MiB across sessions, plus small
  terminal errors and control replies. A dump that exhausts space ends in an
  error, never a false `complete`; the VAPI adapter keeps draining the dump.
- 64 MiB per upload; four transfers per session; 256 MiB aggregate reserved
  upload bytes. The disk budget includes both unfinished and committed uploads.
- 32 sessions, 128 ordinary requests per session, 16 MiB retained request JSON.

This version rejects an individually oversized result record. Current interface
records are small. Generic large-record fragmentation, compression, binary
uploads, cancellation, subscription events, complete schema-wide API coverage,
and disk-to-object-store spillover are future extensions, not advertised features.

## Redis access control

Registration verifies a configured client secret, but **session-stream access
is enforced by Redis ACLs**, not by the session ID. Do not give independent
clients one shared unrestricted Redis login. Provision each client to:

- Append only to `vpp:register` and its own
  `vpp:session:{demo:*}:requests` keys.
- Read/delete only its private bootstrap and response keys.
- Have no read access to the registration stream, which carries client secrets,
  and no access to the gateway owner key or another client's session keys.

Use Redis ACL selectors to separate command permissions by key patterns. A
flat ACL combining `XADD`, `XRANGE` and broad key patterns would also permit
clients to forge their own responses or read registration secrets. Validate
the installed ACL with the actual Redis version before deploying independent
clients. The gateway itself needs stream reads/deletes, EVAL and the GET/SET,
XLEN/XADD/EXPIRE commands called by its scripts.

The gateway bounds accepted data and its own output. Direct Redis writers can
still submit oversized entries or flood ingress before the gateway sees them;
Redis memory limits and trusted/rate-limited ingress remain necessary. No Redis
Streams protocol can prevent an unrestricted Redis user from allocating memory.

## Validation

```sh
ctest --test-dir build --output-on-failure
python3 tests/integration_test.py \
  --server "$PWD/build/vpp-json-gateway" \
  --client "$PWD/build/gateway-client" \
  --redis-server "$(command -v redis-server)"
clang-format --dry-run --Werror src/*.cc src/*.h tools/*.cc tests/*.cc
cpplint --repository=.. --filter=-legal/copyright src/*.cc src/*.h tools/*.cc tests/*.cc
```

Core checks cover malformed/deep envelopes, symbolic state, bounded output with
an error terminal, checksum failures, transfer retry/isolation and journal
persistence. Integration tests start their own loopback Redis on a temporary
port, and cover 3,000-record output, a paused consumer, another client's progress,
large upload, params references, request conflicts, retained replay without
reexecuting a mutation, and recovery from a persisted running marker.

The Redis integration harness uses the mock backend. A separate live harness
starts and stops its own unprivileged VPP and Redis processes, creates only
loopback interfaces, and retains logs in a fresh directory:

```sh
python3 tests/live_vpp_test.py \
  --vpp /path/to/vpp-install/bin/vpp \
  --vppctl /path/to/vpp-install/bin/vppctl \
  --server "$PWD/build-vpp/vpp-json-gateway" \
  --redis-server "$(command -v redis-server)" \
  --workdir /path/to/a/new/live-test-directory
```

It tests a 3,001-interface dump, independent CLI verification of mutations,
a gateway process crash, and VPP restart/reconnection. It uses private sockets
and shared-memory names, regular 4 KiB pages, a 512 MiB main heap and a 128 MiB
statistics segment. All fixture processes are stopped afterward.
See `VALIDATION.md` for the results and retained evidence.

For the VPP source fixture, we used the following build configuration on
Ubuntu 26.04. Install its build dependencies, including `python3-ply`,
`libnuma-dev`, `libelf-dev`, `libunwind-dev` and OpenSSL development files first:

```sh
cmake -S /path/to/vpp/src -B /path/to/vpp-build -G Ninja \
  -DCMAKE_BUILD_TYPE=release -DCMAKE_C_STANDARD=11 \
  -DCMAKE_INSTALL_PREFIX=/path/to/vpp-install \
  -DVPP_USE_LTO=OFF -DVPP_PLUGINS=none -DVPP_DRIVERS=none \
  -DVPP_CRYPTO_ENGINES=none -DVPP_TESTS=none -DVPP_TOOLS=vppctl
cmake --build /path/to/vpp-build -j4
cmake --install /path/to/vpp-build --component vpp
cmake --install /path/to/vpp-build --component vpp-lib
cmake --install /path/to/vpp-build --component vpp-dev
```

C11 is for VPP's C sources under GCC 15; the gateway remains C++20. Selecting
these install components avoids requiring VPP's separate Python API package
for a C++ application.

## Source references

- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide)
- [VPP 26.06 interface API](https://github.com/FDio/vpp/blob/stable/2606/src/vnet/interface.api)
- [VPP VAPI generated-binding implementation](https://github.com/FDio/vpp/blob/stable/2606/src/vpp-api/vapi/vapi_c_gen.py)
- [VPP VAPI tests](https://github.com/FDio/vpp/blob/stable/2606/src/vpp-api/vapi/vapi_c_test.c)
- [Redis Streams](https://redis.io/docs/latest/develop/data-types/streams/)

## Complete runnable example

See [example/README.md](example/README.md) for a local Redis configuration,
separate test credentials and ACLs, gateway startup script, and a C++ client
that registers, queries, streams responses, and changes an interface by name.
