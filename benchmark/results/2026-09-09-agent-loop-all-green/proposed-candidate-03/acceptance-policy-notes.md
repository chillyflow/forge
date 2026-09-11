# Proposed acceptance evidence policy

This proposal changes only the mirrored `benchmark/agent_loop_acceptance.py`,
`benchmark/agent_loop_campaign.py`, and `tests/unit/test_agent_loop_acceptance.py`.
Original copies and hashes are retained under `originals/` and
`acceptance-provenance.json`. Do not apply during the frozen candidate-02 batch.
The companion common/run verifier and retention changes must be applied with it.

New freezes declare candidate schema version 2 and an explicit `verification_policy`.
The policy is included in both the candidate hash and configuration hash:

- Python verifier: `fresh-external-prefix-no-write`.
- Other verifier: `not-applicable`.
- Terminal retention: `complete-except-git-forge`.
- Input inventory: `all-files-except-root-git-forge-directories`.
- Pre-verification snapshot required.

The exclusion applies only to root `.git`/`.forge` directories, with Windows case
matching. Root regular files with those names, nested directories, bytecode,
pytest caches, generated artifacts and every other input file are retained.

Preflight now calls the same common verifier, preserving its actual cache-policy
metadata and full subprocess logs. New run envelopes retain the original harness
result, original before/after maps, full pre-verification snapshot, full terminal
snapshot, and actual verifier/retention metadata. The reporter compares both maps
against explicit snapshot inventories and the raw harness copies in the verified
archive. Missing metadata/maps/snapshots, changed input bytes, and mismatched flags
cannot produce a passing outcome. A verifier mutation with retained consistent
evidence remains a non-passing result.

Candidates frozen without this policy retain the original reporting rules and
receive the descriptive label `legacy-unspecified`; no old run is reclassified.
A read-only comparison against candidate-01's existing evidence produced the same
report apart from that added label (`legacy-report-check.json`). G0-G6 membership,
profiles, commands, seeds and denominators are unchanged.

`acceptance-policy.patch` contains only these three owned files. Its original and
proposed hashes are recorded in `acceptance-provenance.json`. Check application
against the actual source before applying after the frozen batch:

```powershell
git apply --check benchmark/results/2026-09-09-agent-loop-all-green/proposed-candidate-03/acceptance-policy.patch
```

The mirror's model-free tests use byte-identical fixture/contract copies under the
mirror; those copies are test dependencies, not changed production source. The 31
tests include the previous 22 checks, schema/configuration binding, stale/missing
cache metadata, preflight policy, omitted bytecode, root gitfiles, full pre/post
snapshot retention, verifier mutation, synthetic freeze without process launch,
and packaging-to-report verification with retained cache bytes.
