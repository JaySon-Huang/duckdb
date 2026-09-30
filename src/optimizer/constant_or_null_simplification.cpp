#include "duckdb/optimizer/constant_or_null_simplification.hpp"

#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression_nullability.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"

namespace duckdb {

ConstantOrNullSimplification::ConstantOrNullSimplification(ClientContext &context_p) : context(context_p) {
}

static optional<bool> GetBooleanConstant(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return optional<bool>();
	}

	auto &constant = expr.Cast<BoundConstantExpression>().GetValue();
	if (constant.IsNull() || constant.type().id() != LogicalTypeId::BOOLEAN) {
		return optional<bool>();
	}

	return BooleanValue::Get(constant);
}

static optional<bool> GetConstantOrNullBoolean(Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION ||
	    expr.GetReturnType().id() != LogicalTypeId::BOOLEAN) {
		return optional<bool>();
	}

	auto &func = expr.Cast<BoundFunctionExpression>();
	if (ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(true))) {
		return true;
	}

	if (ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(false))) {
		return false;
	}

	return optional<bool>();
}

//! Whether every input that can still turn the result into NULL is provably NOT NULL.
static bool ConstantOrNullInputsAreNotNull(LogicalOperator &input, BoundFunctionExpression &func,
                                           NotNullExpressionAnalyzer &analyzer) {
	auto &children = func.GetChildren();
	D_ASSERT(children.size() >= 2);

	// Folding is only valid when the NULL-preserving inputs cannot be NULL.
	for (idx_t child_idx = 1; child_idx < children.size(); ++child_idx) {
		if (children[child_idx]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			auto &constant = children[child_idx]->Cast<BoundConstantExpression>().GetValue();
			if (!constant.IsNull()) {
				continue;
			}
		}

		if (!analyzer.IsNotNull(input, *children[child_idx])) {
			return false;
		}
	}

	return true;
}

static bool ConstantOrNullInputsAreVolatile(BoundFunctionExpression &func) {
	auto &children = func.GetChildren();
	for (idx_t child_idx = 1; child_idx < children.size(); ++child_idx) {
		if (children[child_idx]->IsVolatile()) {
			return true;
		}
	}
	return false;
}

unique_ptr<Expression> ConstantOrNullSimplification::SimplifyExpression(LogicalOperator &input,
                                                                        unique_ptr<Expression> expr,
                                                                        NotNullExpressionAnalyzer &analyzer,
                                                                        bool allow_folding) {
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		child = SimplifyExpression(input, std::move(child), analyzer, allow_folding);
	});

	if (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		if (!allow_folding) {
			return expr;
		}
		auto value = GetConstantOrNullBoolean(*expr);
		if (!value.has_value()) {
			return expr;
		}

		auto &func = expr->Cast<BoundFunctionExpression>();
		if (!ConstantOrNullInputsAreVolatile(func) && ConstantOrNullInputsAreNotNull(input, func, analyzer)) {
			return make_uniq<BoundConstantExpression>(Value::BOOLEAN(value.value()));
		}

		return expr;
	}

	if (expr->GetExpressionType() != ExpressionType::OPERATOR_NOT) {
		return expr;
	}

	// Push NOT into constant_or_null without dropping per-row NULL checks.
	auto &not_expr = expr->Cast<BoundOperatorExpression>();
	D_ASSERT(not_expr.GetChildren().size() == 1);

	auto value = GetBooleanConstant(*not_expr.GetChildren()[0]);
	if (value.has_value()) {
		return make_uniq<BoundConstantExpression>(Value::BOOLEAN(!value.value()));
	}

	value = GetConstantOrNullBoolean(*not_expr.GetChildren()[0]);
	if (!value.has_value()) {
		return expr;
	}

	auto &func = not_expr.GetChildren()[0]->Cast<BoundFunctionExpression>();
	auto &func_children = func.GetChildrenMutable();
	D_ASSERT(func_children.size() >= 2);

	vector<unique_ptr<Expression>> children;
	children.reserve(func_children.size());
	children.push_back(make_uniq<BoundConstantExpression>(Value::BOOLEAN(!value.value())));
	for (idx_t child_idx = 1; child_idx < func_children.size(); ++child_idx) {
		children.push_back(std::move(func_children[child_idx]));
	}

	return ExpressionRewriter::ConstantOrNull(this->context, std::move(children), Value::BOOLEAN(!value.value()));
}

