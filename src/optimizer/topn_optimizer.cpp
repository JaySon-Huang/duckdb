#include "duckdb/optimizer/topn_optimizer.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"
#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

namespace {

struct TopNSortKeyColumn {
	bool found = false;
	//! The single column binding the sort key depends on
	ColumnBinding binding;
	//! The type of the column reference inside the sort key expression
	LogicalType column_type;
};

//! Checks whether the sort key expression is a deterministic expression over exactly one column
//! binding (e.g. ORDER BY length(s)) and extracts that binding. Returns false for anything we
//! cannot safely rebuild over the raw scan value.
bool ExtractSortKeyColumn(const Expression &expr, TopNSortKeyColumn &result) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF: {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (result.found) {
			return colref.Binding() == result.binding;
		}
		result.found = true;
		result.binding = colref.Binding();
		result.column_type = colref.GetReturnType();
		return true;
	}
	case ExpressionClass::BOUND_CONSTANT:
		return true;
	case ExpressionClass::BOUND_FUNCTION: {
		auto &function = expr.Cast<BoundFunctionExpression>();
		if (function.Function().GetStability() != FunctionStability::CONSISTENT) {
			// the dynamic filter re-evaluates the expression in a different execution context, so
			// it must return identical values there - bail on volatile/query-stable functions
			return false;
		}
		for (auto &child : function.GetChildren()) {
			if (!ExtractSortKeyColumn(*child, result)) {
				return false;
			}
		}
		return true;
	}
	default:
		// cannot reason about other expression shapes (CASE, operators, parameters, ...)
		return false;
	}
}

} // namespace

TopN::TopN(ClientContext &context_p) : context(context_p) {
}

bool TopN::CanOptimize(LogicalOperator &op, optional_ptr<ClientContext> context) {
	if (op.type == LogicalOperatorType::LOGICAL_LIMIT) {
		auto &limit = op.Cast<LogicalLimit>();

		if (limit.limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
			// we need LIMIT to be present AND be a constant value for us to be able to use Top-N
			return false;
		}
		if (limit.offset_val.Type() == LimitNodeType::EXPRESSION_VALUE) {
			// we need offset to be either not set (i.e. limit without offset) OR have offset be
			return false;
		}

		auto child_op = op.children[0].get();
		if (context) {
			// estimate child cardinality if the context is available
			child_op->EstimateCardinality(*context);
		}

		if (child_op->has_estimated_cardinality) {
			// only check if we should switch to full sorting if we have estimated cardinality
			auto constant_limit = static_cast<double>(limit.limit_val.GetConstantValue());
			if (limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
				constant_limit += static_cast<double>(limit.offset_val.GetConstantValue());
			}
			auto child_card = static_cast<double>(child_op->estimated_cardinality);

			// if the limit is > 0.7% of the child cardinality, sorting the whole table is faster
			bool limit_is_large = constant_limit > 5000;
			if (constant_limit > child_card * 0.007 && limit_is_large) {
				return false;
			}
		}

		while (child_op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			D_ASSERT(!child_op->children.empty());
			child_op = child_op->children[0].get();
		}

		return child_op->type == LogicalOperatorType::LOGICAL_ORDER_BY;
	}
	return false;
}

