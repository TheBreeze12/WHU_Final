#pragma once

#include "toyc/ast.h"

namespace toyc {

class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    virtual void visit_comp_unit(const CompUnit& unit);
    virtual void visit_global_const(const GlobalConstDecl& decl);
    virtual void visit_global_var(const GlobalVarDecl& decl);
    virtual void visit_func_def(const FuncDef& func);

    virtual void visit_stmt(const Stmt& stmt);
    virtual void visit_expr(const Expr& expr);

    virtual void visit_int_literal(const IntLiteralExpr& node);
    virtual void visit_ident(const IdentExpr& node);
    virtual void visit_binary(const BinaryExpr& node);
    virtual void visit_unary(const UnaryExpr& node);
    virtual void visit_call(const CallExpr& node);

    virtual void visit_block_stmt(const BlockStmt& node);
    virtual void visit_empty_stmt(const EmptyStmt& node);
    virtual void visit_expr_stmt(const ExprStmt& node);
    virtual void visit_assign_stmt(const AssignStmt& node);
    virtual void visit_const_decl_stmt(const ConstDeclStmt& node);
    virtual void visit_var_decl_stmt(const VarDeclStmt& node);
    virtual void visit_if_stmt(const IfStmt& node);
    virtual void visit_while_stmt(const WhileStmt& node);
    virtual void visit_break_stmt(const BreakStmt& node);
    virtual void visit_continue_stmt(const ContinueStmt& node);
    virtual void visit_return_stmt(const ReturnStmt& node);
};

void walk_comp_unit(const CompUnit& unit, ASTVisitor& visitor);
void walk_stmt(const Stmt& stmt, ASTVisitor& visitor);
void walk_block(const BlockStmt& block, ASTVisitor& visitor);
void walk_expr(const Expr& expr, ASTVisitor& visitor);

}  // namespace toyc
