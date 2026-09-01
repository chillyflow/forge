# Go/Python staged validation

Forge plans validation from indexed Go packages and Python files instead of
asking the model to invent each command. The planner is a library operation:
**it does not execute commands, invoke a language runtime, or refresh the
index**. A returned plan is not a test result.

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

## Python discovery

Python validation is applicable to known changed `.py` files and pytest
configuration changes. An empty/unknown change set enables broad Python
planning when indexed `.py` files exist. Incidental Python files do not block a
known Go-only change. A changed `.py` path remains applicable when that path was
deleted or is absent from the index. The planner searches executable `python3`,
`python`, and (on Windows) `py` entries using the same outside-workspace PATH
rules as the process runner. If Python is applicable but no interpreter is
available, no current Python file can be checked, or no unittest/pytest test
surface is detected, the plan is `blocked`, contains no commands, and must not
be interpreted as a successful or inapplicable verification. Syntax-only
evidence is deliberately insufficient for a passing workspace verdict.

Syntax selection is exact: indexed changed Python files are checked after a
narrow Python change. An empty change list, or a change set with no current
Python syntax target while Python is applicable, checks every indexed Python
file. The command invokes normal in-memory `compile` on source bytes without
importing or executing project code, so compiler-level errors such as a
module-level `return` are rejected. `-B` prevents bytecode writes.

Python has no dependency graph yet. A changed `module.py` selects indexed
`test_module.py` and `module_test.py` files by basename; a changed test selects
itself. Broad discovery remains required after the targeted command. Indexed
`pytest.ini`/`.pytest.ini`, `conftest.py`, pytest settings in `pyproject.toml`,
`setup.cfg`, or `tox.ini`, pytest imports, and plain test functions outside
unittest modules select pytest. Otherwise, `test*.py` and `*_test.py` files use
the standard-library unittest runner. Forge passes every indexed unittest file
explicitly, including files below package-less test directories, and treats a
runner that loads zero tests as a failure. A repository with no detected tests
is blocked because syntax alone cannot establish test success.

## Schedule

All six stages are emitted in this order. Empty stages are allowed.

| Stage | Scope and command |
| --- | --- |
| `format` | Indexed changed Go files, or all eligible indexed Go files when the change set is unknown: `gofmt -l ./path.go` |
| `compile` | Present affected Go packages: `go test -json -count=1 -vet=off -run ^$ ./pkg`; selected Python files: `python -B -c <in-memory compile> ./path.py` |
| `affected_tests` | Present affected Go packages: `go test -json -count=1 -vet=off ./pkg`; related Python test files through pytest or unittest |
| `dependent_tests` | Transitive reverse importers: `go test -json -count=1 -vet=off ./consumer` |
| `vet` | Affected packages and reverse importers: `go vet ./pkg ./consumer` |
| `broad_tests` | Each indexed Go module: `go test -json -count=1 ./...`; Python: `python -B -m pytest -q -p no:cacheprovider` or a batched `python -B -c <unittest runner> ./test_file.py ...` over every indexed unittest file; the runner fails if it loads zero tests |

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
For Python, the same empty list syntax-checks every indexed `.py` file and then
runs broad indexed test verification when tests are applicable.

Packages are sorted by workspace-relative directory, grouped by module, and
batched into at most 32 targets and 12,000 target argument bytes per command.
Each argument remains a separate JSON string; no shell command is constructed.
Package/file arguments use `.` or `./...` prefixes, including names that begin
with `-`. `cwd` is workspace-relative, with `.` denoting the root. `-count=1`
avoids reporting a cached test result as a fresh run. The compile stage does not
run matching test functions, but **can execute package initialization and
`TestMain`**; it is not a sandboxed or execution-free compiler operation.
Python syntax and test-file arguments use the same batch bounds. At most 1,024
indexed Python test files and 1,024 related targets are retained; exceeding a
bound fails planning instead of returning a truncated target set.

## JSON contract, schema version 2

Top-level fields:

| Field | Meaning |
| --- | --- |
| `schema_version`, `generation` | Plan format and indexed repository generation |
| `language`, `status` | Primary language (`go` remains primary in mixed plans) and `planned`, `blocked`, or `not_applicable` |
| `languages` | Applicable languages, in deterministic `go`, `python` order |
| `verification_status`, `verification_available` | Explicit plan readiness; unavailable/irrelevant validation is never a passing result |
| `missing_tools` | Required executable families that could not be resolved |
| `graph_kind`, `sound` | `syntactic_package_imports` when Go applies, otherwise `none`; `sound` is always `false` |
| `applicable` | Whether Go or Python inputs require a schedule |
| `broad_verification_required` | True for applicable repositories |
| `changed_paths` | Normalized, unique, sorted file paths |
| `modules` | `directory`, nullable `module_path`, and `synthetic` |
| `packages` | `directory`, nullable `import_path`, `module_directory`, `present`, `has_tests`, `affected`, `dependent` |
| `edges` | Importer `from`, imported package `to`, and `test_only`; paths identify nodes |
| `affected_packages`, `reverse_dependents` | Sorted, disjoint directory lists |
| `fallback_reasons` | Sorted `{code, path, detail}` records, one representative path per category |
| `limitations` | Explicit interpretation limits |
| `python` | Applicability, interpreter and runner detection, source/syntax/test counts, scheduling flags, and targeted test files |
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

