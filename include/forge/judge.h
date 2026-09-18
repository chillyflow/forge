#ifndef FORGE_JUDGE_H
#define FORGE_JUDGE_H
#include "forge/retrieval.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Optional hosted judgment client (TypeSafe "System One"/Jev).
 *
 * A judge answers narrow semantic questions the deterministic layer cannot:
 * it scores candidates or claims, it never generates text and never executes
 * work. It is OFF unless the host explicitly creates one (the CLI --judge
 * grant), and it is fail-open: every transport, service or parse failure
 * returns an error that callers must treat as "no judgment", leaving their own
 * deterministic behavior unchanged. Requests are bounded (candidate count,
 * body size, per-attempt timeout) and retried at most once on transient
 * transport failures. The API key is read from an environment variable at call
 * time and never stored in configuration files or session artifacts. Raw
 * request/response pairs are recorded only when a record directory is
 * configured. Non-Windows builds have no network transport: every call fails
 * with FORGE_ERR_UNSUPPORTED, which is a fail-open outcome. */

#define FORGE_JUDGE_DEFAULT_ENDPOINT "https://api.typesafe.ai"
#define FORGE_JUDGE_DEFAULT_MODEL "jev-latest"
#define FORGE_JUDGE_DEFAULT_KEY_ENV "TYPESAFE_API_KEY"
#define FORGE_JUDGE_DEFAULT_TIMEOUT_MS ((size_t)2000)
#define FORGE_JUDGE_DEFAULT_MAX_CANDIDATES ((size_t)32)
#define FORGE_JUDGE_MAX_CANDIDATES ((size_t)256)
#define FORGE_JUDGE_MIN_TIMEOUT_MS ((size_t)100)
#define FORGE_JUDGE_MAX_TIMEOUT_MS ((size_t)30000)

typedef struct {
    const char *endpoint;    /* Base URL; default FORGE_JUDGE_DEFAULT_ENDPOINT. */
    const char *model;       /* Alias or pinned id; the response's versioned id is recorded. */
    const char *api_key_env; /* Environment variable holding the API key. */
    const char *record_dir;  /* Optional directory for raw request/response artifacts. */
    size_t timeout_ms;       /* Per attempt; 0 selects the default. */
    size_t max_candidates;   /* Cap per rerank request; 0 selects the default. */
    forge_cancel_fn cancelled;
    void *userdata;
    /* Deterministic testing seam: when set, replaces the network transport.
     * The response buffer is malloc-owned by the transport and freed by the
     * judge. No production caller sets this. */
    forge_status (*transport)(const char *request, size_t request_len, char **response,
                              size_t *response_len, void *userdata, forge_error *);
    void *transport_userdata;
} forge_judge_options;

typedef struct forge_judge forge_judge;

forge_judge *forge_judge_create(const forge_judge_options *, forge_error *);
void forge_judge_destroy(forge_judge *);

/* One batched request; one Noul question per candidate. On FORGE_OK every
 * scores[i] holds the probability in [0,1] that candidate i is a strong match
 * for the query. Any error means "no judgment"; callers keep their own order.
 * count above the configured cap fails with LIMIT. */
forge_status forge_judge_rerank(forge_judge *, const char *query, size_t count,
                                const char *const *paths, const char *const *snippets,
                                const char *const *stages, double *scores, forge_error *);

/* Retrieval rerank callback (forge_rerank_fn); userdata is a forge_judge.
 * Fills info on success from the judge's own telemetry. */
forge_status forge_judge_rerank_retrieval(void *userdata, const char *query, size_t count,
                                          const char *const *paths, const char *const *snippets,
                                          const char *const *stages, double *scores,
                                          forge_rerank_info *info, forge_error *);

/* Worst-case wall time of one rerank call: two attempts at the configured
 * timeout plus the retry backoff. Callers pass this as the retrieval options'
 * rerank_budget_ms so callback latency cannot consume the snapshot timeout. */
size_t forge_judge_budget_ms(const forge_judge *);

typedef struct {
    size_t calls, failures, candidates_scored, input_tokens, output_tokens;
    double last_latency_ms, total_latency_ms;
    char last_model[64]; /* Versioned id reported by the most recent successful response. */
} forge_judge_stats;
void forge_judge_metrics(const forge_judge *, forge_judge_stats *);

#ifdef __cplusplus
}
#endif
#endif
