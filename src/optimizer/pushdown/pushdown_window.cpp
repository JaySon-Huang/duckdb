#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

using Filter = FilterPushdown::Filter;

bool CanPushdownFilter(vector<column_binding_set_t> window_exprs_partition_bindings,
                       const vector<ColumnBinding> &bindings) {
	auto filter_on_all_partitions = true;
	for (auto &partition_binding_set : window_exprs_partition_bindings) {
		auto filter_on_binding_set = true;
		for (auto &binding : bindings) {
			if (partition_binding_set.find(binding) == partition_binding_set.end()) {
				filter_on_binding_set = false;
				break;
			}
		}
		filter_on_all_partitions = filter_on_all_partitions && filter_on_binding_set;
		if (!filter_on_all_partitions) {
			break;
		}
	}
	return filter_on_all_partitions;
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownWindow(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_WINDOW);
	auto &window = op->Cast<LogicalWindow>();
	FilterPushdown pushdown(optimizer, convert_mark_joins, projection_mode);

	// EXPERIMENT (PRUN-0007): FlattenDependentJoins wraps the dependent-join LHS in a
	// row_number() OVER () window (alias "limit_rownum", see flatten_dependent_join.cpp)
	// whose numbering is consumed only as the join-back key. The numbering is
	// self-consistent within the materialized delim CTE - both consumers read the same
	// materialization - so pushing a single-side filter below it renumbers but never
	// changes results. Push every filter whose bindings live entirely in the child
	// subtree. User-written row_number() OVER () windows carry a different alias and are
	// unaffected; PushDownLimit windows carry non-empty partitions and are unaffected.
	bool is_internal_rownum_window = false;
	if (window.expressions.size() == 1) {
		auto &expr = window.expressions[0];
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_WINDOW && expr->HasAlias() &&
		    expr->GetAlias() == "limit_rownum") {
			auto &window_expr = expr->Cast<BoundWindowExpression>();
			is_internal_rownum_window = window_expr.Partitions().empty() && window_expr.OrderByMutable().empty();
		}
	}
	if (is_internal_rownum_window) {
		unordered_set<TableIndex> child_bindings;
		LogicalJoin::GetTableReferences(*op->children[0], child_bindings);
		vector<unique_ptr<Filter>> leftover_filters;
		for (idx_t i = 0; i < filters.size(); i++) {
			bool all_in_child = true;
			for (auto &binding : filters.at(i)->bindings) {
				if (child_bindings.find(binding) == child_bindings.end()) {
					all_in_child = false;
					break;
				}
			}
			if (all_in_child) {
				pushdown.filters.push_back(std::move(filters.at(i)));
			} else {
				leftover_filters.push_back(std::move(filters.at(i)));
			}
		}
		op->children[0] = pushdown.Rewrite(std::move(op->children[0]));
		filters = std::move(leftover_filters);
		return FinishPushdown(std::move(op));
	}

	// 1. Loop through the expressions, find the window expressions and investigate the partitions
	// if a filter applies to a partition in each window expression then you can push the filter
	// into the children.
	vector<column_binding_set_t> window_exprs_partition_bindings;
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
		column_binding_set_t partition_bindings;
		// 2. Get the binding information of the partitions of the window expression
		for (auto &partition_expr : partitions) {
			switch (partition_expr->GetExpressionType()) {
			// TODO: Add expressions for function expressions like FLOOR, CEIL etc.
			case ExpressionType::BOUND_COLUMN_REF: {
				auto &partition_col = partition_expr->Cast<BoundColumnRefExpression>();
				partition_bindings.insert(partition_col.Binding());
				break;
			}
			default:
				break;
			}
		}
		window_exprs_partition_bindings.push_back(partition_bindings);
	}

	if (window_exprs_partition_bindings.empty()) {
		return FinishPushdown(std::move(op));
	}

	vector<unique_ptr<Filter>> leftover_filters;
	// Loop through the filters. If a filter is on a partition in every window expression
	// it can be pushed down.
	for (idx_t i = 0; i < filters.size(); i++) {
		// the filter must be on all partition bindings
		vector<ColumnBinding> bindings;
		ExtractFilterBindings(*filters.at(i)->filter, bindings);
		if (CanPushdownFilter(window_exprs_partition_bindings, bindings)) {
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
