<!--
Copyright 2026 David Cornejo
SPDX-License-Identifier: Apache-2.0
-->

# TODO

Prioritize large-message completion and broader API coverage.

- [x] **Large individual messages:** Added bounded response fragmentation and client reassembly for logical records up to 16 MiB; uploaded JSON parameters up to 16 MiB; generated native payloads up to 1 MiB. Redis frames remain limited to 256 KiB. Larger/unbounded records, binary uploads, and compression remain future extensions.
- [ ] **API coverage:** 35 interface services are supported. Add events, TX-placement streaming, variable-length replies, and broader modules.
- [ ] **Symbolic identifiers:** Typed interface indexes and enums are translated; translate remaining untyped numeric resource identifiers where appropriate.
- [ ] **Scheduling:** Operations run one at a time, so long requests delay others. Improve scheduling and add cancellation.
- [ ] **Recovery:** Interrupted mutations can have an unknown outcome requiring reconciliation. Add reconciliation support and failover.
- [ ] **Production readiness:** Add native Redis TLS and Cluster support. Validate sustained load, persistence-loss scenarios, and concurrent interface lifecycle changes.
- [ ] **Client library:** Build a reusable client with multiplexing, automatic lease renewal, and durable recovery.
