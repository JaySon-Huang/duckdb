#include "duckdb/optimizer/count_distinct_rewriter.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

void CountDistinctRewriter::VisitOperator(unique_ptr<LogicalOperator> &op) {
	for (auto &child : op->children) {
		VisitOperator(child);
	}
	if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		TryRewrite(op);
	}
}

optional_ptr<LogicalGet> CountDistinctRewriter::FindSourceGet(reference<LogicalOperator> child_ref,
                                                              ColumnBinding &binding) const {
	while (true) {
		auto &child = child_ref.get();
		switch (child.type) {
		case LogicalOperatorType::LOGICAL_FILTER:
			// filters do not remap column bindings
			child_ref = *child.children[0];
			continue;
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			// the projection must feed our column with a plain column reference
			auto &projection = child.Cast<LogicalProjection>();
			auto &expr = projection.GetExpression(binding);
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				return nullptr;
			}
			binding = expr.Cast<BoundColumnRefExpression>().Binding();
			child_ref = *child.children[0];
			continue;
		}
		case LogicalOperatorType::LOGICAL_GET:
			return &child.Cast<LogicalGet>();
		default:
			// joins, set operations, etc: the uniqueness of a single table does not carry over
			return nullptr;
		}
	}
}

bool CountDistinctRewriter::ColumnIsUniqueNotNull(LogicalGet &get, const ColumnBinding &binding) const {
	auto table = get.GetTable();
	if (!table) {
		// not a base table (parquet, table function, CTE, etc): no constraints to derive from
		return false;
	}
	auto &column_index = get.GetColumnIndex(binding);
	if (column_index.IsRowIdColumn() || column_index.IsVirtualColumn()) {
		return false;
	}
	auto &column = table->GetColumns().GetColumn(LogicalIndex(column_index.GetPrimaryIndex()));
	const auto column_index_logical = column.Logical();

	bool has_unique = false;
	bool has_not_null = false;
	for (auto &constraint : table->GetConstraints()) {
		if (constraint->type == ConstraintType::NOT_NULL) {
			auto &not_null = constraint->Cast<NotNullConstraint>();
			has_not_null = has_not_null || not_null.index == column_index_logical;
			continue;
		}
		if (constraint->type != ConstraintType::UNIQUE) {
			continue;
		}
		auto &unique = constraint->Cast<UniqueConstraint>();
		if (unique.HasIndex()) {
			// single-column PRIMARY KEY or UNIQUE constraint
			has_unique = has_unique || unique.GetIndex() == column_index_logical;
			continue;
		}
		if (unique.GetColumnNames().size() != 1) {
			// multi-column UNIQUE constraints do not imply single-column uniqueness
			continue;
		}
		auto constrained_columns = unique.GetLogicalIndexes(table->GetColumns());
		has_unique =
		    has_unique || (constrained_columns.size() == 1 && constrained_columns[0] == column_index_logical);
	}
	if (!has_unique || !has_not_null) {
		return false;
	}
	return true;
}

void CountDistinctRewriter::TryRewrite(unique_ptr<LogicalOperator> &op) {
	auto &aggr = op->Cast<LogicalAggregate>();
	if (!aggr.groups.empty() || aggr.grouping_sets.size() > 1 || aggr.expressions.empty()) {
		// uniqueness holds per snapshot for the whole table, so grouped count(DISTINCT) is not covered
		return;
	}
	for (idx_t aggr_idx = 0; aggr_idx < aggr.expressions.size(); aggr_idx++) {
		auto &expr = aggr.expressions[aggr_idx];
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return;
		}
		auto &aggregate = expr->Cast<BoundAggregateExpression>();
		if (!aggregate.IsDistinct() || aggregate.Function().GetName() != "count" ||
		    aggregate.GetChildren().size() != 1 || aggregate.GetFilter() || aggregate.GetOrderBys() ||
		    aggregate.StateExportMode() == AggregateStateExportMode::STATE_EXPORT) {
			continue;
		}
		auto &argument = *aggregate.GetChildren()[0];
		if (argument.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			// only plain column references are covered
			continue;
		}
		auto binding = argument.Cast<BoundColumnRefExpression>().Binding();
		auto get = FindSourceGet(*aggr.children[0], binding);
		if (!get) {
			continue;
		}
		if (!ColumnIsUniqueNotNull(*get, binding)) {
			continue;
		}
		// the column is unique and NOT NULL in every snapshot: count(DISTINCT col) == count(*).
		// Uniqueness is a per-snapshot invariant enforced at every write, so the rewrite is always
		// equivalent. The constant direct-out of the resulting count_star stays guarded by the
		// statistics propagation (exact counts, pending writes, samples, table filters).
		aggregate.FunctionMutable().ReplaceImplementation(CountStarFun::GetFunction());
		aggregate.FunctionMutable().SetName("count_star");
		aggregate.GetChildrenMutable().clear();
		aggregate.GetAggregateTypeMutable() = AggregateType::NON_DISTINCT;
	}
}

} // namespace duckdb
