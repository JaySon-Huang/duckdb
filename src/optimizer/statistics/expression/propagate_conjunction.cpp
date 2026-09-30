
#include "duckdb/optimizer/statistics_propagator.hpp"

#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(BoundConjunctionExpression &expr,
                                                                     unique_ptr<Expression> &expr_ptr) {
	auto is_and = expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND;
	for (idx_t expr_idx = 0; expr_idx < expr.GetChildrenMutable().size(); expr_idx++) {
		auto &child = expr.GetChildrenMutable()[expr_idx];
		auto stats = PropagateExpression(child);
		if (!child->IsFoldable()) {
			continue;
		}
		// we have a constant in a conjunction
		// we (1) either prune the child
		// or (2) replace the entire conjunction with a constant
		auto constant = ExpressionExecutor::EvaluateScalar(context, *child);
		if (constant.IsNull()) {
			continue;
		}
		auto b = BooleanValue::Get(constant);
		bool prune_child = false;
		bool constant_value = true;
		if (b) {
			// true
			if (is_and) {
				// true in and: prune child
				prune_child = true;
			} else {
				// true in OR: replace with TRUE
				constant_value = true;
			}
		} else {
			// false
			if (is_and) {
				// false in AND: replace with FALSE
				constant_value = false;
			} else {
				// false in OR: prune child
				prune_child = true;
			}
		}
		if (prune_child) {
			expr.GetChildrenMutable().erase_at(expr_idx);
			expr_idx--;
			removed_expressions = true;
			continue;
		}
		expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(constant_value));
		return PropagateExpression(expr_ptr);
	}
	if (expr.GetChildrenMutable().empty()) {
		// if there are no children left, replace the conjunction with TRUE (for AND) or FALSE (for OR)
		expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(is_and));
		return PropagateExpression(expr_ptr);
	} else if (expr.GetChildrenMutable().size() == 1) {
		// if there is one child left, replace the conjunction with that one child
		expr_ptr = std::move(expr.GetChildrenMutable()[0]);
	}
	return nullptr;
}

//! Whether the constant_or_null evaluates to FALSE or NULL for every row, i.e. can never pass a filter
static bool IsNeverTrueConstantOrNull(BoundFunctionExpression &func) {
	if (ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(false))) {
		return true;
	}
	if (!ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(true))) {
		return false;
	}
	// constant TRUE nullified by the children: a constant NULL child nullifies every row
	auto &children = func.GetChildren();
	for (idx_t i = 1; i < children.size(); i++) {
		if (children[i]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			if (children[i]->Cast<BoundConstantExpression>().GetValue().IsNull()) {
				return true;
			}
		}
	}
	return false;
}

//! Move the nullifying children of a constant_or_null(TRUE, ...) into NULL checks - the expression passes a filter
//! if and only if all of them are non-NULL. Returns false if the expression must stay as-is
static bool TryGetNullChecks(BoundFunctionExpression &func, vector<unique_ptr<Expression>> &null_checks) {
	auto &children = func.GetChildrenMutable();
	for (idx_t i = 1; i < children.size(); i++) {
		if (children[i]->IsVolatile()) {
			return false;
		}
	}
	for (idx_t i = 1; i < children.size(); i++) {
		auto &child = children[i];
		if (child->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			// a non-NULL constant cannot nullify; constant NULLs were rejected by IsNeverTrueConstantOrNull
			continue;
		}
		auto null_check = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
		null_check->GetChildrenMutable().push_back(std::move(child));
		null_checks.push_back(std::move(null_check));
	}
	return true;
}

bool StatisticsPropagator::SimplifyConstantOrNullConnectives(unique_ptr<Expression> &expr) {
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return false;
	}
	auto &conjunction = expr->Cast<BoundConjunctionExpression>();
	const bool is_or = conjunction.GetExpressionType() == ExpressionType::CONJUNCTION_OR;
	auto &children = conjunction.GetChildrenMutable();
	bool changed = false;
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		auto &child = children[child_idx];
		changed |= SimplifyConstantOrNullConnectives(child);
		if (child->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			continue;
		}
		auto &func = child->Cast<BoundFunctionExpression>();
		if (!ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(true)) &&
		    !ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(false))) {
			continue;
		}
		if (IsNeverTrueConstantOrNull(func)) {
			if (is_or) {
				// a disjunct that can never be TRUE cannot make the OR pass a row: drop it
				children.erase_at(child_idx);
				child_idx--;
			} else {
				// a conjunct that can never be TRUE makes the conjunction never pass a row
				child = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
			}
			removed_expressions = true;
			changed = true;
			continue;
		}
		vector<unique_ptr<Expression>> null_checks;
		if (!TryGetNullChecks(func, null_checks)) {
			continue;
		}
		if (null_checks.empty()) {
			// only non-NULL constants nullify: the expression is always TRUE
			child = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
		} else if (null_checks.size() == 1) {
			child = std::move(null_checks[0]);
		} else {
			auto new_conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			for (auto &null_check : null_checks) {
				new_conjunction->GetChildrenMutable().push_back(std::move(null_check));
			}
			child = std::move(new_conjunction);
		}
		changed = true;
	}
	if (!changed) {
		return false;
	}
	PropagateExpression(expr);
	return true;
}

} // namespace duckdb
