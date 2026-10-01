#!/usr/bin/env bash
# Per-test selection in the C test runner: CBM_TEST_ONLY / CBM_TEST_ONLY_FILE.
#
# The test-impact selector hands the runner `suite:test` tokens. Two properties
# make that safe to gate on, and both are process-level behaviour a suite cannot
# assert about the runner that is executing it:
#   1. exactly the named tests run: the other tests of a named suite are counted
#      as deselected, and a suite no token names never executes its body;
#   2. a token that matches nothing is an ERROR. A selection that quietly ran
#      fewer tests than it named would report green for work that never ran.
#
# Every case drives the built runner over two small in-memory suites, so the
# whole script is a dozen runner starts. Expected counts are read from the suite
# sources, never hard-coded, so adding a test to either suite cannot stale them.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# scripts/test.sh builds into $BUILD_DIR, which is not build/c on every leg; it
# passes the runner it built. The default keeps a bare manual invocation working.
RUNNER="${CBM_TEST_RUNNER:-${ROOT}/build/c/test-runner}"
if [[ ! -x "${RUNNER}" && -x "${RUNNER}.exe" ]]; then
    RUNNER="${RUNNER}.exe"
fi
if [[ ! -x "${RUNNER}" ]]; then
    echo "missing test runner: ${RUNNER}" >&2
    exit 2
fi

# A selection inherited from the caller would decide every case below.
unset CBM_TEST_ONLY CBM_TEST_ONLY_FILE

NAMED_SUITE=dyn_array
OTHER_SUITE=str_intern
UNKNOWN_MESSAGE="selected test not compiled into this build or unknown:"

run_test_count() {
    awk '/^[[:space:]]*RUN_TEST\(/ { n++ } END { print n + 0 }' "$1"
}
first_run_test() {
    awk -F'[()]' '/^[[:space:]]*RUN_TEST\(/ { print $2; exit }' "$1"
}
NAMED_TOTAL="$(run_test_count "${ROOT}/tests/test_${NAMED_SUITE}.c")"
OTHER_TOTAL="$(run_test_count "${ROOT}/tests/test_${OTHER_SUITE}.c")"
OTHER_TEST="$(first_run_test "${ROOT}/tests/test_${OTHER_SUITE}.c")"
if [[ "${NAMED_TOTAL}" -lt 3 || "${OTHER_TOTAL}" -lt 1 || -z "${OTHER_TEST}" ]]; then
    echo "fixture suites changed shape: ${NAMED_SUITE}=${NAMED_TOTAL} ${OTHER_SUITE}=${OTHER_TOTAL}" >&2
    exit 2
fi

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/cbm-test-only.XXXXXX")"
trap 'rm -rf "${tmpdir}"' EXIT

FAILURES=0
RC=0
OUT=""

# run <case> [suite...] — runs the runner under the caller's VAR=VAL prefix and
# records its merged output and exit status. CR is stripped: the Windows CRT
# ends every stdout line with CRLF.
run() {
    local name="$1"
    shift
    OUT="${tmpdir}/${name}.out"
    set +e
    "${RUNNER}" "$@" > "${OUT}.raw" 2>&1
    RC=$?
    set -e
    tr -d '\r' < "${OUT}.raw" > "${OUT}"
}

# The summary line, e.g. "  1 passed, 25 deselected".
summary_count() {
    awk -v label="$1" '
        /^  [0-9]+ passed/ {
            n = 0
            count = split($0, part, ", ")
            for (i = 1; i <= count; i++) {
                gsub(/^ +/, "", part[i])
                if (split(part[i], field, " ") == 2 && field[2] == label) n = field[1]
            }
            found = 1
        }
        END { print found ? n : "none" }
    ' "${OUT}"
}
pass_lines() {
    awk '/PASS$/ { n++ } END { print n + 0 }' "${OUT}"
}
has_line() {
    grep -F -- "$1" "${OUT}" > /dev/null
}

CASE=""
CASE_OK=1
begin() {
    CASE="$1"
    CASE_OK=1
}
expect() {
    local what="$1" actual="$2" wanted="$3"
    if [[ "${actual}" != "${wanted}" ]]; then
        echo "  FAIL ${CASE}: ${what} = ${actual}, expected ${wanted}"
        CASE_OK=0
    fi
}
expect_nonzero_exit() {
    if [[ "${RC}" -eq 0 ]]; then
        echo "  FAIL ${CASE}: runner exited 0, expected a nonzero exit"
        CASE_OK=0
    fi
}
expect_line() {
    if ! has_line "$1"; then
        echo "  FAIL ${CASE}: output lacks: $1"
        CASE_OK=0
    fi
}
expect_no_line() {
    if has_line "$1"; then
        echo "  FAIL ${CASE}: output unexpectedly has: $1"
        CASE_OK=0
    fi
}
finish() {
    if [[ "${CASE_OK}" -eq 1 ]]; then
        echo "  ok   ${CASE}"
    else
        FAILURES=$((FAILURES + 1))
        echo "  ---- ${CASE}: runner exit ${RC}, output tail ----"
        tail -6 "${OUT}" | sed 's/^/  | /'
    fi
}

echo "per-test selection: ${RUNNER}"

# ── No filter: every test of the requested suites runs, nothing is deselected ──
begin "no filter runs every test"
run unfiltered "${NAMED_SUITE}" "${OTHER_SUITE}"
expect "exit status" "${RC}" 0
expect "passed" "$(summary_count passed)" "$((NAMED_TOTAL + OTHER_TOTAL))"
expect "PASS lines" "$(pass_lines)" "$((NAMED_TOTAL + OTHER_TOTAL))"
expect_no_line "deselected"
finish

