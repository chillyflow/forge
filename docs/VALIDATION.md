# Go dependency graph and staged validation

Forge plans validation from indexed Go packages instead of asking the model to
invent each command. The planner is a library operation: **it does not execute
commands, invoke Go, or refresh the index**. A returned plan is not a test result.

## Library API

```c
#include <forge/validation.h>

forge_error error = {0};
forge_repo *repo = forge_repo_open(workspace, &error);
if (repo && forge_repo_index(repo, &error) == FORGE_OK) {
    const char *changed[] = {"pkg/base/base.go", "pkg/base/base_test.go"};
    char *json = forge_repo_validation_plan(repo, changed, 2, &error);
    /* Inspect the plan, or pass it to an executor enforcing the contract below. */
    forge_free(json);
}
forge_repo_close(repo);
```

Call `forge_repo_index` after the changes, including deletions. Planning against
an unopened/unindexed repository fails. Planning against a previously indexed
repository uses that snapshot; the API cannot know whether the caller has made
unindexed edits. An empty change list requests broad verification. Absolute
paths, traversal, protected metadata, and unsafe existing paths are rejected.
Forward and backward slashes and leading `./` are normalized; duplicate paths
are removed. Output is deterministic for the same indexed snapshot and change
set, including when the repository is reopened.

`fg_repo_targets` remains a compact human suggestion. It is not an executable
schedule and does not refresh the index after a patch.

## Graph construction

The repository index stores Go package clauses, whether a file is a test,
explicit build-constraint presence, syntax-error flags, and imports. It also
indexes `go.mod`, `go.sum`, `go.work`, and `go.work.sum`. Existing indexes acquire
the Go metadata on the next scan even when file contents are unchanged.

Each directory containing eligible Go source becomes a package. Its module is
the nearest containing `go.mod`; its import path is the declared module path
plus the relative package directory. Nested modules therefore do not inherit a
parent module's import identity. Discovered module boundaries are retained even
when their metadata cannot be read or exceeds the index's 2 MiB file limit.
Their import identities are unknown, but their module directories still appear
in the final verification stage.

Imports resolve by declared local package import path, not by package name or
textual directory suffix. Normal imports, aliases, blank imports, dot imports,
and imports from both internal and external test files participate. Duplicate
edges are merged; `test_only` is true only when every observation of an edge
comes from a test file. An external test importing its own package is not
treated as an import cycle. Cross-module matches are candidate edges: Go's
actual workspace/version/replacement selection is not emulated.

Directly changed package directories form the initial set. A reverse graph
traversal adds every transitive importing package, including test importers.
Traversal is cycle-safe. A changed test file still selects reverse dependents
conservatively. Entirely deleted Go packages remain as `present: false` nodes
derived from the changed paths, allowing surviving importers to be tested;
commands targeting the missing package itself are omitted.

Module configuration changes select every package in the relevant module.
Workspace configuration changes select all packages. Non-Go inputs in a known
package select that package; inputs without a direct mapping require broad
verification. `vendor`, `testdata`, and directories/files beginning with `.` or
`_` are excluded from package discovery. Changes to such fixtures still request
broad verification. Formatting can check an indexed changed Go fixture without
treating it as a build package.

## Schedule

All six stages are emitted in this order. Empty stages are allowed.

| Stage | Scope and command |
| --- | --- |
| `format` | Indexed changed Go files, or all eligible indexed Go files when the change set is unknown: `gofmt -l ./path.go` |
| `compile` | Present affected packages: `go test -json -count=1 -vet=off -run ^$ ./pkg` |
| `affected_tests` | Present affected packages: `go test -json -count=1 -vet=off ./pkg` |
| `dependent_tests` | Transitive reverse importers: `go test -json -count=1 -vet=off ./consumer` |
| `vet` | Affected packages and reverse importers: `go vet ./pkg ./consumer` |
| `broad_tests` | Each indexed module, from its own directory: `go test -json -count=1 ./...` |

