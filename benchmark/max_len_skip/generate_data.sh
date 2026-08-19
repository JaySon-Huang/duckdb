#!/usr/bin/env bash
# Generate benchmark data for the max_len_skip performance benchmark.
#
# Usage: ./generate_data.sh <db_path> [large_rows] [multi_rows]
#
#   db_path     database file to create (default: /tmp/lenbench.duckdb)
#   large_rows  rows for len_concentrated / len_unicode / len_unicode_all (default: 500000000)
#   multi_rows  rows for len_multi (8 string columns) (default: 100000000)
#
# The database is created with the latest storage format so that
# min_string_length / total_string_length survive the checkpoint (required
# for the MIN optimization to work after reopening the database).
#
# Data layout (lengths are distributed per row group, not per row):
#   len_concentrated  - 90% of the row groups hold 10-50 byte ASCII strings,
#                       10% hold 900-1000 byte strings. All row group stats
#                       are exact, so MAX/MIN(strlen/char_length) can be
#                       fully precomputed (zero scan).
#   len_unicode       - 90% all-ASCII row groups (10-50 bytes), 10% hold
#                       multi-byte strings ('你' * 20-50, 60-150 bytes).
#                       MAX(char_length) must scan only the ~10% unicode
#                       row groups; MIN(char_length) prunes them via the
#                       ceil(bytes/4) lower bound (ceil(60/4)=15 >= 10).
#   len_unicode_all   - every row group contains unicode characters: no exact
#                       candidate extreme exists, the query scans everything
#                       (should match the baseline).
#   len_multi         - 8 string columns with the concentrated layout, for the
#                       issue-style query (MAX(CHAR_LENGTH) x 8).

set -euo pipefail

DB_PATH=${1:-/tmp/lenbench.duckdb}
N=${2:-500000000}
NM=${3:-100000000}
DUCKDB=${DUCKDB:-build/reldebug/duckdb}

if [[ ! -x "$DUCKDB" ]]; then
    echo "duckdb binary not found at $DUCKDB (set DUCKDB=/path/to/duckdb)" >&2
    exit 1
fi

echo "== generating data into $DB_PATH =="
echo "   large tables: $N rows, multi table: $NM rows"

rm -f "$DB_PATH"

# The storage version must be set BEFORE the database file is created - opening
# the file first would create it with the default compatibility format, which
# drops min_string_length on checkpoint. Start from an empty in-memory database,
# set the version, then ATTACH to create the file.
"$DUCKDB" -c "
SET storage_compatibility_version='latest';
ATTACH '${DB_PATH}' AS db;
USE db;

CREATE TABLE len_concentrated AS
SELECT i,
       CASE WHEN i < ${N} * 0.9 THEN repeat('a', 10 + (i % 41))
            ELSE repeat('a', 900 + (i % 101)) END AS s
FROM range(${N}) _(i);

CREATE TABLE len_unicode AS
SELECT i,
       CASE WHEN i < ${N} * 0.9 THEN repeat('a', 10 + (i % 41))
            ELSE repeat('你', 20 + (i % 31)) END AS s
FROM range(${N}) _(i);

CREATE TABLE len_unicode_all AS
SELECT i, repeat('你', 1 + (i % 50)) AS s
FROM range(${N}) _(i);

CREATE TABLE len_multi AS
SELECT i,
       CASE WHEN i < ${NM} * 0.9 THEN repeat('a', 10 + (i % 41)) ELSE repeat('a', 900 + (i % 101)) END AS s1,
       CASE WHEN i < ${NM} * 0.9 THEN repeat('a', 10 + (i % 41)) ELSE repeat('a', 900 + (i % 101)) END AS s2,
       CASE WHEN i < ${NM} * 0.9 THEN repeat('a', 10 + (i % 41)) ELSE repeat('a', 900 + (i % 101)) END AS s3,
       CASE WHEN i < ${NM} * 0.9 THEN repeat('a', 10 + (i % 41)) ELSE repeat('a', 900 + (i % 101)) END AS s4,
       CASE WHEN i < ${NM} * 0.9 THEN repeat('a', 10 + (i % 41)) ELSE repeat('a', 900 + (i % 101)) END AS s5,
       CASE WHEN i < ${NM} * 0.9 THEN repeat('a', 10 + (i % 41)) ELSE repeat('a', 900 + (i % 101)) END AS s6,
       CASE WHEN i < ${NM} * 0.9 THEN repeat('a', 10 + (i % 41)) ELSE repeat('a', 900 + (i % 101)) END AS s7,
       CASE WHEN i < ${NM} * 0.9 THEN repeat('a', 10 + (i % 41)) ELSE repeat('a', 900 + (i % 101)) END AS s8
FROM range(${NM}) _(i);

-- persist everything: a fresh connection then sees exact, clean row group stats
CHECKPOINT;
"

echo "== verifying =="
for table in len_concentrated len_unicode len_unicode_all len_multi; do
    row_groups=$("$DUCKDB" "$DB_PATH" -csv -c "SELECT count(DISTINCT row_group_id) FROM pragma_storage_info('$table')" 2>/dev/null | tail -1)
    echo "   $table: $row_groups row groups"
done

echo "== done =="
