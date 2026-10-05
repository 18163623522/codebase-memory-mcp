/*
 * test_test_impact_engine.c — detect_changes scope:"tests" end to end
 * (src/mcp/test_impact_engine*.c) on small real git repositories: a base
 * commit, a change, and the answer for it.
 *
 * Without an admitted coverage map every runner suite runs whole by design;
 * what the static walk found shows in each suite's reasons ("STATIC" for a
 * reached suite, "CHANGED" for a changed one).
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "cli/cli.h"
#include "foundation/compat.h"
#include "foundation/platform.h"
#include "foundation/subprocess.h"
#include "mcp/test_impact_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char git[1024];
    char home[1024];
    char repo[1100];
    char work[1100];
    char log[1100];
} tie_fixture_t;

static bool tie_git(tie_fixture_t *fx, const char *const *tail) {
    const char *argv[32] = {fx->git,
                            "-C",
                            fx->repo,
                            "-c",
                            "commit.gpgSign=false",
                            "-c",
                            "core.autocrlf=false",
                            "-c",
                            "user.name=t",
                            "-c",
                            "user.email=t@example.invalid"};
    size_t n = 11;
    for (size_t i = 0; tail[i] && n + 1 < sizeof(argv) / sizeof(argv[0]); i++) {
        argv[n++] = tail[i];
    }
    argv[n] = NULL;
    cbm_proc_opts_t opts = {.bin = fx->git,
                            .argv = argv,
                            .log_file = fx->log,
                            .strip_git_repo_env = true,
                            .quiet_timeout_ms = 20000};
    cbm_proc_result_t result = {0};
    return cbm_subprocess_run(&opts, &result) == 0 && result.outcome == CBM_PROC_CLEAN &&
           result.exit_code == 0;
}

static bool tie_write(tie_fixture_t *fx, const char *rel, const char *content) {
    return th_write_file(TH_PATH(fx->repo, rel), content) == 0;
}

static bool tie_commit(tie_fixture_t *fx, const char *message) {
    const char *add[] = {"add", "-A", NULL};
    const char *commit[] = {"commit", "--quiet", "-m", message, NULL};
    return tie_git(fx, add) && tie_git(fx, commit);
}

static const char tie_config[] =
    "{\"test_impact\":{\"version\":1,\"tests\":{\"presets\":{\"c-cbm\":true}},"
    "\"rules\":[{\"id\":\"build\",\"paths\":[\"Makefile\"],\"action\":\"run_all\"}]}}\n";

static const char tie_main[] = "#include \"test_framework.h\"\n"
                               "int main(int argc, char **argv) {\n"
                               "    RUN_SELECTED_SUITE(alpha);\n"
                               "    RUN_SELECTED_SUITE(beta);\n"
                               "    RUN_SELECTED_SUITE(matrix);\n"
                               "    return 0;\n"
                               "}\n";

static const char tie_alpha[] = "int lib_value(void);\n"
                                "TEST(alpha_uses_lib) {\n"
                                "    return lib_value() == 1 ? 0 : 1;\n"
                                "}\n"
                                "TEST(alpha_plain) {\n"
                                "    return 0;\n"
                                "}\n"
                                "SUITE(alpha) {\n"
                                "    RUN_TEST(alpha_uses_lib);\n"
                                "    RUN_TEST(alpha_plain);\n"
                                "}\n";

static const char tie_beta[] = "TEST(beta_alone) {\n"
                               "    return 0;\n"
                               "}\n"
                               "SUITE(beta) {\n"
                               "    RUN_TEST(beta_alone);\n"
                               "}\n";

/* Tests registered by a macro, as the cbm repro matrices do. */
static const char tie_matrix[] = "int lib_other(void);\n"
                                 "TEST(matrix_one) {\n"
                                 "    return lib_other() == 2 ? 0 : 1;\n"
                                 "}\n"
                                 "#define MATRIX_CASES(X) X(matrix_one)\n"
                                 "#define RUN_ONE(n) RUN_TEST(n);\n"
                                 "SUITE(matrix) {\n"
                                 "    MATRIX_CASES(RUN_ONE)\n"
                                 "}\n";

static const char tie_lib[] = "int lib_value(void) {\n"
                              "    return 1;\n"
                              "}\n"
                              "int lib_other(void) {\n"
                              "    return 2;\n"
                              "}\n";

static bool tie_open_with(tie_fixture_t *fx, const char *extra_path, const char *extra);

