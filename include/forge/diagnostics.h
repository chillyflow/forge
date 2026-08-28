#ifndef FORGE_DIAGNOSTICS_H
#define FORGE_DIAGNOSTICS_H
#include "forge/forge.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FORGE_DIAGNOSTICS_AUTO,
    FORGE_DIAGNOSTICS_GO_TEST,
    FORGE_DIAGNOSTICS_GO_VET,
    FORGE_DIAGNOSTICS_GOLANGCI_LINT,
    FORGE_DIAGNOSTICS_GCC,
    FORGE_DIAGNOSTICS_CLANG,
    FORGE_DIAGNOSTICS_CARGO,
    FORGE_DIAGNOSTICS_PYTEST,
    FORGE_DIAGNOSTICS_GENERIC
} forge_diagnostic_adapter;

typedef struct {
    forge_diagnostic_adapter adapter;
    size_t max_input_bytes; /* 1..16 MiB; the unexamined suffix is reported. */
    size_t max_diagnostics; /* 1..4096; retain the highest-ranked records. */
    size_t max_text_bytes;  /* 64..16384, per rendered text field. */
    size_t max_json_bytes;  /* 4096..16 MiB; never return invalid partial JSON. */
    bool input_truncated;   /* Caller knows that its captured stream was cut. */
} forge_diagnostic_options;

forge_diagnostic_options forge_diagnostics_default_options(void);

/* Parse a byte stream, including embedded NUL/invalid UTF-8, without running
 * tools or opening paths named in diagnostics. NULL options select defaults.
 * The caller owns the returned schema-v1 JSON and releases it with forge_free.
 * Bad input falls back to generic records with explicit malformed/incomplete
 * metadata; invalid options and allocation failures return NULL plus error.
 * Adapter hints disambiguate shared text formats; they do not make arbitrary
 * text a successfully parsed named format. A complete parse is not a test or
 * command success verdict. Raw stream artifacts remain authoritative. */
char *forge_diagnostics_parse(const char *bytes, size_t length,
                              const forge_diagnostic_options *options, forge_error *error);

/* Render normalized schema-v1 JSON into at most byte_budget bytes (64..16 MiB),
 * excluding the terminating NUL. Output is valid UTF-8 and includes an omission
 * marker when clipped. Caller owns the result; visible may be NULL. */
char *forge_diagnostics_render(const char *json, size_t length, size_t byte_budget, size_t *visible,
                               forge_error *error);

#ifdef __cplusplus
}
#endif
#endif
