# Second repair candidate: rejected development experiment

Revision `6b8df0d664db8160f8efc1f649aa9d5cffad82dc` passed **84/87** from
a clean freeze, but failed **all three atomic-transfer repetitions**. It cannot
satisfy the immediate atomic-transfer gate. No holdout model run had occurred.

This candidate corrected native validation of the host's final-only registry
and shortened the repair instructions to discourage long pre-call explanations.
The latter prompt change was rejected after the atomic traces skipped baseline
validation and rewrote the transaction algorithm incorrectly. The next candidate
restores the first candidate's exact repair instructions while retaining the
specific final-only template fix and its native-template regression test.

All other development tasks passed 3/3, including quota allocation, event replay,
interval union and query parsing. These successes are not combined with another
candidate's atomic passes to manufacture a passing matrix.

[Audit](audit.json), [numeric analysis](analysis/summary.json), the original
[protocol lock](protocol-lock.json), all numeric records and failed fixture
sources are preserved. Full original sessions remain at the source path recorded
in the audit. This is development evidence, not a comparative holdout claim.
