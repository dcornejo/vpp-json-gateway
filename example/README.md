# Local gateway example

This example runs Redis on `127.0.0.1:6389`, a gateway with 100 mock interfaces,
and a C++ client. No VPP installation is needed. All credentials are complete,
public **test-only** values; replace them before using this configuration outside
an isolated local test. Redis 7 or later is required for ACL selectors.

Build from the repository root:

```sh
cmake -S . -B build
cmake --build build -j
```

Run these commands in three separate terminals, starting in the repository root.

1. Start the example Redis instance:

   ```sh
   cd example
   redis-server redis.conf
   ```

2. Start the gateway:

   ```sh
   ./example/start-gateway.sh
   ```

3. Run the sample client:

   ```sh
   . ./example/client.env
   ./build/example-client
   ```

The client authenticates to Redis, registers `demo` with the gateway, and uses
its assigned request/response stream keys. It prints capabilities, streams the
interface list, changes `loop1` to `up`, and lists again. Each response is printed
as JSON and acknowledged with `XDEL` after processing. The 100-item lists exceed
the 32-frame response window and demonstrate continued delivery through
acknowledgments. Request IDs and sequence numbers correlate and order replies.
See `client.cc` for the complete exchange, including timeout and error handling.

## Configuration and keys

- `redis.conf`: loopback listener on port 6389; persistence disabled for testing.
- `users.acl`: separate Redis users for the server (`gateway`) and client (`demo`).
  The client can append requests and read/delete its own responses; selectors
  prevent it from reading registration secrets or writing response frames.
- `server.env`: gateway Redis username and password.
- `client.env`: client Redis username/password plus the gateway registration token.
- `clients.json`: gateway allowlist mapping `demo` to that same registration token.
- `start-gateway.sh`: gateway options, including the mock backend and state path.

There are two independent credentials: the Redis password authenticates the TCP
connection; `GATEWAY_TOKEN` authenticates session registration. This local TCP
example does not use TLS certificates or private keys.

Redis stream keys are created automatically:

- `vpp:register`: registration requests.
- `vpp:bootstrap:demo`: registration responses with assigned session keys.
- `vpp:session:{demo:<session-id>}:requests`: session requests.
- `vpp:session:{demo:<session-id>}:responses`: session responses.

Run one sample client at a time. It starts a fresh session on every run and does
not persist recovery state. Sessions expire after one hour; after 32 sessions,
wait for expiration or reset the disposable environment. For saved sessions and
explicit replay, use the repository's `gateway-client` tool instead.

Stop the gateway and Redis with Ctrl-C in their respective terminals. To reset,
stop both, remove `example/state/`, then restart both. Reset them together because
the gateway journal is associated with the Redis namespace. The state directory
is ignored by Git. Do not run against an existing Redis instance on port 6389.

For a different build directory, set `GATEWAY_SERVER` to the absolute path of
`vpp-json-gateway` before starting the script, and run `example-client` from that
same build. For live VPP, follow the main README's VAPI build instructions and
change the gateway backend/socket options; this sample's mutation assumes the
mock interface `loop1` exists.
