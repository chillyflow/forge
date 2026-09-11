# Pre-review combined-loop smoke

The local Qwen3-Coder model completed the Go `add` integration smoke with all four
new loop policies enabled and two candidates. Independent verification passed,
protected files were unchanged, and cold end-to-end time was 56.641 seconds.
The run used 23 actions and 1806 generated tokens. Full outputs, session events,
trial workspaces and validation evidence are retained with [results.json](results.json).

This is one integration smoke on an earlier executable, before the final recovery,
indexing and diagnostic review corrections. Its identity is recorded in
[environment.json](environment.json); the runner did not retain a source snapshot
for this smoke. It must not be pooled with the later frozen 42-run diagnostic or
used as accuracy, preservation or promotion evidence.

[complete-evidence.zip](complete-evidence.zip) contains this directory's retained
evidence and a SHA-256 inventory. Model weights and runtime libraries are excluded.
