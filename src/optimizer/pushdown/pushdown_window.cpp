#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

using Filter = FilterPushdown::Filter;

namespace {

struct WindowPartitionInfo {
	// Column bindings of the partition keys that are plain column references.
	column_binding_set_t column_bindings;
	// All partition key expressions (any shape). Used for the expression-equivalence rule below.
	vector<reference<const Expression>> partition_exprs;
};

// A filter whose column bindings are all partition column bindings keeps or drops entire
// partitions: a partition column is constant within any partition.
bool FilterOnPartitionBindings(const vector<ColumnBinding> &bindings, const column_binding_set_t &partition_bindings) {
	for (auto &binding : bindings) {
		if (partition_bindings.find(binding) == partition_bindings.end()) {
			return false;
		}
	}
	return true;
}

// WND-0001 (MVP): a filter whose value is a function of a partition key expression E alone is also
// constant within every partition. Sufficient condition implemented here: the filter predicate is a
// comparison with one side structurally equal (post-binder normalization) to a partition expression
// E of this window, and the other side a constant. Anything that does not match this shape stays
// above the window: a filter on the underlying columns of E (e.g. "id >= 50" for PARTITION BY
// floor(id / 10)) can split a partition and change the row numbering, so it must not be pushed.
bool PartitionAnchoredComparison(const Expression &filter_expr,
                                 const vector<reference<const Expression>> &partition_exprs) {
	if (!BoundComparisonExpression::IsComparison(filter_expr)) {
		return false;
	}
	auto &comparison = filter_expr.Cast<BoundFunctionExpression>();
	const auto &left = BoundComparisonExpression::Left(comparison);
	const auto &right = BoundComparisonExpression::Right(comparison);
	for (auto &partition_ref : partition_exprs) {
		auto &partition_expr = partition_ref.get();
		if ((Expression::Equals(left, partition_expr) &&
		     right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) ||
		    (Expression::Equals(right, partition_expr) &&
		     left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT)) {
			return true;
		}
	}
	return false;
}

} // namespace

unique_ptr<LogicalOperator> FilterPushdown::PushdownWindow(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_WINDOW);
	auto &window = op->Cast<LogicalWindow>();
	FilterPushdown pushdown(optimizer, convert_mark_joins, projection_mode);

	// 1. Loop through the expressions, find the window expressions and investigate the partitions
	// if a filter applies to a partition in each window expression then you can push the filter
	// into the children.
	vector<WindowPartitionInfo> window_exprs_partition_info;
	for (auto &expr : window.expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_WINDOW) {
			continue;
		}
		auto &window_expr = expr->Cast<BoundWindowExpression>();
		auto &partitions = window_expr.Partitions();
		if (partitions.empty()) {
			// If any window expression does not have partitions, we cannot push any filters.
			// all window expressions need to be partitioned by the same column
			// in order to push down the window.
			return FinishPushdown(std::move(op));
		}
		WindowPartitionInfo partition_info;
		// 2. Get the binding information of the partitions of the window expression
		for (auto &partition_expr : partitions) {
			partition_info.partition_exprs.push_back(*partition_expr);
			switch (partition_expr->GetExpressionType()) {
			case ExpressionType::BOUND_COLUMN_REF: {
				auto &partition_col = partition_expr->Cast<BoundColumnRefExpression>();
				partition_info.column_bindings.insert(partition_col.Binding());
				break;
			}
			default:
				break;
			}
		}
		window_exprs_partition_info.push_back(std::move(partition_info));
	}

	if (window_exprs_partition_info.empty()) {
		return FinishPushdown(std::move(op));
	}

	vector<unique_ptr<Filter>> leftover_filters;
	// Loop through the filters. If a filter is on a partition in every window expression
	// it can be pushed down: either the filter only references partition column bindings, or it is
	// a comparison anchored on (i.e. expression-equivalent to) a partition key expression of that
	// window expression.
	for (idx_t i = 0; i < filters.size(); i++) {
		// the filter must be on all partition bindings
		vector<ColumnBinding> bindings;
		ExtractFilterBindings(*filters.at(i)->filter, bindings);
		auto filter_can_pushdown = true;
		for (auto &partition_info : window_exprs_partition_info) {
			if (FilterOnPartitionBindings(bindings, partition_info.column_bindings)) {
				continue;
			}
			if (PartitionAnchoredComparison(*filters.at(i)->filter, partition_info.partition_exprs)) {
				continue;
			}
			filter_can_pushdown = false;
			break;
		}
		if (filter_can_pushdown) {
			pushdown.filters.push_back(std::move(filters.at(i)));
		} else {
			leftover_filters.push_back(std::move(filters.at(i)));
		}
	}
	op->children[0] = pushdown.Rewrite(std::move(op->children[0]));
	filters = std::move(leftover_filters);
	return FinishPushdown(std::move(op));
}
} // namespace duckdb
