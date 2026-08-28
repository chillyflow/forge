# Diagnostic adapters and bounded views

`forge/diagnostics.h` exposes a model-free parser for captured tool output. It
returns owned, normalized JSON; a separate renderer produces a bounded UTF-8
view. Neither API runs commands, reads diagnostic paths, changes permissions,
or writes artifacts. The tool runtime remains responsible for saving the exact
stdout/stderr bytes and making those artifacts available through `expand_output`.

A diagnostic view is evidence about output, not a command or validation verdict.
In particular, an empty diagnostic array does not mean success. Exit status,
timeout, cancellation, actual process start, and verification input snapshots
remain the executor's responsibility.

## C API and ownership

```c
#include "forge/diagnostics.h"
#include <string.h>

forge_error error = {0};
forge_diagnostic_options options = forge_diagnostics_default_options();
options.adapter = FORGE_DIAGNOSTICS_GO_TEST;
options.input_truncated = capture_was_truncated;

char *json = forge_diagnostics_parse(bytes, byte_count, &options, &error);
if (json) {
    size_t visible = 0;
    char *view = forge_diagnostics_render(json, strlen(json), 2048, &visible, &error);
    /* Consume json/view here. Either allocation is released with forge_free. */
    forge_free(view);
    forge_free(json);
}
```

`bytes` may contain NUL, invalid UTF-8, or control bytes. A NULL input is valid
only when the length is zero. NULL options select defaults. The caller retains
ownership of its input and owns each successful result. On invalid arguments,
allocation failure, or an invalid normalized document passed to the renderer,
the API returns NULL and sets the supplied error. The error pointer and the
renderer’s `visible` pointer may be NULL. A successful call does not clear a
previous error; initialize the error before an operation.

Malformed captured output is normally a successful parse with generic records
and explicit metadata. Unknown valid JSON is retained as `generic_json`, with
`unrecognized_json_records` incremented; it is not called malformed merely
because its schema is unsupported. An explicitly generic hint disables named
format detection.

## Recognized formats

Hints disambiguate compatible formats; they do not relabel arbitrary stdout as
a successfully parsed named format. Mixed input can report several adapters.