# An empty variable is the shell's spelling of "unset": same bytes as no filter.
begin "empty variables are no filter"
CBM_TEST_ONLY="" CBM_TEST_ONLY_FILE="" run empty "${NAMED_SUITE}" "${OTHER_SUITE}"
expect "exit status" "${RC}" 0
if ! cmp -s "${tmpdir}/unfiltered.out" "${OUT}"; then
    echo "  FAIL ${CASE}: output differs from the unfiltered run"
    CASE_OK=0
fi
finish

# ── CBM_TEST_ONLY: exactly the named test; the rest of its suite is deselected,
# and a requested suite that no token names does not execute at all ──
begin "CBM_TEST_ONLY runs exactly the named test"
CBM_TEST_ONLY="${NAMED_SUITE}:da_last" run only "${NAMED_SUITE}" "${OTHER_SUITE}"
expect "exit status" "${RC}" 0
expect "passed" "$(summary_count passed)" 1
expect "deselected" "$(summary_count deselected)" "$((NAMED_TOTAL - 1))"
expect "PASS lines" "$(pass_lines)" 1
expect_line "  da_last "
expect_no_line "=== ${OTHER_SUITE} ==="
finish

# ── Strict: a token that matches nothing fails the run and is named ──
begin "unknown test token fails and is named"
CBM_TEST_ONLY="${NAMED_SUITE}:da_last,${NAMED_SUITE}:no_such_test" run unknown-test "${NAMED_SUITE}"
expect_nonzero_exit
expect_line "${UNKNOWN_MESSAGE} ${NAMED_SUITE}:no_such_test"
expect "passed" "$(summary_count passed)" 1
finish

begin "unknown suite token fails and is named"
CBM_TEST_ONLY="no_such_suite:da_last" run unknown-suite "${NAMED_SUITE}"
expect_nonzero_exit
expect_line "${UNKNOWN_MESSAGE} no_such_suite:da_last"
expect "PASS lines" "$(pass_lines)" 0
finish

# ── Argv keeps selecting suites; the filter narrows inside that selection. A
# token for a suite this process was not asked to run is out of its scope, not
# an error: the parallel harness gives every per-suite process the whole list ──
begin "token outside the argv selection is not an error"
CBM_TEST_ONLY="${NAMED_SUITE}:da_last,${OTHER_SUITE}:${OTHER_TEST}" run argv-scope "${NAMED_SUITE}"
expect "exit status" "${RC}" 0
expect "passed" "$(summary_count passed)" 1
expect_no_line "=== ${OTHER_SUITE} ==="
finish

# ── CBM_TEST_ONLY_FILE: one token per line; blank lines, comments, CRLF ──
selection="${tmpdir}/selection.txt"
printf '%s\n' "# written by the test-impact selector" "" \
    "${NAMED_SUITE}:da_last" "   ${NAMED_SUITE}:da_clear   # trailing comment" > "${selection}"
printf '%s\r\n' "${NAMED_SUITE}:da_free" >> "${selection}"

begin "CBM_TEST_ONLY_FILE runs exactly the listed tests"
CBM_TEST_ONLY_FILE="${selection}" run file "${NAMED_SUITE}" "${OTHER_SUITE}"
expect "exit status" "${RC}" 0
expect "passed" "$(summary_count passed)" 3
expect "deselected" "$(summary_count deselected)" "$((NAMED_TOTAL - 3))"
expect "PASS lines" "$(pass_lines)" 3
expect_no_line "=== ${OTHER_SUITE} ==="
finish

begin "CBM_TEST_ONLY and CBM_TEST_ONLY_FILE select their union"
CBM_TEST_ONLY="${OTHER_SUITE}:${OTHER_TEST}" CBM_TEST_ONLY_FILE="${selection}" \
    run union "${NAMED_SUITE}" "${OTHER_SUITE}"
expect "exit status" "${RC}" 0
expect "passed" "$(summary_count passed)" 4
expect "deselected" "$(summary_count deselected)" "$((NAMED_TOTAL + OTHER_TOTAL - 4))"
finish

printf '%s\n' "${NAMED_SUITE}:da_last" "${NAMED_SUITE}:no_such_test" > "${tmpdir}/unknown.txt"
begin "unknown test token in the file fails and is named"
CBM_TEST_ONLY_FILE="${tmpdir}/unknown.txt" run file-unknown "${NAMED_SUITE}"
expect_nonzero_exit
expect_line "${UNKNOWN_MESSAGE} ${NAMED_SUITE}:no_such_test"
finish

# ── Fail closed: a selection that cannot be read, names nothing, or is not a
# token must never fall back to running everything or nothing in silence ──
begin "unreadable selection file fails before any test"
CBM_TEST_ONLY_FILE="${tmpdir}/does-not-exist.txt" run file-missing "${NAMED_SUITE}"
expect_nonzero_exit
expect_line "does-not-exist.txt"
expect "PASS lines" "$(pass_lines)" 0
finish

printf '%s\n' "# nothing selected" "" > "${tmpdir}/empty.txt"
begin "selection file naming no test fails before any test"
CBM_TEST_ONLY_FILE="${tmpdir}/empty.txt" run file-empty "${NAMED_SUITE}"
expect_nonzero_exit
expect "PASS lines" "$(pass_lines)" 0
finish

begin "malformed token fails before any test"
CBM_TEST_ONLY="${NAMED_SUITE}" run malformed "${NAMED_SUITE}"
expect_nonzero_exit
expect_line "malformed test selection token: ${NAMED_SUITE}"
expect "PASS lines" "$(pass_lines)" 0
finish

if [[ "${FAILURES}" -gt 0 ]]; then
    echo "FAIL: ${FAILURES} per-test selection case(s) failed"
    exit 1
fi
echo "PASS: per-test selection is exact, strict, and inert without a filter"
