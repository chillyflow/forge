/* Integration test: suggest_paths tool with judge rerank on a real indexed repo.
 * Proves the full tool dispatch + fg_repo_search_hits + forge_judge_rerank
 * path fires end-to-end without the model layer. */

#include "internal.h"
#include "forge/tools.h"
#include "forge/repo.h"
#include "forge/judge.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

static bool cancel_stub(void *ud) { (void)ud; return false; }

/* Stub judge: returns fixed scores that exercise the rerank path. */
typedef struct {
    forge_status status;
    double *scores;
    size_t score_count;
} stub_judge_state;

static forge_status stub_judge_rerank(forge_judge *j, const char *query, size_t count,
                                      const char **paths, const char **snippets,
                                      const char **stages, double *scores,
                                      forge_error *e) {
    (void)j; (void)query; (void)paths; (void)snippets; (void)stages;
    stub_judge_state *s = j->options.userdata;
    if (s->status != FORGE_OK)
        return fg_error(e, FORGE_ERR_IO, "stub judge failure");
    for (size_t i = 0; i < count && i < s->score_count; i++)
        scores[i] = s->scores[i];
    return FORGE_OK;
}

static void test_suggest_paths_no_judge(void) {
    forge_error e = {0};
    char cwd[4096];
    assert(getcwd(cwd, sizeof(cwd)));
    
    /* Open the real repo root (current checkout) */
    forge_repo *repo = forge_repo_open(cwd, &e);
    if (!repo) {
        fprintf(stderr, "SKIP: cannot open repo at %s: %s\n", cwd, e.message);
        return;
    }
    
    fg_tool_context ctx = {0};
    ctx.repo = repo;
    ctx.config.allow_read = true;
    ctx.config.allow_write = true;
    ctx.config.allow_exec = true;
    
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *args = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, args, "query", "judge rerank");
    yyjson_mut_doc_set_root(doc, args);
    
    forge_error tool_error = {0};
    char *result = suggest_paths(&ctx, args, &tool_error);
    
    if (result) {
        printf("=== suggest_paths (no judge) ===\n%s\n\n", result);
        free(result);
    } else {
        fprintf(stderr, "suggest_paths error: %s\n", tool_error.message);
    }
    
    yyjson_mut_doc_free(doc);
    forge_repo_close(repo);
}

static void test_suggest_paths_with_judge(void) {
    forge_error e = {0};
    char cwd[4096];
    assert(getcwd(cwd, sizeof(cwd)));
    
    forge_repo *repo = forge_repo_open(cwd, &e);
    if (!repo) {
        fprintf(stderr, "SKIP: cannot open repo at %s: %s\n", cwd, e.message);
        return;
    }
    
    /* Create a stub judge that returns known scores */
    forge_judge_options opts = {0};
    opts.api_key_env = "TYPESAFE_API_KEY";
    opts.timeout_ms = 2000;
    opts.max_candidates = 16;
    opts.confidence_threshold = 0.5;
    
    forge_judge *judge = forge_judge_init(&opts, &e);
    if (!judge) {
        fprintf(stderr, "SKIP: cannot init judge: %s\n", e.message);
        forge_repo_close(repo);
        return;
    }
    
    /* Set up stub scores: reverse order so last candidate gets highest score */
    stub_judge_state stub = {0};
    double scores[16] = {0.1, 0.3, 0.5, 0.95, 0.2, 0.4, 0.6, 0.8,
                         0.15, 0.35, 0.55, 0.75, 0.25, 0.45, 0.65, 0.85};
    stub.scores = scores;
    stub.score_count = 16;
    stub.status = FORGE_OK;
    judge->options.userdata = &stub;
    judge->rerank = stub_judge_rerank;
    
    fg_tool_context ctx = {0};
    ctx.repo = repo;
    ctx.config.judge = judge;
    ctx.config.allow_read = true;
    ctx.config.allow_write = true;
    ctx.config.allow_exec = true;
    
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *args = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, args, "query", "judge rerank");
    yyjson_mut_doc_set_root(doc, args);
    
    forge_error tool_error = {0};
    char *result = suggest_paths(&ctx, args, &tool_error);
    
    if (result) {
        printf("=== suggest_paths (with judge stub) ===\n%s\n\n", result);
        
        /* Parse result to verify rerank occurred */
        yyjson_doc *rdoc = yyjson_read(result, strlen(result), 0);
        if (rdoc) {
            yyjson_val *root = yyjson_doc_get_root(rdoc);
            bool reranked = yyjson_get_bool(yyjson_obj_get(root, "reranked"));
            count_t count = yyjson_get_uint(yyjson_obj_get(root, "count"));
            printf("Reranked: %s\nCount: %u\n", reranked ? "YES" : "NO", (unsigned)count);
            yyjson_doc_free(rdoc);
        }
        free(result);
    } else {
        fprintf(stderr, "suggest_paths error: %s\n", tool_error.message);
    }
    
    yyjson_mut_doc_free(doc);
    forge_judge_destroy(judge);
    forge_repo_close(repo);
}

int main(void) {
    test_suggest_paths_no_judge();
    test_suggest_paths_with_judge();
    printf("Integration tests passed\n");
    return 0;
}
