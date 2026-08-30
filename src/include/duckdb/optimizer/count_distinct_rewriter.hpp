//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/count_distinct_rewriter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class LogicalGet;

//! Rewrites `count(DISTINCT col)` into `count_star()` when `col` is provably unique and NOT NULL
//! in every snapshot: `col` carries a single-column PRIMARY KEY or UNIQUE constraint on a base
//! table and the table has a NOT NULL constraint for that column (PRIMARY KEY implies one).
//! Uniqueness is a per-snapshot invariant enforced at every write, so the rewrite is always
//! equivalent. The constant direct-out of the resulting count_star remains guarded by the
//! existing statistics machinery (exact row counts, pending writes, etc).
class CountDistinctRewriter {
public:
	void VisitOperator(unique_ptr<LogicalOperator> &op);

private:
	//! Attempts to rewrite the distinct count aggregates of an ungrouped aggregate in-place
	void TryRewrite(unique_ptr<LogicalOperator> &op);
	//! Resolves a column binding down to a base table scan, passing through projections and filters
	optional_ptr<LogicalGet> FindSourceGet(reference<LogicalOperator> child_ref, ColumnBinding &binding) const;
	//! True if the table has a single-column UNIQUE (or PRIMARY KEY) constraint on the given
	//! column, and a NOT NULL constraint for it
	bool ColumnIsUniqueNotNull(LogicalGet &get, const ColumnBinding &binding) const;
};

} // namespace duckdb