The broad stage is always present for applicable Go repositories, even when no
specific fallback was detected. Narrow checks are a scheduling optimization,
not evidence that all other tests can be omitted. A root-module `./...` is not
used as a substitute for testing nested modules. Go's package-pattern and
module rules are described in the [Go command documentation](https://pkg.go.dev/cmd/go)
and [module reference](https://go.dev/ref/mod).

An empty changed-path list does not skip formatting: it checks all eligible
indexed Go source, including nested modules. This covers explicit broad checks
and command-origin edits whose exact paths are unavailable. The unknown-change
scan excludes vendor/testdata/hidden fixture paths consistently with package
discovery; a known changed Go file remains individually eligible for formatting.

Packages are sorted by workspace-relative directory, grouped by module, and
batched into at most 32 targets and 12,000 target argument bytes per command.
Each argument remains a separate JSON string; no shell command is constructed.
Package/file arguments use `.` or `./...` prefixes, including names that begin
with `-`. `cwd` is workspace-relative, with `.` denoting the root. `-count=1`
avoids reporting a cached test result as a fresh run. The compile stage does not
run matching test functions, but **can execute package initialization and
`TestMain`**; it is not a sandboxed or execution-free compiler operation.

## JSON contract, schema version 1

Top-level fields:

| Field | Meaning |
| --- | --- |
| `schema_version`, `generation` | Plan format and indexed repository generation |
| `language`, `status` | `go`, `planned` |
| `graph_kind`, `sound` | `syntactic_package_imports`, always `false` |
| `applicable` | Whether Go modules/packages or known incomplete Go indexing require a schedule |
| `broad_verification_required` | True for applicable repositories |
| `changed_paths` | Normalized, unique, sorted file paths |
| `modules` | `directory`, nullable `module_path`, and `synthetic` |
| `packages` | `directory`, nullable `import_path`, `module_directory`, `present`, `has_tests`, `affected`, `dependent` |
| `edges` | Importer `from`, imported package `to`, and `test_only`; paths identify nodes |
| `affected_packages`, `reverse_dependents` | Sorted, disjoint directory lists |
| `fallback_reasons` | Sorted `{code, path, detail}` records, one representative path per category |
| `limitations` | Explicit interpretation limits |
| `stages` | Ordered stage objects |
| `command_count` | Total command count, including broad verification |

A stage contains `name`, `requires_previous_success`, and `commands`. A command
contains `cwd`, `argv`, `require_empty_stdout`, and `reason`. For example:

```json
{
  "name": "compile",
  "requires_previous_success": true,
  "commands": [{
    "cwd": "services/payments",
    "argv": ["go", "test", "-json", "-count=1", "-vet=off", "-run", "^$", "./ledger"],
    "require_empty_stdout": false,
    "reason": "directly_affected_packages"
  }]
}
```

A repository with no Go applicability has `applicable: false`, zero commands,
and empty command arrays in all six stages. This means “no Go validation
scheduled,” not “the repository passed validation.”

## Execution contract

An executor must authorize every subprocess, resolve each `cwd` within the
workspace, pass `argv` directly, and enforce output, wall-clock, command, and
cancellation budgets. Run stages in order and stop on the first failed command.
Do not broaden after local failure. Policy denial, timeout, cancellation,
truncated output, or budget exhaustion must not become a passing verification.

`gofmt -l` can exit successfully while printing unformatted paths. Its
`require_empty_stdout: true` makes that a failed check. The plan never requests
`gofmt -w`. All Go commands can run repository code, access the network and
caches, or update Go metadata as permitted by the active Go environment; process
authorization is required. Formatting permission is not a substitute for
process authorization, and process permission is not an OS sandbox.

Before claiming success, an executor should preserve command results and raw
outputs, refresh the index, and ensure that it validated the same generation.
Any changed input requires a fresh plan. The planner itself reports no command
results or pass status.

## Built-in executor and evidence

The built-in executor is available through `forge validate` and
`forge_verify_workspace` in `include/forge/verification.h`. Ordinary agent runs
invoke it before accepting a final answer after edits or any launched command.
Commands can affect unindexed fixtures, so they request broad validation even
when the source generation is unchanged. `--no-auto-validation` disables this
gate for experiments; it does not certify success.

Applicable validation takes [workspace input snapshots](INPUT_SNAPSHOTS.md)
before and after execution. All regular files participate, including binary
fixtures, hidden files and vendor data. Only root `.git/` and `.forge/`
directories are excluded. Defaults are 100,000 files and 2 GiB of contents;
links, special files, incomplete scans and changed inputs cannot pass.
These checks do not cover dependencies or services outside the workspace and
are not an OS sandbox. A test that changes inputs requires a new validation
attempt, even if every command exits zero.

Reports distinguish `commands_attempted` from `commands_run`; `started` is
true only after OS process creation. Unavailable exit status is `-1`.
Policy/event callbacks are followed by a fresh deadline and cancellation check.
Child `cwd` is separate from the workspace trust root used to exclude
repository-controlled bare executables.

`checks_passed` describes the check verdict. `passed` also requires successful
evidence recording; `evidence_complete` is false when a plan, stream, report or
event cannot be saved. Failed writes are reconciled in the returned report and,
where possible, existing files. An I/O failure can leave incomplete artifacts:
consumers must require successful API/run status as well as the report.

Sessions store `validation/NNNN.plan.json`, `validation/NNNN.json`,
`validation/latest.json`, and exact stdout/stderr bytes for each command.
Reports include stage, argv, cwd, exit status, timeout/cancellation/truncation,
input hashes, generation, durations and artifact references. Input hashes
detect changes; they are not cryptographic authentication.

## Conservative fallbacks and limits

Fallback categories include syntax errors; explicit or filename build
constraints; possible import cycles; cgo; relative/escaped imports; unresolved
local imports; missing, invalid, or duplicate module paths; cross-module matches;
replacement directives; Go workspaces; absent/deleted paths; configuration or
unassigned input changes; incomplete indexing; and fallback filesystem
enumeration. A package without a containing indexed module receives a synthetic
root scope. A Go environment may need to be configured before that command can
succeed; Forge does not guess GOPATH or invent a module declaration.

This graph is **not** type resolution, call-graph analysis, symbol-to-test
coverage, or a proof of minimality. Imports from all indexed build variants are
unioned, so the graph can over-select packages or report cycles that disappear
under a particular build configuration. Escaped import literals and local
replacement aliases are not decoded/resolved; they force explicit fallback
reasons instead of invented edges. Package-level tests, rather than guessed
individual test names, are the smallest scheduled test scope.

Even broad tests cover only the active GOOS, GOARCH, tags, toolchain, workspace,
and service environment. Other build matrices, runtime dependencies, embedded
inputs, generated code, cgo headers/libraries, module contents outside the
workspace, and integration services may require additional checks. Go's resolver
can select a downloaded version instead of a local module with the same import
path. The planner flags these resolution limits and never labels the graph
sound. The existing index's Git enumeration/fallback-walker exclusions also
bound what can be discovered.

Hard limits are 1,024 input paths, 256 modules including synthetic scopes, 4,096
packages including deletion nodes, 65,536 internal import observations before
deduplication, 2,048 planned commands, and 16 MiB of serialized JSON. The existing
index limits each file to 2 MiB and the repository to 100,000 supported files.
An exceeded planning limit returns an error with no partial/truncated plan.

## Tests

`tests/unit/test_validation.c` creates and removes isolated temporary fixtures.
It covers normal/test/transitive imports, exact module identities and nested
working directories, deletion nodes, stale-edge removal after reindexing,
deterministic normalization and reopening, stage ordering, build constraints,
cycles, replacements/workspaces, missing and duplicate modules, parse errors,
cgo, unsupported imports, input validation, batching, edge deduplication, and
unreadable module-boundary preservation. It does not run Go, inference, or GPU
code; command execution is tested separately by the verifier integration tests.
