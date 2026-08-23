#!/usr/bin/env bash
# Run the max_len_skip performance benchmark: baseline (statistics propagation
# disabled) vs optimized, over the query set, and report wall times, scanned
# row groups and result values.
#
# Usage: ./run_benchmark.sh <db_path> [runs]
#
#   runs  number of timing repetitions per query/mode (default: 3, median used)
#
# The baseline disables the whole statistics propagation pass
# (SET disabled_optimizers='statistics_propagation'); there is no finer-grained
# switch for the aggregate precomputation alone, so the baseline also disables
# the pre-existing count/min/max precomputation.
#
# Set COLD=1 to drop the OS page cache (requires passwordless sudo) before
# every duckdb invocation, measuring cold reads from disk instead of the cache.

set -euo pipefail

DB_PATH=${1:?usage: run_benchmark.sh <db_path> [runs]}
RUNS=${2:-3}
COLD=${COLD:-0}
DUCKDB=${DUCKDB:-build/reldebug/duckdb}

if [[ ! -x "$DUCKDB" ]]; then
    echo "duckdb binary not found at $DUCKDB (set DUCKDB=/path/to/duckdb)" >&2
    exit 1
fi
if [[ ! -f "$DB_PATH" ]]; then
    echo "database not found at $DB_PATH (run generate_data.sh first)" >&2
    exit 1
fi

drop_caches() {
    if [[ "$COLD" == "1" ]]; then
        sync
        sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    fi
}

# name|query
QUERIES=(
    # fully exact stats: zero scan expected
    "max_strlen_concentrated|SELECT max(strlen(s)) FROM len_concentrated"
    "min_strlen_concentrated|SELECT min(strlen(s)) FROM len_concentrated"
    # CSE-lifted shared strlen: min+max in one query exercises the projection conversion
    "minmax_strlen_concentrated|SELECT min(strlen(s)), max(strlen(s)) FROM len_concentrated"
    "max_charlen_concentrated|SELECT max(char_length(s)) FROM len_concentrated"
    "min_charlen_concentrated|SELECT min(char_length(s)) FROM len_concentrated"
    # 10% unicode row groups: MAX scans ~10%, MIN prunes them via ceil(bytes/4)
    "max_charlen_unicode|SELECT max(char_length(s)) FROM len_unicode"
    "min_charlen_unicode|SELECT min(char_length(s)) FROM len_unicode"
    # no exact candidate extreme exists: scans everything, must match baseline
    "max_charlen_unicode_all|SELECT max(char_length(s)) FROM len_unicode_all"
    # issue-style multi-column query
    "multi_charlen|SELECT max(char_length(s1)), max(char_length(s2)), max(char_length(s3)), max(char_length(s4)), max(char_length(s5)), max(char_length(s6)), max(char_length(s7)), max(char_length(s8)) FROM len_multi"
    # non-target queries (regression check)
    "sum_i|SELECT sum(i) FROM len_concentrated"
    "count_star|SELECT count(*) FROM len_concentrated"
)

median() {
    local arr=("$@")
    local n=${#arr[@]}
    local sorted
    sorted=$(printf '%s\n' "${arr[@]}" | sort -n)
    local mid=$(( (n + 1) / 2 ))
    echo "$sorted" | sed -n "${mid}p"
}

run_query() {
    local name=$1 sql=$2 mode=$3
    local pre=""
    if [[ $mode == "baseline" ]]; then
        pre="SET disabled_optimizers='statistics_propagation';"
    fi

    # timing: RUNS repetitions, median wall time in ms
    local times=()
    for _ in $(seq 1 "$RUNS"); do
        local start end
        drop_caches
        start=$(date +%s%N)
        "$DUCKDB" "$DB_PATH" -c "$pre $sql" > /dev/null
        end=$(date +%s%N)
        times+=($(( (end - start) / 1000000 )))
    done
    local median_ms
    median_ms=$(median "${times[@]}")

    # scanned row groups from EXPLAIN ANALYZE (single run); a fully precomputed
    # query has no table scan at all and therefore no Row Groups Scanned text
    local analyzed scanned
    drop_caches
    analyzed=$("$DUCKDB" "$DB_PATH" -c "$pre EXPLAIN ANALYZE $sql" 2>/dev/null | tr '\n' ' ')
    scanned=$(echo "$analyzed" | grep -oE 'Row Groups Scanned: [0-9]+ / [0-9]+' | tail -1 || true)
    if [[ -z "$scanned" ]]; then
        scanned="Row Groups Scanned: 0 (fully precomputed)"
    fi

    # result value (for correctness comparison between modes)
    local result
    drop_caches
    result=$("$DUCKDB" "$DB_PATH" -csv -c "$pre $sql" 2>/dev/null | tail -1)

    echo "$name|$mode|${median_ms}ms|${scanned:-Row Groups Scanned: n/a}|$result"
}

echo "query|mode|median_time|row_groups|result"
for entry in "${QUERIES[@]}"; do
    name=${entry%%|*}
    sql=${entry#*|}
    for mode in baseline optimized; do
        run_query "$name" "$sql" "$mode"
    done
done
