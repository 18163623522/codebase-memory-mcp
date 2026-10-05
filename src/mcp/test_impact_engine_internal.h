/*
 * test_impact_engine_internal.h — the state of one scope:"tests" request,
 * shared by the engine's steps (test_impact_engine*.c).
 */
#ifndef CBM_TEST_IMPACT_ENGINE_INTERNAL_H
#define CBM_TEST_IMPACT_ENGINE_INTERNAL_H

#include "mcp/test_impact.h"
#include "mcp/test_impact_changes.h"
#include "mcp/test_impact_classify.h"
#include "mcp/test_impact_engine.h"
#include "mcp/test_impact_git.h"
#include "mcp/test_impact_inventory.h"
#include "mcp/test_impact_result.h"
#include "mcp/test_impact_seed.h"
#include "mcp/test_impact_source.h"
#include "mcp/test_impact_tree.h"
#include "discover/test_conventions.h"
#include "foundation/arena.h"
#include "store/store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every rule, fallback, warning and path the request records lives in the
 * arena; dynamic arrays grow through the memory core. */
typedef struct {
    int rule_index;
    bool resolved; /* every path it matched resolved its target */
} te_match_t;

typedef struct {
    const cbm_test_impact_request_t *rq;
    char *diagnostic;
    size_t diagnostic_len;
    CBMArena arena;
    bool oom;

    /* git */
    cbm_git_facts_t *facts;
    const cbm_git_facts_identity_t *id;
    cbm_git_diff_t diff;
    cbm_changes_t *changes;
    bool diff_complete;
    bool has_changes;

    /* query-time configuration (the merge base's, M-4) */
    cbm_test_config_t *config;
    cbm_test_policy_t *policy;

    /* the HEAD snapshot */
    cbm_pinned_tree_t *tree;
    cbm_test_impact_inventory_t *inventory;
    cbm_test_impact_ledger_t *ledger;
    cbm_test_declarations_t *declarations;
    cbm_test_model_t *model;
    char work_dir[4096];
    char candidate_db[4096];
    cbm_store_t *store;
    cbm_store_read_scope_t *scope; /* one stable read of the candidate graph */
    unsigned char graph_sha256[32];
    bool graph_digest;
    cbm_ti_graph_t *graph;
    cbm_ti_source_t *source;
    bool snapshot_ok;

    /* classification of the changed paths */
    te_match_t *matches;
    int match_count;
    int match_cap;
    cbm_test_result_fallback_t *fallbacks;
    int fallback_count;
    int fallback_cap;
    cbm_test_result_warning_t *warnings;
    int warning_count;
    int warning_cap;
    cbm_ti_change_t *seed_changes; /* C-family paths the graph seeding reads */
    int seed_change_count;
    int seed_change_cap;
    const char **fixture_paths; /* paths a `referencing_tests` rule matched */
    int *fixture_rules;         /* their rule index */
    int fixture_rule_cap;
    int fixture_count;
    int fixture_cap;
    bool activation_complete;

    /* selection inputs */
    cbm_ti_seeds_t *seeds;
    cbm_test_reach_t *reach;
    int reach_count;
    cbm_test_suite_trigger_t *triggers;
    int trigger_count;
    int trigger_cap;
    bool static_complete;

    cbm_test_selection_t *selection;
} te_ctx_t;

/* Record a run-all reason once; the first diagnostic wins. false only when
 * out of memory. */
bool te_fallback(te_ctx_t *c, cbm_test_result_fallback_t code, const char *diagnostic);
bool te_grow(void **items, int *cap, int count, size_t size);
bool te_oom(te_ctx_t *c);

/* test_impact_engine_snapshot.c */
bool te_work_dir(te_ctx_t *c);
bool te_snapshot(te_ctx_t *c);
void te_snapshot_free(te_ctx_t *c);

/* test_impact_engine_reach.c */
bool te_fixtures(te_ctx_t *c, int64_t **extra_seeds, int *extra_count, bool **fixture_cases);
bool te_reach(te_ctx_t *c);

#endif /* CBM_TEST_IMPACT_ENGINE_INTERNAL_H */
