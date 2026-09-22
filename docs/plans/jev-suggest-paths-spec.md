# `suggest_paths` — implementation spec

Date: 2026-09-20. Repo HEAD `bfadc7bf`.

**Purpose.** Add a `suggest_paths` tool to the minimal-agent schema that creates a jev-rerankable decision point inside the benchmark loop. The dominant read surface is `read_file` (1086/1179 retained events), but it goes straight from "model picks path" → "read that file" with no intermediate candidate selection. `suggest_paths` fills that gap: it gathers candidate paths matching a query, optionally reranks them via jev, and returns the ranked list as data. The model then calls `read_file` on its chosen path.

This is the minimal-schema complement to the existing `retrieve_context` rerank (which is wired but unreachable in the benchmark loop).

---

## 1. Tool contract

| Field | Value |
|---|---|
| **Name** | `suggest_paths` |
| **Description** | "Find source files relevant to a query. Returns candidate paths with snippets, optionally reranked by a semantic judge. Read a result with read_file." |
| **Arguments** | `query:string` (required) — search term or question |
| **Capabilities** | `FORGE_CAP_READ` (read-only, no filesystem mutation) |
| **Mutates workspace** | No |
| **Returns** | JSON object: `{query, candidates[], reranked, model?}` |

The tool is advisory: it returns data, the model decides which path to read next. This preserves the host-authority constraint (jev never gates).

---

## 2. Candidate gathering

Candidates come from `fg_repo_search`, which does a literal `instr(content, query)` scan over indexed chunks, ordered by path. The existing function returns formatted text:

```
src/a.c:10:match line content...
src/b.c:25:another match...
[result limit reached]
```

Rather than parse this fragile text, add one internal function in `src/repo/repo.c`:

```c
typedef struct {
    char path[FG_PATH_MAX];
    size_t line;
    char *snippet;  /* malloc-owned, caller frees */
} fg_repo_search_hit;

/* Fill hits[0..count) with structured search results. Returns hit count. */
size_t fg_repo_search_hits(forge_repo *r, const char *query, size_t limit,
                           fg_repo_search_hit *hits, size_t *truncated, forge_error *e);
```

`fg_repo_search` (richer loop, backward compat) is refactored to call `fg_repo_search_hits` then format. `suggest_paths` uses the structured hits directly.

Default candidate limit: 32 (matches current `FORGE_JUDGE_DEFAULT_MAX_CANDIDATES`). The existing 50-hit callers (`search_text`, CLI `retrieve` command) are unaffected.

---

## 3. Judge rerank wiring

When `c->config.judge` is set, call `forge_judge_rerank` over the candidates before returning. Wiring mirrors `src/tools/tools.c:1852-1856`:

```c
if (c->config.judge) {
    /* Cap candidates at the judge's configured max. */
    if (count > c->config.judge->max_candidates)
        count = c->config.judge->max_candidates;
    double *scores = calloc(count, sizeof(double));
    const char **paths = calloc(count, sizeof(char*));
    const char **snippets = calloc(count, sizeof(char*));
    const char **stages = calloc(count, sizeof(char*));
    for (size_t i = 0; i < count; i++) {
        paths[i] = hits[i].path;
        snippets[i] = hits[i].snippet;
        stages[i] = "literal";  /* fg_repo_search is literal-only */
    }
    forge_error rerank_error = {0};
    forge_status status = forge_judge_rerank(c->config.judge, query, count,
                                             paths, snippets, stages, scores, &rerank_error);
    if (status == FORGE_OK) {
        size_t order[FORGE_RETRIEVAL_MAX_RESULTS];
        fg_rerank_permutation(scores, count, order);
        /* Reorder hits by permutation. */
        fg_repo_search_hit *ordered = malloc(count * sizeof(*ordered));
        for (size_t i = 0; i < count; i++)
            ordered[i] = hits[order[i]];
        memcpy(hits, ordered, count * sizeof(*ordered));
        free(ordered);
        reranked = true;
        snprintf(model, sizeof(model), "%s", c->config.judge->last_model);
    }
    free(scores); free(paths); free(snippets); free(stages);
}
```

Fail-open: any judge error (transport, parse, timeout, LIMIT) keeps the original path order. The call is bounded by `forge_judge_budget_ms(judge)` which is already configured at judge creation time.

---

## 4. Output format

Returns a UTF-8 string (the tool result format for native protocol):