A repository with no Go or Python applicability has `applicable: false`,
`verification_status: "not_applicable"`, zero commands, and empty command
arrays in all six stages. This is not a successful verification. A `blocked`
plan remains `applicable: true` but also has zero commands, making missing tools,
checkable inputs, or Python test evidence a fail-closed condition.

## Execution contract

An executor must authorize every subprocess, resolve each `cwd` within the
workspace, pass `argv` directly, and enforce output, wall-clock, command, and
cancellation budgets. Run stages in order and stop on the first failed command.
Do not broaden after local failure. Policy denial, timeout, cancellation,
truncated output, or budget exhaustion must not become a passing verification.
An executor must reject `blocked` plans and must not turn `not_applicable` into
passed verification evidence.

`gofmt -l` can exit successfully while printing unformatted paths. Its
`require_empty_stdout: true` makes that a failed check. The plan never requests
`gofmt -w`. All Go commands can run repository code, access the network and
caches, or update Go metadata as permitted by the active Go environment; process
authorization is required. Formatting permission is not a substitute for
process authorization, and process permission is not an OS sandbox.
Python commands use `-B`; pytest also disables its cache provider. Project code
or plugins can still write other files, which the input snapshot detects.

Before claiming success, an executor should preserve command results and raw
outputs, refresh the index, and ensure that it validated the same generation.
Any changed input requires a fresh plan. The planner itself reports no command
results or pass status.

## Built-in executor and evidence

The built-in executor is available through `forge validate` and
`forge_verify_workspace` in `include/forge/verification.h`. Ordinary agent runs
invoke it before accepting a final answer after edits or arbitrary
`run_command` execution. Such commands can affect unindexed fixtures, so they
request broad validation even when the source generation is unchanged. Explicit
read-only Git inspection and externally refreshed read context do not create a
validation claim by themselves. `--no-auto-validation` disables this gate for
experiments; it does not certify success.

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

## Repeated-action recovery

The agent enters `FORGE_AGENT_RECOVERY` on the second canonical occurrence of
an action in the same repository-generation and diagnostic state. An exact
replay of an already applied patch also enters recovery even though the first
patch advanced the generation. Process actions are compared by context-free
strategy until a real edit or observed external change, because merely launching
a command advances Forge's repository generation. The repeated tool is rejected
without execution; recovery replaces the former fatal repeated-action counter.

The session emits a `recovery` event and the model-visible result includes a
bounded last-edit diff, the latest validation/tool diagnostic, relevant and
changed files, the failed thought or action as a hypothesis, and remaining
turn, generated-token, input-token, and wall-clock budgets. `loop_warnings`
counts recovery episodes, not every rejected turn in one episode.

Recovery remains active until the model proposes a materially different action.
A changed edit, a different file or symbol, or a validation command can proceed.
Changing only the line bounds of a repeated read of the same file, or replaying
the unchanged patch, is rejected again without starting a new episode. Normal
turn, token, cancellation, and wall-clock limits still bound a run. Repeating a
final answer against the same failed validation result follows the same recovery
path instead of terminating early.

On the first dirty-workspace stall, Forge runs validation before assembling the
packet. A blocked or policy-denied plan remains non-passing but contributes its
actionable reason to recovery; cancellation, resource exhaustion, and internal
I/O/parse failures still terminate. The packet preserves both a preceding tool
failure and the stall-validation result. Finalization requires
`verification.passed`; a successful planner call whose result is
`not_applicable` cannot authorize a final answer.

## Conservative fallbacks and limits

Fallback categories include syntax errors; explicit or filename build
constraints; possible import cycles; cgo; relative/escaped imports; unresolved
local imports; missing, invalid, or duplicate module paths; cross-module matches;
replacement directives; Go workspaces; absent/deleted paths; configuration or
unassigned input changes; incomplete indexing; and fallback filesystem
enumeration. A package without a containing indexed module receives a synthetic
root scope. A Go environment may need to be configured before that command can
succeed; Forge does not guess GOPATH or invent a module declaration.

Python fallbacks record absent/deleted changed files, missing interpreters,
missing current syntax inputs, unmatched changed-module/test basenames, and the
absence of detectable tests. Filename matching is a conservative scheduling
hint, not import resolution or test coverage.

Explicit unittest paths still use the standard loader's path-to-module
conversion. A local test namespace that collides with an installed regular
package can require pytest or explicit project configuration; Forge records this
as a limitation rather than claiming the filename heuristic is complete.

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
deduplication, 1,024 indexed Python test files, 1,024 related Python targets,
2,048 planned commands, and 16 MiB of serialized JSON. The existing index limits
each file to 2 MiB and the repository to 100,000 supported files.
An exceeded planning limit returns an error with no partial/truncated plan.

## Tests

`tests/unit/test_validation.c` creates and removes isolated temporary fixtures.
It covers normal/test/transitive imports, exact module identities and nested
working directories, deletion nodes, stale-edge removal after reindexing,
deterministic normalization and reopening, stage ordering, build constraints,
cycles, replacements/workspaces, missing and duplicate modules, parse errors,
cgo, unsupported imports, input validation, batching, edge deduplication, and
unreadable module-boundary preservation. Python fixtures cover compiler syntax
commands, related-test selection, pytest/unittest choice and configuration,
explicit broad unittest files below a package-less directory, mixed-language
plans, Go-only scoping in a mixed repository, and missing-interpreter blocking.
The test does not execute planned Go or Python commands, inference, or GPU code;
command execution is tested separately by the verifier integration tests.