static bool tie_open(tie_fixture_t *fx) {
    return tie_open_with(fx, NULL, NULL);
}

/* The fixture, plus one more file in the base commit. */
static bool tie_open_with(tie_fixture_t *fx, const char *extra_path, const char *extra) {
    memset(fx, 0, sizeof(*fx));
    const char *git = cbm_find_cli("git", cbm_get_home_dir());
    const char *home = th_mktempdir("cbm-ti-engine");
    char real[1024];
    if (!git || !home || !realpath(home, real) || strlen(git) >= sizeof(fx->git)) {
        return false;
    }
    snprintf(fx->git, sizeof(fx->git), "%s", git);
    snprintf(fx->home, sizeof(fx->home), "%s", real);
    snprintf(fx->repo, sizeof(fx->repo), "%s/repo", real);
    snprintf(fx->work, sizeof(fx->work), "%s/work", real);
    snprintf(fx->log, sizeof(fx->log), "%s/git.log", real);
    const char *init[] = {"-c",      "init.templateDir=",     "init",
                          "--quiet", "--initial-branch=main", NULL};
    const char *topic[] = {"checkout", "--quiet", "-b", "topic", NULL};
    return th_mkdir_p(fx->repo) == 0 && th_mkdir_p(fx->work) == 0 && chmod(fx->work, 0700) == 0 &&
           tie_git(fx, init) && tie_write(fx, ".codebase-memory.json", tie_config) &&
           tie_write(fx, "tests/test_main.c", tie_main) &&
           tie_write(fx, "tests/test_alpha.c", tie_alpha) &&
           tie_write(fx, "tests/test_beta.c", tie_beta) &&
           tie_write(fx, "tests/test_matrix.c", tie_matrix) &&
           tie_write(fx, "src/lib.c", tie_lib) && tie_write(fx, "Makefile", "all:\n") &&
           (!extra_path || tie_write(fx, extra_path, extra)) && tie_commit(fx, "base") &&
           tie_git(fx, topic);
}

static void tie_close(tie_fixture_t *fx) {
    (void)th_rmtree(fx->home);
}

/* The answer for the topic branch against main, as JSON (owned). */
static char *tie_answer(tie_fixture_t *fx) {
    cbm_test_impact_request_t rq = {.repo_root = fx->repo,
                                    .base_ref = "main",
                                    .work_parent = fx->work,
                                    .deadline_ms = cbm_now_ms() + 120000U};
    cbm_test_result_t *out = NULL;
    char diagnostic[512];
    if (cbm_test_impact_run(&rq, &out, diagnostic, sizeof(diagnostic)) != CBM_TEST_IMPACT_OK) {
        printf("  engine: %s\n", diagnostic);
        return NULL;
    }
    if (diagnostic[0]) {
        printf("  engine note: %s\n", diagnostic);
    }
    size_t len = 0;
    const char *json = cbm_test_result_json(out, &len);
    char *copy = json ? strdup(json) : NULL;
    cbm_test_result_free(out);
    return copy;
}

/* The "reasons" array of one suite's entry in the answer. */
static bool tie_suite_has(const char *json, const char *suite, const char *reason) {
    char key[128];
    snprintf(key, sizeof(key), "\"suite\":\"%s\"", suite);
    const char *at = strstr(json, key);
    if (!at) {
        return false;
    }
    const char *end = strstr(at, "\"suite\":\"");
    end = end && end != at ? end : strstr(at + strlen(key), "\"suite\":\"");
    char want[128];
    snprintf(want, sizeof(want), "\"%s\"", reason);
    const char *hit = strstr(at, want);
    return hit && (!end || hit < end);
}

/* A change the walk follows to one test's suite marks that suite reached;
 * a suite it does not reach is not. */
TEST(test_impact_engine_reached_suites_carry_static) {
    tie_fixture_t fx;
    ASSERT_TRUE(tie_open(&fx));
    ASSERT_TRUE(tie_write(&fx, "src/lib.c",
                          "int lib_value(void) {\n    return 1 + 0;\n}\n"
                          "int lib_other(void) {\n    return 2;\n}\n"));
    ASSERT_TRUE(tie_commit(&fx, "change lib_value"));
    char *json = tie_answer(&fx);
    ASSERT_NOT_NULL(json);
    if (!tie_suite_has(json, "alpha", "STATIC")) {
        printf("  %.600s\n", json);
    }
    ASSERT_TRUE(tie_suite_has(json, "alpha", "STATIC"));
    ASSERT_FALSE(tie_suite_has(json, "beta", "STATIC"));
    ASSERT_FALSE(tie_suite_has(json, "matrix", "STATIC"));
    free(json);
    tie_close(&fx);
    PASS();
}

