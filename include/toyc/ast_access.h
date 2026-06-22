#pragma once

#include "toyc/ast.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace toyc {

const IntLiteralExpr* as_int_literal(const Expr& expr);
const IdentExpr* as_ident(const Expr& expr);
const BinaryExpr* as_binary(const Expr& expr);
const UnaryExpr* as_unary(const Expr& expr);
const CallExpr* as_call(const Expr& expr);

const BlockStmt* as_block(const Stmt& stmt);
const AssignStmt* as_assign(const Stmt& stmt);
const ConstDeclStmt* as_const_decl(const Stmt& stmt);
const VarDeclStmt* as_var_decl(const Stmt& stmt);

using FuncSignatureMap = std::unordered_map<std::string, FuncReturnType>;

FuncSignatureMap build_func_signature_map(const CompUnit& unit);
std::vector<const FuncDef*> collect_func_defs(const CompUnit& unit);

}  // namespace toyc
