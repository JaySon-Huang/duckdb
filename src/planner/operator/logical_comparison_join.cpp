#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

LogicalComparisonJoin::LogicalComparisonJoin(JoinType join_type, LogicalOperatorType logical_type)
    : LogicalJoin(join_type, logical_type) {
}

//! Maximum depth when resolving the source column name of a join condition
static constexpr idx_t CONDITION_NAME_RESOLVE_DEPTH = 8;

static bool TryResolveColumnNameFromChild(const LogicalOperator &op, const ColumnBinding &binding, string &result,
                                          idx_t depth) {
	if (depth > CONDITION_NAME_RESOLVE_DEPTH) {
		return false;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		if (get.table_index != binding.table_index) {
			return false;
		}
		auto column_index = binding.column_index.GetIndex();
		const auto &column_ids = get.GetColumnIds();
		if (column_ids.empty() || column_index >= column_ids.size()) {
			return false;
		}
		const auto &column_id = column_ids[column_index];
		if (column_id.IsVirtualColumn()) {
			auto entry = get.virtual_columns.find(column_id.GetPrimaryIndex());
			if (entry == get.virtual_columns.end()) {
				return false;
			}
			result = entry->second.name.GetIdentifierName();
			return true;
		}
		if (column_id.GetPrimaryIndex() >= get.names.size()) {
			return false;
		}
		result = get.names[column_id.GetPrimaryIndex()].GetIdentifierName();
		return true;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &proj = op.Cast<LogicalProjection>();
		if (proj.table_index != binding.table_index) {
			return false;
		}
		auto column_index = binding.column_index.GetIndex();
		if (column_index >= proj.expressions.size()) {
			return false;
		}
		auto &expr = proj.expressions[column_index];
		if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto &colref = expr->Cast<BoundColumnRefExpression>();
			if (colref.Depth() == 0) {
				// follow the reference to the column it projects
				return TryResolveColumnNameFromChild(*op.children[0], colref.Binding(), result, depth + 1);
			}
		}
		result = expr->GetName().GetIdentifierName();
		return true;
	}
	default:
		break;
	}
	// filters, cross products and joins pass child columns through unchanged
	for (auto &child : op.children) {
		if (TryResolveColumnNameFromChild(*child, binding, result, depth + 1)) {
			return true;
		}
	}
	return false;
}

bool LogicalComparisonJoin::TryResolveConditionColumnName(const LogicalOperator &child, const Expression &side,
                                                          string &result) {
	if (side.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	auto &colref = side.Cast<BoundColumnRefExpression>();
	if (colref.Depth() != 0) {
		return false;
	}
	return TryResolveColumnNameFromChild(child, colref.Binding(), result, 0);
}

static void NormalizeConditionSide(Expression &side, LogicalOperator &child) {
	string resolved;
	if (LogicalComparisonJoin::TryResolveConditionColumnName(child, side, resolved)) {
		side.SetAlias(Identifier(resolved));
	}
}

void LogicalComparisonJoin::NormalizeConditionColumnNames(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN: {
		auto &join = op.Cast<LogicalComparisonJoin>();
		for (auto &cond : join.conditions) {
			if (!cond.IsComparison()) {
				continue;
			}
			// for a flipped delim join the condition sides resolve against the opposite children
			auto lhs_child = op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN && join.delim_flipped ? 1 : 0;
			auto rhs_child = 1 - lhs_child;
			NormalizeConditionSide(cond.GetLHS(), *op.children[lhs_child]);
			NormalizeConditionSide(cond.GetRHS(), *op.children[rhs_child]);
		}
		break;
	}
	default:
		break;
	}
	for (auto &child : op.children) {
		NormalizeConditionColumnNames(*child);
	}
}

InsertionOrderPreservingMap<string> LogicalComparisonJoin::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Join Type"] = EnumUtil::ToChars(join_type);

	string conditions_info;
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			conditions_info += "\n";
		}
		auto &condition = conditions[i];
		if (condition.IsComparison()) {
			// render column references with the names of the columns they actually reference: the
			// alias of a condition side can be stale after decorrelation rewrites
			auto lhs_child = type == LogicalOperatorType::LOGICAL_DELIM_JOIN && delim_flipped ? 1 : 0;
			auto render_side = [&](const Expression &side, const LogicalOperator &child) {
				string resolved;
				if (TryResolveConditionColumnName(child, side, resolved)) {
					return resolved;
				}
				return side.GetName().GetIdentifierName();
			};
			conditions_info += StringUtil::Format("%s %s %s", render_side(condition.GetLHS(), *children[lhs_child]),
			                                      ExpressionTypeToOperator(condition.GetComparisonType()),
			                                      render_side(condition.GetRHS(), *children[1 - lhs_child]));
		} else {
			conditions_info += condition.GetJoinExpression().ToString();
		}
	}
	result["Conditions"] = conditions_info;
	SetParamsEstimatedCardinality(result);

	return result;
}

bool LogicalComparisonJoin::HasEquality(idx_t &range_count) const {
	bool result = false;
	for (size_t c = 0; c < conditions.size(); ++c) {
		auto &cond = conditions[c];
		if (cond.IsComparison()) {
			switch (cond.GetComparisonType()) {
			case ExpressionType::COMPARE_EQUAL:
			case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
				result = true;
				break;
			case ExpressionType::COMPARE_LESSTHAN:
			case ExpressionType::COMPARE_GREATERTHAN:
			case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
				++range_count;
				break;
			case ExpressionType::COMPARE_NOTEQUAL:
			case ExpressionType::COMPARE_DISTINCT_FROM:
				break;
			default:
				throw NotImplementedException("Unimplemented comparison join");
			}
		}
	}
	return result;
}

bool LogicalComparisonJoin::HasArbitraryConditions() const {
	for (size_t c = 0; c < conditions.size(); ++c) {
		auto &cond = conditions[c];
		if (!cond.IsComparison()) {
			return true;
		}
	}
	return false;
}

} // namespace duckdb