/* A test a macro registers has no explicit registration: reaching it must
 * still select its suite (whole), as the measured reference does. */
TEST(test_impact_engine_macro_registered_tests_select_their_suite) {
    tie_fixture_t fx;
    ASSERT_TRUE(tie_open(&fx));
    ASSERT_TRUE(tie_write(&fx, "src/lib.c",
                          "int lib_value(void) {\n    return 1;\n}\n"
                          "int lib_other(void) {\n    return 2 + 0;\n}\n"));
    ASSERT_TRUE(tie_commit(&fx, "change lib_other"));
    char *json = tie_answer(&fx);
    ASSERT_NOT_NULL(json);
    if (!tie_suite_has(json, "matrix", "STATIC")) {
        printf("  %.900s\n", json);
    }
    ASSERT_TRUE(tie_suite_has(json, "matrix", "STATIC"));
    ASSERT_FALSE(tie_suite_has(json, "alpha", "STATIC"));
    free(json);
    tie_close(&fx);
    PASS();
}

/* A path a run-all rule matches runs everything; so does a change the
 * test-runner lane cannot read (M-10); an empty change selects nothing. */
TEST(test_impact_engine_rules_unmapped_and_empty) {
    tie_fixture_t fx;
    ASSERT_TRUE(tie_open(&fx));
    char *json = tie_answer(&fx);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"decision\":\"nothing\""));
    free(json);

    ASSERT_TRUE(tie_write(&fx, "Makefile", "all:\n\techo\n"));
    ASSERT_TRUE(tie_commit(&fx, "build change"));
    json = tie_answer(&fx);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"decision\":\"run_all\""));
    ASSERT_NOT_NULL(strstr(json, "RULE_RUN_ALL"));
    free(json);

    const char *reset[] = {"reset", "--quiet", "--hard", "main", NULL};
    ASSERT_TRUE(tie_git(&fx, reset));
    ASSERT_TRUE(tie_write(&fx, "data/table.csv", "a,b\n1,2\n"));
    ASSERT_TRUE(tie_commit(&fx, "runtime data"));
    json = tie_answer(&fx);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "UNMAPPED_FILE"));
    free(json);
    tie_close(&fx);
    PASS();
}

/* A parse gap hides the edges written in its lines: a helper whose broken
 * line calls the changed function has no CALLS edge to it. The gap's text
 * still names the function, so the helper is reached, and with it the test
 * that calls the helper (per-file PARSE_GAP rule, user decision 2026-10-04). */
TEST(test_impact_engine_parse_gaps_reach_what_they_name) {
    static const char gapped[] = "int lib_value(void);\n"
                                 "static int helper(void) {\n"
                                 "    int x = 0;\n"
                                 "    x = (lib_value() ;\n"
                                 "    return x;\n"
                                 "}\n"
                                 "TEST(gap_case) {\n"
                                 "    return helper();\n"
                                 "}\n"
                                 "SUITE(beta) {\n"
                                 "    RUN_TEST(gap_case);\n"
                                 "}\n";
    tie_fixture_t fx;
    ASSERT_TRUE(tie_open_with(&fx, "tests/test_beta.c", gapped));
    ASSERT_TRUE(tie_write(&fx, "src/lib.c",
                          "int lib_value(void) {\n    return 1 + 0;\n}\n"
                          "int lib_other(void) {\n    return 2;\n}\n"));
    ASSERT_TRUE(tie_commit(&fx, "change lib_value"));
    char *json = tie_answer(&fx);
    ASSERT_NOT_NULL(json);
    if (!tie_suite_has(json, "beta", "STATIC")) {
        printf("  %.900s\n", json);
    }
    ASSERT_TRUE(tie_suite_has(json, "beta", "STATIC"));
    free(json);
    tie_close(&fx);
    PASS();
}

SUITE(test_impact_engine) {
    RUN_TEST(test_impact_engine_reached_suites_carry_static);
    RUN_TEST(test_impact_engine_macro_registered_tests_select_their_suite);
    RUN_TEST(test_impact_engine_rules_unmapped_and_empty);
    RUN_TEST(test_impact_engine_parse_gaps_reach_what_they_name);
}