| Input | Normalized support | Limits and interpretation |
| --- | --- | --- |
| Go `test -json` / `test2json` | Test/package identities, fail events, output fragments, source locations, explicit labelled values, Go stack frames. Fragments are joined separately by package and test. | Test and package terminal events have separate counters. `Time` and `Elapsed` envelope fields do not enter diagnostic identity. Unrecognized actions remain generic. The supported event envelope follows [Go test2json](https://pkg.go.dev/cmd/test2json). |
| Go test text | `--- FAIL:` identities and source diagnostics when the Go hint supplies context. | Plain output lacks the JSON envelope’s package/test association; use JSON for reliable interleaved output. A file-and-line message with no explicit severity keeps severity `unknown`. |
| Go vet text | `file.go:line[:column]: message`, classified as lint with the Go vet hint. | Text does not identify the originating tool on its own. Vet’s analyzer-specific JSON maps are not an implemented named format. |
| golangci-lint | `Issues` JSON records with `FromLinter`, `Text`, `Pos`, optional severity/source lines; classic colon text with the lint hint. | Empty severity remains `unknown`. JSON positions use Go byte columns from one. Format references: [JSON printer](https://github.com/golangci/golangci-lint/blob/main/pkg/printers/json.go), [issue fields](https://github.com/golangci/golangci-lint/blob/main/pkg/result/issue.go), [Go positions](https://pkg.go.dev/go/token#Position). |
| GCC / Clang text | Classic `path:line[:column]: severity: message`, warning options, source/caret details. Windows drive paths are accepted. | Auto detection says `compiler`, since the shared text cannot establish which compiler emitted it. GCC/Clang hints select the corresponding adapter. Default Clang colon columns are bytes from one; GCC text units/origins remain unknown because they are configurable. See [Clang diagnostic formatting](https://clang.llvm.org/docs/UsersManual.html#formatting-of-diagnostics) and [GCC formatting options](https://gcc.gnu.org/onlinedocs/gcc/Diagnostic-Message-Formatting-Options.html). |
| Legacy GCC JSON | Array records with kind/message, caret location, warning option, and child messages. Byte columns are preferred when present; explicit column origin is retained, including zero. | This is the [GCC 13 JSON format](https://gcc.gnu.org/onlinedocs/gcc-13.2.0/gcc/Diagnostic-Message-Formatting-Options.html). It is not a claim that every current GCC version supports that output option. SARIF and compiler MSVC/VI formats are not implemented. |
| Cargo / rustc JSON | Cargo `compiler-message` envelopes and rustc diagnostic objects, code, severity, package ID, first primary source location, labels/source lines, child messages. | Cargo build progress is counted separately. `build-finished: success=true` is not a test verdict. Rustc JSON columns use Unicode scalar positions from one. See [Cargo external tools](https://doc.rust-lang.org/cargo/reference/external-tools.html) and [rustc JSON](https://doc.rust-lang.org/rustc/json.html). |
| Cargo test / Rust text | Failed libtest identities, panic thread/location, literal `left:`/`right:` values, numbered backtrace frames with source locations, rustc code/arrow diagnostics. | A panic thread is recorded as `thread`, not guessed to be a test. Left/right operands are not relabelled expected/actual. Stable Cargo build JSON does not imply a stable JSON test-harness protocol. Text examples follow [the Rust testing chapter](https://doc.rust-lang.org/book/ch11-01-writing-tests.html). |
| pytest text | Failure sections, assertion/error lines, source locations, summary node IDs, and native Python traceback frames. | Assertion operands have no inferred expected/actual direction. A short failure heading and a full summary node ID remain separate observations. Plugin formats and every `--tb` variant are not covered. See [pytest output formats](https://docs.pytest.org/en/stable/how-to/output.html). |
| Generic | Unrecognized lines/JSON, explicit severity prefixes, exit-status text, and unambiguous colon locations. | Generic capture and tail clipping are not named adapters. Numeric-looking negative, zero-line, or overflowing locations do not become source coordinates. Colon-containing filenames other than Windows drive prefixes remain generic because the text is ambiguous. |

ANSI SGR color sequences are removed from the view. Other control bytes and
invalid UTF-8 bytes are escaped visibly as `\xNN`; valid Unicode is retained.
This display conversion is not a reversible encoding: a literal `\x00` and a
NUL can look alike. Their underlying diagnostic identities remain distinct,
and the exact byte artifact is the authority.

Only literal `expected=... actual=...`, `Expected:`, and `Actual:` labels populate
the corresponding fields. Other assertion prose remains in the message/details.
Parsed stacks retain their emitted order. There is no invented classification
of “user code” versus library frames, and no claim that the first frame is the
root cause.

## JSON contract

The root has `schema_version: 1`, `adapter_hint`, detected `adapters`, input and
limit metadata, `summary`, and the ranked `diagnostics` array. Record fields are:

| Field | Meaning |
| --- | --- |
| `adapter`, `format`, `kind`, `severity` | Recognized family, concrete supported syntax, diagnostic category, and normalized severity. Missing severity is `unknown`. |
| `message`, `package`, `test`, `thread`, `code` | Text observed in the supported format; absent values are JSON null. Thread and test identity are separate. |
| `location` | `path`, `line`, `column`, `column_unit`, and `column_origin`. Unknown coordinates/origin are null; unknown column units are `unknown`. Paths are not resolved or opened. |
| `expected`, `actual`, `left`, `right` | Literal labelled values when parsed, otherwise null. |
| `details`, `stack` | Source/note/context strings and parsed stack records. Child compiler notes are details, not fabricated stack frames. |
| `display` | Original text presentation after color/control handling, or null for structured records. |
| `occurrences` | Multiplicity of identical retained diagnostic observations. This is not a unique failing-test count. |
| `fingerprint` | A deterministic 64-bit heuristic fingerprint. It is not cryptographic or a verification result. |
| `truncated`, `bytes_rendered` | Whether the record lost fields/details to limits, and whether byte escaping changed its presentation. |

The primary location is normalized; additional GCC locations, nonselected
rustc spans, and child source locations are not expanded into the main location.
Their omission increments `omitted_details` and marks the report incomplete.
Their full identity is still included in deduplication. Source ranges,
suggestion edits, macro expansion trees, and SARIF relationships are not a
supported replacement for the saved stream.

Records rank by fatal/error/warning/note/info/unknown severity, then diagnostic
versus ordinary output, path and coordinates, package/test/message/code, and
stable identity. The retained set favors higher-ranked records even if they
arrive later. Byte-identical normalized identities are compared directly, not
merged solely because a hash matches. Identity includes content before field
clipping, so differing hidden suffixes are not falsely deduplicated.

The summary counts observed diagnostic candidates, retained records, merged
duplicate occurrences, omissions, clipped fields, ignored progress events, and
Go terminal events. `malformed_records` counts parse/field validation
observations; several can belong to one input record. These are processing
counters, not a test runner’s success totals. `go_pass_events`/`go_fail_events`
include both test and package events; use the separate `test_*` and `package_*`
counters to distinguish them.

## Bounds and incomplete data

| Option | Default | Accepted range |
| --- | --- | --- |
| `max_input_bytes` | 4 MiB | 1–16 MiB |
| `max_diagnostics` | 256 | 1–4096 |
| `max_text_bytes` | 4096 per field | 64–16384 |
| `max_json_bytes` | 1 MiB | 4096–16 MiB |
| Renderer byte budget | Caller supplied | 64–16 MiB |

`input_bytes` records the supplied length; `parsed_bytes` is the prefix admitted
for processing. An excluded suffix or caller-declared capture loss sets
`input_truncated`. Plain lines and line-delimited records have a 256 KiB
processing cap; a complete structured JSON document may instead be parsed
within the total input limit. Go output lines also have the 256 KiB cap, with
at most 64 separate package/test fragment streams. Each diagnostic retains at
most 16 detail strings and 16 frames; compiler child-note recursion stops at
depth eight.

`incomplete` is true for capture/input loss, capped lines/streams, malformed
recognized input, clipped fields, omitted records, or omitted details. A
complete generic parse only means the generic view was retained within these
limits; it does not mean a named format was understood. JSON stays valid even
when a record is too large for the document budget: that record is omitted and
the count is reported. Deduplication counts are for retained identities; the
parser does not keep an unbounded index of discarded output.

Rendering preserves UTF-8 boundaries and reports the actual visible byte count,
excluding the terminating NUL. A shortened or incomplete view carries an
explicit marker, including when space for a Go event summary forces clipping.
The public renderer does not claim to have saved anything. Keep the normalized
JSON alongside the raw stream when omission details matter.

## Runtime compatibility and tests

The internal `fg_compress_output` and `fg_diagnostic_hash` signatures remain
unchanged. Named multi-record output uses the normalized pipeline. The existing
small generic-output contract, generic tail view, and single Go output-event
clipping behavior remain for callers that depend on them. Those legacy views
do not expose all new JSON metadata; use the length-taking API for structured
consumers and raw byte streams.

The hash ignores ordinary output records and Go envelope timing/order. Textual
timing, addresses, and arbitrary application log fields are not universally
normalized. Treat it only as a loop-detection hint, never a security identifier,
a command outcome, or evidence that a test ran.

`tests/unit/test_diagnostics.c` is offline and requires no Go, Rust, Python,
compiler subprocess, model, or GPU. It exercises the supported fixture formats,
interleaved Go fragments, explicit versus unknown metadata, duplicate counts,
ranking, malformed schemas, invalid coordinates, NUL/invalid UTF-8, color
removal, resource limits, renderer byte boundaries, deterministic malformed
byte cases, and the older compressor/hash contracts. Integration with actual
tool invocations is a separate runtime test responsibility.
