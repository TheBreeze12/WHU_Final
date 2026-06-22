#include "toyc/ast_visitor.h"

namespace toyc {

void ASTVisitor::visit_comp_unit(const CompUnit& unit) {
    for (const CompUnit::Item& item : unit.items) {
        switch (item.kind) {
            case CompUnit::ItemKind::GlobalConst:
                visit_global_const(item.global_const);
                break;
            case CompUnit::ItemKind::GlobalVar:
                visit_global_var(item.global_var);
                break;
            case CompUnit::ItemKind::FuncDef:
                visit_func_def(item.func_def);
                break;
        }
    }
}

void ASTVisitor::visit_global_const(const GlobalConstDecl& decl) {
    if (decl.init) {
        visit_expr(*decl.init);
    }
}

void ASTVisitor::visit_global_var(const GlobalVarDecl& decl) {
    if (decl.init) {
        visit_expr(*decl.init);
    }
}

void ASTVisitor::visit_func_def(const FuncDef& func) {
    if (func.body) {
        visit_block_stmt(*func.body);
    }
}

void ASTVisitor::visit_stmt(const Stmt& stmt) {
    switch (stmt.kind) {
        case Stmt::Kind::Block:
            visit_block_stmt(stmt.block);
            break;
        case Stmt::Kind::Empty:
            visit_empty_stmt(stmt.empty);
            break;
        case Stmt::Kind::Expr:
            visit_expr_stmt(stmt.expr);
            break;
        case Stmt::Kind::Assign:
            visit_assign_stmt(stmt.assign);
            break;
        case Stmt::Kind::ConstDecl:
            visit_const_decl_stmt(stmt.const_decl);
            break;
        case Stmt::Kind::VarDecl:
            visit_var_decl_stmt(stmt.var_decl);
            break;
        case Stmt::Kind::If:
            visit_if_stmt(stmt.if_stmt);
            break;
        case Stmt::Kind::While:
            visit_while_stmt(stmt.while_stmt);
            break;
        case Stmt::Kind::Break:
            visit_break_stmt(stmt.break_stmt);
            break;
        case Stmt::Kind::Continue:
            visit_continue_stmt(stmt.continue_stmt);
            break;
        case Stmt::Kind::Return:
            visit_return_stmt(stmt.return_stmt);
            break;
    }
}

void ASTVisitor::visit_expr(const Expr& expr) {
    switch (expr.kind) {
        case Expr::Kind::IntLiteral:
            visit_int_literal(expr.int_literal);
            break;
        case Expr::Kind::Ident:
            visit_ident(expr.ident);
            break;
        case Expr::Kind::Binary:
            visit_binary(expr.binary);
            break;
        case Expr::Kind::Unary:
            visit_unary(expr.unary);
            break;
        case Expr::Kind::Call:
            visit_call(expr.call);
            break;
    }
}

void ASTVisitor::visit_int_literal(const IntLiteralExpr& /*node*/) {}

void ASTVisitor::visit_ident(const IdentExpr& /*node*/) {}

void ASTVisitor::visit_binary(const BinaryExpr& node) {
    if (node.lhs) {
        visit_expr(*node.lhs);
    }
    if (node.rhs) {
        visit_expr(*node.rhs);
    }
}

void ASTVisitor::visit_unary(const UnaryExpr& node) {
    if (node.operand) {
        visit_expr(*node.operand);
    }
}

void ASTVisitor::visit_call(const CallExpr& node) {
    for (const std::unique_ptr<Expr>& arg : node.args) {
        if (arg) {
            visit_expr(*arg);
        }
    }
}

void ASTVisitor::visit_block_stmt(const BlockStmt& node) {
    walk_block(node, *this);
}

void ASTVisitor::visit_empty_stmt(const EmptyStmt& /*node*/) {}

void ASTVisitor::visit_expr_stmt(const ExprStmt& node) {
    if (node.expr) {
        visit_expr(*node.expr);
    }
}

void ASTVisitor::visit_assign_stmt(const AssignStmt& node) {
    if (node.value) {
        visit_expr(*node.value);
    }
}

void ASTVisitor::visit_const_decl_stmt(const ConstDeclStmt& node) {
    if (node.init) {
        visit_expr(*node.init);
    }
}

void ASTVisitor::visit_var_decl_stmt(const VarDeclStmt& node) {
    if (node.init) {
        visit_expr(*node.init);
    }
}

void ASTVisitor::visit_if_stmt(const IfStmt& node) {
    if (node.condition) {
        visit_expr(*node.condition);
    }
    if (node.then_branch) {
        visit_stmt(*node.then_branch);
    }
    if (node.else_branch) {
        visit_stmt(*node.else_branch);
    }
}

void ASTVisitor::visit_while_stmt(const WhileStmt& node) {
    if (node.condition) {
        visit_expr(*node.condition);
    }
    if (node.body) {
        visit_stmt(*node.body);
    }
}

void ASTVisitor::visit_break_stmt(const BreakStmt& /*node*/) {}

void ASTVisitor::visit_continue_stmt(const ContinueStmt& /*node*/) {}

void ASTVisitor::visit_return_stmt(const ReturnStmt& node) {
    if (node.value && *node.value) {
        visit_expr(**node.value);
    }
}

void walk_comp_unit(const CompUnit& unit, ASTVisitor& visitor) {
    visitor.visit_comp_unit(unit);
}

void walk_block(const BlockStmt& block, ASTVisitor& visitor) {
    for (const std::unique_ptr<Stmt>& stmt : block.body) {
        if (stmt) {
            visitor.visit_stmt(*stmt);
        }
    }
}

void walk_stmt(const Stmt& stmt, ASTVisitor& visitor) {
    visitor.visit_stmt(stmt);
}

void walk_expr(const Expr& expr, ASTVisitor& visitor) {
    visitor.visit_expr(expr);
}

}  // namespace toyc