void TopN::PushdownDynamicFilters(LogicalTopN &op) {
	// pushdown dynamic filters through the Top-N operator
	bool nulls_first = op.orders[0].null_order == OrderByNullType::NULLS_FIRST;
	auto &type = op.orders[0].expression->GetReturnType();
	if (!TypeIsNumeric(type.InternalType()) && type.id() != LogicalTypeId::VARCHAR) {
		// only supported for numeric and varchar types
		return;
	}
	if (op.dynamic_filter) {
		// dynamic filter is already set
		return;
	}
	auto &order_expression = op.orders[0].expression;
	// determine the column to push the filter onto: either the sort key is a bare column reference,
	// or a deterministic expression over exactly one column (e.g. ORDER BY length(s)). The dynamic
	// filter boundary lives in the sort key value domain either way - the heap boundary is the decoded
	// sort key value of orders[0] - so the pushed filter must evaluate the same expression over the
	// raw scan column and compare it against the boundary.
	bool has_filter_expression = false;
	unique_ptr<Expression> filter_expression;
	TopNSortKeyColumn sort_key_column;
	if (order_expression->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		sort_key_column.found = true;
		sort_key_column.binding = order_expression->Cast<BoundColumnRefExpression>().Binding();
		sort_key_column.column_type = type;
	} else if (!ExtractSortKeyColumn(*order_expression, sort_key_column)) {
		// not a deterministic single-column expression - bail out
		return;
	} else {
		filter_expression = order_expression->Copy();
		has_filter_expression = true;
	}
	if (!sort_key_column.found) {
		// pure constant sort key (e.g. a folded ORDER BY 1+1) - there is no column to push a
		// filter onto
		return;
	}
	// resolve through the projection nodes directly below the Top-N: the binder materializes
	// expression sort keys (e.g. ORDER BY year(d)) in a projection below the ORDER BY, and the sort
	// key references the materialized projection column rather than the base table column
	auto resolve_child = op.children[0].get();
	while (resolve_child->type == LogicalOperatorType::LOGICAL_PROJECTION &&
	       sort_key_column.binding.table_index == resolve_child->Cast<LogicalProjection>().table_index) {
		auto &proj = resolve_child->Cast<LogicalProjection>();
		auto &proj_expr = proj.GetExpression(sort_key_column.binding);
		if (!has_filter_expression && proj_expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			// bare reference - the filter stays on the plain column path, follow the reference
			auto &colref = proj_expr.Cast<BoundColumnRefExpression>();
			sort_key_column.binding = colref.Binding();
			sort_key_column.column_type = colref.GetReturnType();
		} else {
			TopNSortKeyColumn resolved;
			if (!ExtractSortKeyColumn(proj_expr, resolved)) {
				// the materialized sort key is not a deterministic single-column expression over one
				// column - bail out
				return;
			}
			if (!has_filter_expression) {
				filter_expression = proj_expr.Copy();
			} else {
				auto composed = filter_expression->Copy();
				ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
				    composed, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &node) {
					    if (colref.Binding() == sort_key_column.binding) {
						    node = proj_expr.Copy();
					    }
				    });
				filter_expression = std::move(composed);
			}
			has_filter_expression = true;
			sort_key_column.binding = resolved.binding;
			sort_key_column.column_type = resolved.column_type;
		}
		resolve_child = resolve_child->children[0].get();
	}
	vector<JoinFilterPushdownColumn> columns;
	JoinFilterPushdownColumn column;
	column.probe_column_index = sort_key_column.binding;
	columns.emplace_back(column);
	vector<PushdownFilterTarget> pushdown_targets;
	JoinFilterPushdownOptimizer::GetPushdownFilterTargets(*resolve_child, std::move(columns), pushdown_targets);
	if (pushdown_targets.empty()) {
		// no pushdown targets
		return;
	}
	for (auto &target : pushdown_targets) {
		auto &pushed_column = target.columns[0];
		if (pushed_column.mode != JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION ||
		    RuntimeFilterCastUtil::RuntimeFilterUsesTryCast(pushed_column)) {
			// the pushed expression cannot be reconstructed on top of the raw scan value (e.g. a
			// VARIANT in between), or the cast chain is not order-preserving (an explicit TRY_CAST,
			// or a cast that can throw) - bail out
			return;
		}
		if (has_filter_expression) {
			// expression sort key: the sort key is rebuilt over the raw scan column, so a cast chain
			// recorded between the scan and the sort key column (e.g. through a GROUP BY CAST(...) or
			// a set-op child projection) is out of scope - bail out
			if (!pushed_column.runtime_filter_casts.empty() ||
			    pushed_column.storage_type != sort_key_column.column_type) {
				return;
			}
		} else if (RuntimeFilterCastUtil::GetRuntimeFilterInputType(pushed_column, type) != type) {
			// the cast chain does not land in the sort key's type - bail out. The filter constants
			// are built in the sort key's type, so a type-changing cast recorded in between must be
			// replayed (in the loop below) or rejected (here), never ignored
			return;
		}
	}
	// found pushdown targets! generate dynamic filters
	ExpressionType comparison_type;
	if (op.orders[0].type == OrderType::ASCENDING) {
		// for ascending order, we want the lowest N elements, so we filter on C <= [boundary]
		// if we only have a single order clause, we can filter on C < boundary
		comparison_type =
		    op.orders.size() == 1 ? ExpressionType::COMPARE_LESSTHAN : ExpressionType::COMPARE_LESSTHANOREQUALTO;
	} else {
		// for descending order, we want the highest N elements, so we filter on C >= [boundary]
		// if we only have a single order clause, we can filter on C > boundary
		comparison_type =
		    op.orders.size() == 1 ? ExpressionType::COMPARE_GREATERTHAN : ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	}
	Value minimum_value = type.InternalType() == PhysicalType::VARCHAR ? Value("") : Value::MinimumValue(type);
	auto filter_data = make_shared_ptr<DynamicFilterData>(comparison_type, std::move(minimum_value));

	// put the filter into the Top-N clause
	op.dynamic_filter = filter_data;

	// rebuild the sort key expression over a reference to the raw scan column (filter-local index 0)
	auto build_filter_child = [&]() -> unique_ptr<Expression> {
		if (!has_filter_expression) {
			return make_uniq<BoundReferenceExpression>(type, storage_t(0));
		}
		auto child = filter_expression->Copy();
		ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
		    child, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &expr) {
			    if (colref.Binding() == sort_key_column.binding) {
				    expr = make_uniq<BoundReferenceExpression>(sort_key_column.column_type, storage_t(0));
			    }
		    });
		return child;
	};

	for (auto &target : pushdown_targets) {
		auto &get = target.get;
		D_ASSERT(target.columns.size() == 1);
		auto &pushed_column = target.columns[0];
		auto col_binding = pushed_column.probe_column_index;

		// build the filter input: either the bare sort key column replayed through its
		// order-preserving cast chain, or the sort key expression rebuilt over a reference to the raw
		// scan column (filter-local index 0) - both produce values in the sort key's type, so the
		// boundary constant - which is built in that same type - is compared against values in it
		unique_ptr<Expression> filter_input;
		if (has_filter_expression) {
			filter_input = build_filter_child();
		} else {
			bool preserves_cast_errors = false;
			filter_input =
			    RuntimeFilterCastUtil::CreateRuntimeFilterInputExpression(context, pushed_column, preserves_cast_errors);
			D_ASSERT(filter_input->GetReturnType() == type);
			D_ASSERT(!preserves_cast_errors);
		}

		// create the actual dynamic filter
		auto pushed_expr = CreateDynamicFilterExpression(filter_data, type, filter_input->Copy());
		if (nulls_first) {
			// rows whose sort key evaluates to NULL must not be dropped by the filter: with
			// NULLS FIRST they can be part of the top-N
			auto or_filter = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
			auto is_null =
			    ExpressionFilter::CreateNullCheckExpression(std::move(filter_input), ExpressionType::OPERATOR_IS_NULL);
			or_filter->GetChildrenMutable().push_back(std::move(is_null));
			or_filter->GetChildrenMutable().push_back(std::move(pushed_expr));
			pushed_expr = std::move(or_filter);
		}

		// push the filter into the table scan
		get.table_filters.PushFilter(col_binding.column_index,
		                             make_uniq<ExpressionFilter>(CreateOptionalFilterExpression(
		                                 std::move(pushed_expr), pushed_column.storage_type)));
	}
}

