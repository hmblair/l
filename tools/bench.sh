#!/bin/bash
# bench.sh - wall-clock benchmarks for l's two extreme cases:
#
#   1. l /                      whole-disk listing with no size cache
#   2. l -d2 ~/academic/libs    many git repositories (status + aggregation)
#
# Usage:
#   tools/bench.sh [candidate] [baseline]
#
# candidate defaults to the repo's bin/l. With a baseline binary the script
# reports both plus the ratio and flags candidate > 1.3x baseline as a
# regression; without one it just times the candidate.
#
# Environment:
#   RUNS=N       timed runs per case (default 3, plus 1 warm-up)
#   SKIP_ROOT=1  skip the whole-disk `l /` case
#
# Both cases run under a scratch HOME (no size-cache DB, config copied from
# the repo) so results measure live scans and are reproducible.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_HOME="${BENCH_HOME:-$HOME/.cache/l-bench-home}"
CAND="${1:-$REPO/bin/l}"
BASELINE="${2:-}"
RUNS="${RUNS:-3}"
LIBS="$HOME/academic/libs"

[[ -x "$CAND" ]] || { echo "candidate missing: $CAND" >&2; exit 2; }
[[ -z "$BASELINE" || -x "$BASELINE" ]] || { echo "baseline missing: $BASELINE" >&2; exit 2; }

mkdir -p "$BENCH_HOME/.config/l"
cp "$REPO/config.toml" "$BENCH_HOME/.config/l/config.toml"
rm -rf "$BENCH_HOME/.cache/l"   # no size cache: measure live scans

time_once() { # binary args... -> prints "real" seconds
    local bin="$1"; shift
    local errf="$BENCH_HOME/.time.$$"
    /usr/bin/time -p env -i HOME="$BENCH_HOME" PATH=/usr/bin:/bin TZ=UTC LC_ALL=C \
        "$bin" "$@" >/dev/null 2>"$errf"
    awk '/^real/ {print $2}' "$errf"
    rm -f "$errf"
}

median_time() { # binary args... -> median of $RUNS (after 1 warm-up)
    local bin="$1"; shift
    time_once "$bin" "$@" >/dev/null   # warm-up
    local times=()
    for _ in $(seq 1 "$RUNS"); do
        times+=("$(time_once "$bin" "$@")")
    done
    printf '%s\n' "${times[@]}" | sort -n | awk -v n="$RUNS" 'NR == int((n + 1) / 2)'
}

bench_case() { # label args...
    local label="$1"; shift
    local c
    c=$(median_time "$CAND" "$@")
    if [[ -z "$BASELINE" ]]; then
        printf '%-24s %8ss\n' "$label" "$c"
        return
    fi
    local b ratio verdict
    b=$(median_time "$BASELINE" "$@")
    ratio=$(awk -v b="$b" -v c="$c" 'BEGIN { printf (b > 0) ? "%.2f" : "n/a", c / b }')
    verdict=$(awk -v b="$b" -v c="$c" 'BEGIN { print (b > 0 && c > 1.3 * b) ? "REGRESSION" : "ok" }')
    printf '%-24s baseline %8ss   candidate %8ss   ratio %s  %s\n' \
        "$label" "$b" "$c" "$ratio" "$verdict"
}

echo "candidate: $CAND"
[[ -n "$BASELINE" ]] && echo "baseline:  $BASELINE"
echo "runs: $RUNS (median, 1 warm-up each)"
echo

[[ "${SKIP_ROOT:-0}" = 1 ]] || bench_case "l / (uncached)" /
if [[ -d "$LIBS" ]]; then
    bench_case "l -d2 academic/libs" -d2 "$LIBS"
else
    echo "skip: $LIBS not found"
fi
