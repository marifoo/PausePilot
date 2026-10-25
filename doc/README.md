# PausePilot developer documentation

This folder is for **contributors** who have read the top-level `README.md`
and now want to understand how the application is structured before opening
source files. It is *not* an API reference — the per-class and per-function
detail lives in the `.h` files where it is least likely to rot.

Read in this order:

- [`architecture.md`](architecture.md) — the component model, what each
  module hides, the `SessionStore` seam, and the signal/slot wiring set up
  in `main.cpp`. Start here.
- [`runtime-behaviour.md`](runtime-behaviour.md) — the `TimeTracker` state
  machine, the 100 ms heartbeat, cross-midnight handling, and the shutdown
  flow (including the Linux Unix-signal path).
- [`persistence.md`](persistence.md) — the SQLite schema, checkpoints, and
  how orphan checkpoints are reconciled on startup.