unique_ptr<LogicalOperator> TopN::Optimize(unique_ptr<LogicalOperator> op) {
	if (CanOptimize(*op, &context)) {
		vector<unique_ptr<LogicalOperator>> projections;

		// traverse operator tree and collect all projection nodes until we reach
		// the order by operator

		auto child = std::move(op->children[0]);
		// collect all projections until we get to the order by
		while (child->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			D_ASSERT(!child->children.empty());
			auto tmp = std::move(child->children[0]);
			projections.push_back(std::move(child));
			child = std::move(tmp);
		}
		D_ASSERT(child->type == LogicalOperatorType::LOGICAL_ORDER_BY);
		auto &order_by = child->Cast<LogicalOrder>();

		// Move order by operator into children of limit operator
		op->children[0] = std::move(child);

		auto &limit = op->Cast<LogicalLimit>();
		auto limit_val = limit.limit_val.GetConstantValue();
		idx_t offset_val = 0;
		if (limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
			offset_val = limit.offset_val.GetConstantValue();
		}
		auto topn = make_uniq<LogicalTopN>(std::move(order_by.orders), limit_val, offset_val);
		topn->AddChild(std::move(order_by.children[0]));
		auto cardinality = limit_val;
		if (topn->children[0]->has_estimated_cardinality && topn->children[0]->estimated_cardinality < limit_val) {
			cardinality = topn->children[0]->estimated_cardinality;
		}
		topn->SetEstimatedCardinality(cardinality);
		op = std::move(topn);

		// reconstruct all projection nodes above limit operator
		while (!projections.empty()) {
			auto node = std::move(projections.back());
			node->children[0] = std::move(op);
			op = std::move(node);
			projections.pop_back();
		}
	}
	if (op->type == LogicalOperatorType::LOGICAL_TOP_N) {
		PushdownDynamicFilters(op->Cast<LogicalTopN>());
	}

	for (auto &child : op->children) {
		child = Optimize(std::move(child));
	}
	return op;
}

} // namespace duckdb