unique_ptr<LogicalOperator> ConstantOrNullSimplification::OptimizeFilter(unique_ptr<LogicalOperator> op,
                                                                         bool plan_has_side_effects) {
	auto &filter = op->Cast<LogicalFilter>();
	if (filter.children.size() != 1) {
		return op;
	}

	// Folding removes the NULL check, so disable it for plans with side effects.
	// Same-statement DML can add NULLs after statistics-based nullability analysis.
	const bool allow_folding = !plan_has_side_effects;

	NotNullExpressionAnalyzer analyzer(context);
	vector<unique_ptr<Expression>> remaining_expressions;
	remaining_expressions.reserve(filter.expressions.size());
	for (auto &expr : filter.expressions) {
		expr = SimplifyExpression(*filter.children[0], std::move(expr), analyzer, allow_folding);
		RewriteConnectives(expr);
		auto value = GetBooleanConstant(*expr);
		if (!value.has_value()) {
			remaining_expressions.push_back(std::move(expr));
		} else if (!value.value()) {
			return make_uniq<LogicalEmptyResult>(std::move(op));
		}
	}

	if (!remaining_expressions.empty()) {
		filter.expressions = std::move(remaining_expressions);
		return op;
	}

	if (filter.projection_map.empty()) {
		return std::move(filter.children[0]);
	}

	remaining_expressions.push_back(make_uniq<BoundConstantExpression>(Value::BOOLEAN(true)));
	filter.expressions = std::move(remaining_expressions);
	return op;
}

unique_ptr<LogicalOperator> ConstantOrNullSimplification::Optimize(unique_ptr<LogicalOperator> op) {
	const bool has_side_effects = op->HasSideEffects();
	return OptimizeInternal(std::move(op), has_side_effects);
}

unique_ptr<LogicalOperator> ConstantOrNullSimplification::OptimizeInternal(unique_ptr<LogicalOperator> op,
                                                                           bool plan_has_side_effects) {
	for (auto &child : op->children) {
		child = OptimizeInternal(std::move(child), plan_has_side_effects);
	}

	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		return OptimizeFilter(std::move(op), plan_has_side_effects);
	}

	return op;
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

bool ConstantOrNullSimplification::RewriteConnectives(unique_ptr<Expression> &expr) {
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return false;
	}
	auto &conjunction = expr->Cast<BoundConjunctionExpression>();
	const bool is_or = conjunction.GetExpressionType() == ExpressionType::CONJUNCTION_OR;
	auto &children = conjunction.GetChildrenMutable();
	bool changed = false;
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		auto &child = children[child_idx];
		changed |= RewriteConnectives(child);
		if (child->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			continue;
		}
		auto &func = child->Cast<BoundFunctionExpression>();
		if (!ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(true)) &&
		    !ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(false))) {
			continue;
		}
		if (func.IsVolatile()) {
			// dropping or rewriting the atom would change how often its inputs are evaluated
			continue;
		}
		if (IsNeverTrueConstantOrNull(func)) {
			if (is_or) {
				// a disjunct that can never be TRUE cannot make the OR pass a row: drop it
				children.erase_at(child_idx);
				child_idx--;
			} else if (std::any_of(children.begin(), children.end(), [](const unique_ptr<Expression> &sibling) {
					return sibling->IsVolatile();
				})) {
				// replacing the conjunction with FALSE would drop the evaluation of a volatile sibling
				continue;
			} else {
				// a conjunct that can never be TRUE makes the conjunction never pass a row
				child = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
			}
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
	if (children.empty()) {
		// every disjunct was dropped: the conjunction can never pass a row
		expr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(is_or ? false : true));
	} else if (children.size() == 1) {
		expr = std::move(children[0]);
	}
	return true;
}

} // namespace duckdb