```json
{
  "query": "where is the edit journal prepared",
  "candidates": [
    {"path": "src/tools/edit_journal.c", "line": 42, "snippet": "static void edit_journal_prepare(...)"},
    {"path": "src/tools/edit_journal.c", "line": 87, "snippet": "void edit_journal_commit(...)"}
  ],
  "reranked": true,
  "model": "jev-1.13.0"
}
```

When no judge is configured:

```json
{
  "query": "...",
  "candidates": [...],
  "reranked": false
}
```

When no hits: `"candidates": []`, `reranked: false`. The model falls back to `read_file` with a known path or `list_directory`.

The `snippet` field is bounded to 512 bytes per hit (matching `fg_repo_search`'s existing `FG_MIN(n, 512)` cap).

---

## 5. Schema entries

### 5.1 Tool registry (`src/tools/tools.c:15`)

Add to `static const fg_tool_def definitions[]`:

```c
{"suggest_paths",
 "Find source files relevant to a query. Returns candidate paths with snippets, "
 "optionally reranked by a semantic judge. Read a result with read_file.",
 "query:string", NULL, FORGE_CAP_READ},
```

Order: right after `retrieve_context` (both are retrieval-shaped tools). This auto-generates the grammar production `callN` via `fg_tool_grammar`.

### 5.2 Minimal native schema (`fg_tool_minimal_native_schema`)

Add after `read_file`:

```c
native_schema_function(&out, "suggest_paths",
                       "Find source files relevant to a query. Returns candidate paths with "
                       "snippets, optionally reranked by a semantic judge. Read a result with "
                       "read_file.",
                       "query:string", true) &&
```

The trailing `true` adds the comma separator before the next entry (`apply_patch`).

### 5.3 Candidate schema

`fg_tool_candidate_schema` delegates to `fg_tool_minimal_native_schema`, so it inherits automatically. No change needed.

### 5.4 Noedit schema

`fg_tool_noedit_schema` is the candidate surface without `apply_patch`. `suggest_paths` is read-only and should be available here too. Add it after `read_file` in the same pattern.

### 5.5 Richer loop

`suggest_paths` is NOT added to the richer loop's tool set (the richer loop already has `retrieve_context` with rerank wired; adding `suggest_paths` there would be redundant and the richer loop is not benchmarked). If the richer loop needs it later, a one-line addition to `fg_tool_schema`'s definitions iteration covers it.

---

## 6. Dispatch

In `src/tools/tools.c`, add a dispatch branch before `search_text` (line 1838):

```c
if (!strcmp(name, "suggest_paths"))
    return suggest_paths(c, args, e);
```

The static function `suggest_paths` lives in `tools.c` (near `read_lines`, ~line 791). It:
1. Extracts `query` from args (required, non-empty string)
2. Calls `fg_repo_search_hits` with limit 32
3. If judge configured, reranks as described in §3
4. Formats JSON output as described in §4
5. Frees all allocated resources

Argument validation: query must be a non-empty string ≤ 1024 bytes (`FORGE_RETRIEVAL_MAX_QUERY_BYTES`). Empty query returns `FORGE_ERR_ARGUMENT`.

---

## 7. Files touched

| File | Change |
|---|---|
| `src/repo/repo.c` | Add `fg_repo_search_hits`; refactor `fg_repo_search` to call it |
| `src/repo/repo_internal.h` | Declare `fg_repo_search_hit` and `fg_repo_search_hits` |
| `src/tools/tools.c:15` | Add `suggest_paths` entry to `definitions[]` |
| `src/tools/tools.c:279` | Add `suggest_paths` to `fg_tool_minimal_native_schema` |
| `src/tools/tools.c:327` | Add `suggest_paths` to `fg_tool_noedit_schema` |
| `src/tools/tools.c:~1838` | Add dispatch branch |
| `src/tools/tools.c` (new static fn) | `suggest_paths` implementation (~80 lines) |
| `tests/unit/test_judge.c` | Add unit tests (§9) |
| `tests/integration/test_suggest_paths.py` | New integration test |
| `docs/plans/jev-suggest-paths-spec.md` | This file |

---

## 8. Edge cases & error handling

| Condition | Behavior |
|---|---|
| Empty/missing query | `FORGE_ERR_ARGUMENT`, no candidates gathered |
| Query > 1024 bytes | `FORGE_ERR_ARGUMENT` (matches `FORGE_RETRIEVAL_MAX_QUERY_BYTES`) |
| No hits | Return `"candidates": []`, `reranked: false` |
| Judge not configured | Return candidates in path order, `reranked: false` |
| Judge fails (any error) | Keep path order, `reranked: false` (fail-open) |
| Candidate count > judge max_candidates | Cap at `judge->max_candidates` before calling |
| Candidate count = 0 | Skip rerank call entirely |
| `fg_repo_search_hits` fails | Return the error, no rerank attempted |
| Binary/null in snippet | Already handled by `fg_repo_search`'s existing UTF-8 validation |

---

## 9. Unit tests (`tests/unit/test_judge.c`)

Add to the existing judge unit test file (uses the deterministic `stub_transport`):

**`test_suggest_paths_rerank_order`** — mock 3 candidates with scores `{0.3, 0.9, 0.6}`, verify output order is `[1, 2, 0]` (descending score), verify `reranked: true` and model id recorded.

**`test_suggest_paths_fail_open`** — mock transport returning malformed JSON, verify output is original path order, `reranked: false`, no crash.

**`test_suggest_paths_above_cap`** — create judge with `max_candidates = 2`, pass 4 candidates, verify only 2 are scored (the first 2 passed to the judge), and `rerank_scored == 2` in stats.

**`test_suggest_paths_no_judge`** — call with `c->config.judge == NULL`, verify path order preserved, `reranked: false`.

**`test_suggest_paths_empty_query`** — verify `FORGE_ERR_ARGUMENT`.

---

## 10. Integration tests (`tests/integration/test_suggest_paths.py`)

Uses a real indexed fixture repo (Go or Python):

**`test_suggest_paths_returns_candidates`** — run `forge --minimal-agent` with a query that matches known symbols, verify at least one candidate returned, verify JSON parses, verify candidate paths exist in workspace.

**`test_suggest_paths_with_judge`** — same but with `--judge`, verify `reranked: true` when `TYPESAFE_API_KEY` is set (skip with exit 77 otherwise).

**`test_suggest_paths_no_match`** — query that matches nothing, verify empty candidates, verify run continues (model can fall back to list_directory).

---

## 11. Design decisions & trade-offs

**Why a new tool instead of extending `read_file`?** Separating "find paths" from "read path" keeps each tool's contract clean. The model explicitly decides: "I need to discover" vs. "I know what to read." Adding a `query` param to `read_file` conflates two operations and makes the tool harder to test and reason about.

**Why not wire rerank into `search_text`?** `search_text` isn't in the minimal schema AND the agent doesn't use it even in the richer loop (0/1179 events, finding N1). Two unknowns: reachability AND agent behavior change. `suggest_paths` targets the proven `read_file` surface.

**Why default to 32 candidates?** Matches `FORGE_JUDGE_DEFAULT_MAX_CANDIDATES`. The existing N3 finding (32-cap vs 50-hit silent fail-open) is the exact bug we're avoiding. If the candidate pool should be larger later, raise both consistently.

**Why literal search only?** `fg_repo_search` is literal `instr(content, query)` — it's what's indexed and reachable in the minimal loop. Adding exact-symbol or FTS stages would require pulling from `forge_repo_retrieve` (richer loop only). Literal is the right first cut; measure hit@1 delta, then expand.

**Why not gate on jev confidence?** The existing repair-feedback campaign's refutation was caused by saturated thresholds producing constant "inspect" hints. `suggest_paths` avoids this by design: it returns ranked data, not guidance text. The model uses the ranking as one signal among many. No confidence gate needed.

---

## 12. Estimated effort

| Component | Lines | Time |
|---|---|---|
| `fg_repo_search_hits` (repo.c) | ~40 | 0.5 day |
| `suggest_paths` static fn (tools.c) | ~80 | 0.5 day |
| Schema entries (4 files) | ~8 | 0.25 day |
| Unit tests | ~60 | 0.5 day |
| Integration tests | ~40 | 0.5 day |
| Build + GPU suite verification | — | 0.25 day |
| **Total** | **~228** | **~2.5 days** |

After implementation: run the engagement screen (Phase 3 of the original plan, adapted for `suggest_paths`) to confirm the tool is actually called by the agent in benchmark tasks. Kill if engagement stays near zero after prompt tuning.

---

## 13. Out of scope

- **Repair-feedback threshold retune** — separate campaign, parked by its own refutation.
- **Best-of-N candidate selection** — closed axis.
- **Context-ranking (D4)** — shadow-only, different campaign.
- **Adding `suggest_paths` to the richer loop** — unnecessary for the benchmark path; can be added later trivially.
