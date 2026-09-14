# TODO

Prioritize large-message completion and broader API coverage.

- [ ] **Large individual messages:** Multi-frame dumps and uploads work, but individual records and consumed parameters remain limited to 256 KiB. Add support for larger individual records and parameters.
- [ ] **API coverage:** 35 interface services are supported. Add events, TX-placement streaming, variable-length replies, and broader modules.
- [ ] **Symbolic identifiers:** Typed interface indexes and enums are translated; translate remaining untyped numeric resource identifiers where appropriate.
- [ ] **Scheduling:** Operations run one at a time, so long requests delay others. Improve scheduling and add cancellation.
- [ ] **Recovery:** Interrupted mutations can have an unknown outcome requiring reconciliation. Add reconciliation support and failover.
- [ ] **Production readiness:** Add native Redis TLS and Cluster support. Validate sustained load, persistence-loss scenarios, and concurrent interface lifecycle changes.
- [ ] **Client library:** Build a reusable client with multiplexing, automatic lease renewal, and durable recovery.
