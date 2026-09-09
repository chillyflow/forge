# First repair candidate: development evidence

Revision `c1b0ca036901047ce57694e378dd32c017bc5f77`, frozen from a clean
checkout. Forge passed **82/87** on the unchanged 29 development tasks.
Atomic transfers and quota allocation each passed 3/3. This is development
evidence and cannot satisfy the new-holdout gate.

All five failures remain: three query-parser runs, one interval-union run, and
one event-replay run. The query repairs still accepted malformed percent
encoding. Interval union exhausted the per-call generation budget before an
edit. The event-replay run reached the final-only registry and exposed a
native-template validation bug: the validator incorrectly required a memory
tool even when the host intentionally allowed only final. Prompt counting
masked that rejection as a context-limit error. That finding led to the second
candidate before any holdout model run.

[Audit](audit.json), [numeric analysis](analysis/summary.json), and the
[original protocol lock](protocol-lock.json) are retained. Each run has its
unchanged numeric record, independent verifier output and diff; failed fixture
sources are included. Full original sessions remain at the source path in the
audit. This export omits private harness state and generated caches.

Build work on the next candidate overlapped the tail of this development run.
Treat timing as descriptive; no comparative timing claim uses this dataset.
The measured executable and frozen source checkout remained unchanged.
