#!/bin/sh
set -eu
cd "$(dirname "$0")"
. ./server.env
umask 077
exec "${GATEWAY_SERVER:-../build/vpp-json-gateway}" \
  --backend mock --mock-count 100 --redis-host 127.0.0.1 --redis-port 6389 \
  --namespace vpp --clients ./clients.json --state ./state
