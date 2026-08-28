# Summary generation and cache smoke — 2026-08-28

This checks one tiny Go file with Qwen3-Coder on Windows/CUDA. It establishes
bounded generation, persistent cache reuse and dependency invalidation for this
fixture, not broad summary quality or an overall performance improvement.

[environment.json](environment.json) identifies exact source `2184388`, binary,
backend, verified model SHA-256 and configuration.
[results.json](results.json) includes complete prepared evidence, generated text,
cache identities, metrics and the fixture changes made by the test harness.

| Invocation | Indexed generation | Model calls | Prompt / generated tokens | CLI model load | Summary operation |
| --- | ---: | ---: | ---: | ---: | ---: |
| Cold file summary | 1 | 1 | 446 / 164 | 11,110 ms | 1,813 ms |
| Reload, unchanged file | 1 | 0 | 0 / 0 | 9,031 ms | 0 ms |
| Unrelated file added | 2 | 0 | 0 / 0 | 9,031 ms | 0 ms |
| Summarized function changed | 3 | 1 | 451 / 134 | 8,078 ms | 1,062 ms |

The first summary correctly describes `Answer()` returning `37`. The second and
third return identical cached text and keys without generation. After the
harness changes that function to return `53`, the fourth produces a new key and
a summary describing `53`. Each CLI invocation loads a fresh model; zero
generation does not mean zero end-to-end cost. Windows timer resolution is
coarse: the zero-duration operations were below the observed resolution.
These individual timings have no repetitions, baseline or confidence interval.

## Retained initial quality issue

An earlier `forge.summary.v1` trial on source `2a91ccd` correctly named the return
value but also claimed that function bodies were absent, despite full-source
evidence. It recited metadata instead of focusing on behavior. That output is
retained and **is not a successful quality trial**. The revised recipe explicitly
names the selected evidence mode and asks for concise source-supported behavior.
Its new recipe/prompt identity prevents an old cached summary from being treated
as a v2 result. The few source claims above were manually checked; this is not a
general hallucination test or measured task-quality result.

## Reproduce

Initialize a standalone Git repository with the two initial files in the record.
Run the following command twice. Add the recorded unrelated file and run again;
then make the recorded `37` to `53` edit and run once more.

```text
forge summarize answer.go --workspace FIXTURE --no-config --model MODEL --summary-producer VERIFIED_MODEL_AND_BACKEND_ID --gpu-layers -1 --context 4096 --output-reserve 512 --max-tokens 512 --max-input 3584 --threads 1 --temperature 0 --seed 42 --wall-ms 180000
```

Use the exact producer assertion from the environment record with the verified
model/backend. The runtime trusts that assertion; it does not independently hash
the weights. Check the actual text, keys, generation counters, zero-call hits and
unchanged source after each command. The harness alone changes fixture files.
No source-edit tool or validation command was invoked. Repository indexing still
uses Git discovery. No Linux CUDA, macOS Metal or agent summary-selection result
is implied.
