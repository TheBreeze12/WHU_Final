#include "toyc/eval.h"

#include "toyc/ast.h"
#include "toyc/sema.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toyc {

namespace {

using i32 = std::int32_t;
using i64 = std::int64_t;

// Deterministic two's-complement wrap to 32 bits, avoiding C++ signed overflow UB.
i32 wrap32(i64 value) {
    return static_cast<i32>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(value)));
}

// Thrown to unwind the C++ call stack when the evaluator must give up (budget
// exceeded, division by zero, missing side-table entry, ...). The caller turns
// this into std::nullopt and falls back to real codegen.
struct EvalAbort {};

enum class Flow { Normal, Break, Continue, Return, TailCall };

// Storage location of a declaration, resolved once at compile time. Locals and
// params index into the current call frame's flat vector; globals index into the
// shared global vector. Flat indexing avoids per-access hashing in hot loops.
struct VarLoc {
    bool global = false;
    std::uint32_t index = 0;
};

using Frame = std::vector<i32>;

struct CFunc;

// ---- compiled (resolved) expression / statement trees --------------------
//
// The AST is walked once and lowered into these nodes with every identifier,
// assignment target and call site pre-resolved. Execution then touches no hash
// maps, which is what makes tight loops fast enough to fold within budget.

struct CExpr {
    enum class Kind { Const, Load, Binary, Unary, And, Or, Call };
    Kind kind = Kind::Const;
    i32 const_value = 0;       // Const
    VarLoc loc;                // Load
    BinaryOp bop = BinaryOp::Add;   // Binary
    UnaryOp uop = UnaryOp::Plus;    // Unary
    const CExpr* a = nullptr;  // Binary/And/Or lhs, Unary operand
    const CExpr* b = nullptr;  // Binary/And/Or rhs
    const CFunc* callee = nullptr;   // Call
    std::vector<const CExpr*> args;  // Call
};

struct CStmt {
    enum class Kind {
        Block, ExprStmt, Assign, If, While, Break, Continue, Return, TailCall
    };
    Kind kind = Kind::Block;
    std::vector<const CStmt*> body;   // Block
    const CExpr* expr = nullptr;      // ExprStmt/Assign value/If cond/While cond/Return value
    VarLoc loc;                       // Assign target
    const CStmt* then_stmt = nullptr; // If then
    const CStmt* else_stmt = nullptr; // If else
    const CStmt* while_body = nullptr;// While body
    std::vector<const CExpr*> tail_args;  // TailCall
};

struct CFunc {
    const FuncDef* fn = nullptr;
    std::uint32_t frame_size = 0;
    const CStmt* body = nullptr;
    bool pure = false;
};

struct MemoKey {
    const FuncDef* fn;
    std::vector<i32> args;
    bool operator==(const MemoKey& other) const {
        return fn == other.fn && args == other.args;
    }
};

struct MemoKeyHash {
    std::size_t operator()(const MemoKey& key) const {
        std::size_t hash = std::hash<const void*>{}(key.fn);
        for (i32 arg : key.args) {
            hash ^= std::hash<i32>{}(arg) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

class Evaluator {
public:
    Evaluator(const CompUnit& unit, const SemaResult& sema, const EvalBudget& budget)
        : unit_(unit), sema_(sema), budget_(budget) {}

    std::optional<i32> run() {
        deadline_ = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(budget_.max_millis);
        try {
            if (!prepass()) {
                return std::nullopt;
            }
            classify_purity();
            if (!compile_all()) {
                return std::nullopt;
            }
            // Evaluate global initializers in declaration order.
            Frame empty;
            for (const auto& [index, init] : global_inits_) {
                globals_[index] = eval_expr(*init, empty);
            }
            const CFunc* main = find_main();
            if (!main || !main->body) {
                return std::nullopt;
            }
            std::vector<i32> no_args;
            return call_function(*main, std::move(no_args));
        } catch (const EvalAbort&) {
            return std::nullopt;
        }
    }

private:
    inline void tick() {
        if ((++steps_ & 0xFFFFu) == 0) {
            if (steps_ > budget_.max_steps ||
                std::chrono::steady_clock::now() > deadline_) {
                throw EvalAbort{};
            }
        }
    }

    // ---- pre-pass: assign flat storage slots ------------------------------

    bool prepass() {
        std::uint32_t gidx = 0;
        for (const CompUnit::Item& item : unit_.items) {
            switch (item.kind) {
                case CompUnit::ItemKind::GlobalConst:
                    loc_[&item.global_const] = VarLoc{true, gidx};
                    globals_.push_back(0);
                    global_init_ast_.push_back({gidx, item.global_const.init.get()});
                    ++gidx;
                    break;
                case CompUnit::ItemKind::GlobalVar:
                    loc_[&item.global_var] = VarLoc{true, gidx};
                    globals_.push_back(0);
                    global_init_ast_.push_back({gidx, item.global_var.init.get()});
                    ++gidx;
                    break;
                case CompUnit::ItemKind::FuncDef: {
                    const FuncDef& fn = item.func_def;
                    functions_[fn.name] = &fn;
                    std::uint32_t idx = 0;
                    for (const Param& param : fn.params) {
                        loc_[&param] = VarLoc{false, idx++};
                    }
                    if (fn.body) {
                        collect_locals_block(*fn.body, idx);
                    }
                    frame_size_[&fn] = idx;
                    break;
                }
            }
        }
        return true;
    }

    void collect_locals_block(const BlockStmt& block, std::uint32_t& idx) {
        for (const std::unique_ptr<Stmt>& stmt : block.body) {
            if (stmt) {
                collect_locals_stmt(*stmt, idx);
            }
        }
    }

    void collect_locals_stmt(const Stmt& stmt, std::uint32_t& idx) {
        switch (stmt.kind) {
            case Stmt::Kind::Block:
                collect_locals_block(stmt.block, idx);
                break;
            case Stmt::Kind::ConstDecl:
                loc_[&stmt.const_decl] = VarLoc{false, idx++};
                break;
            case Stmt::Kind::VarDecl:
                loc_[&stmt.var_decl] = VarLoc{false, idx++};
                break;
            case Stmt::Kind::If:
                collect_locals_stmt(*stmt.if_stmt.then_branch, idx);
                if (stmt.if_stmt.else_branch) {
                    collect_locals_stmt(*stmt.if_stmt.else_branch, idx);
                }
                break;
            case Stmt::Kind::While:
                collect_locals_stmt(*stmt.while_stmt.body, idx);
                break;
            default:
                break;
        }
    }

    // ---- purity analysis (for memoization) --------------------------------

    struct FuncInfo {
        bool touches_global_var = false;
        std::unordered_set<const FuncDef*> callees;
    };

    void scan_expr_purity(const Expr& expr, FuncInfo& info) {
        switch (expr.kind) {
            case Expr::Kind::IntLiteral:
                break;
            case Expr::Kind::Ident: {
                auto it = sema_.idents.find(&expr.ident);
                if (it != sema_.idents.end() &&
                    it->second.storage == SymbolStorageKind::GlobalVar) {
                    info.touches_global_var = true;
                }
                break;
            }
            case Expr::Kind::Binary:
                scan_expr_purity(*expr.binary.lhs, info);
                scan_expr_purity(*expr.binary.rhs, info);
                break;
            case Expr::Kind::Unary:
                scan_expr_purity(*expr.unary.operand, info);
                break;
            case Expr::Kind::Call: {
                auto it = sema_.calls.find(&expr.call);
                if (it != sema_.calls.end()) {
                    info.callees.insert(it->second);
                }
                for (const std::unique_ptr<Expr>& arg : expr.call.args) {
                    scan_expr_purity(*arg, info);
                }
                break;
            }
        }
    }

    void scan_stmt_purity(const Stmt& stmt, FuncInfo& info) {
        switch (stmt.kind) {
            case Stmt::Kind::Block:
                for (const std::unique_ptr<Stmt>& child : stmt.block.body) {
                    if (child) {
                        scan_stmt_purity(*child, info);
                    }
                }
                break;
            case Stmt::Kind::Empty:
            case Stmt::Kind::Break:
            case Stmt::Kind::Continue:
                break;
            case Stmt::Kind::Expr:
                scan_expr_purity(*stmt.expr.expr, info);
                break;
            case Stmt::Kind::Assign: {
                auto it = sema_.assigns.find(&stmt.assign);
                if (it != sema_.assigns.end() &&
                    it->second.storage == SymbolStorageKind::GlobalVar) {
                    info.touches_global_var = true;
                }
                scan_expr_purity(*stmt.assign.value, info);
                break;
            }
            case Stmt::Kind::ConstDecl:
                scan_expr_purity(*stmt.const_decl.init, info);
                break;
            case Stmt::Kind::VarDecl:
                scan_expr_purity(*stmt.var_decl.init, info);
                break;
            case Stmt::Kind::If:
                scan_expr_purity(*stmt.if_stmt.condition, info);
                scan_stmt_purity(*stmt.if_stmt.then_branch, info);
                if (stmt.if_stmt.else_branch) {
                    scan_stmt_purity(*stmt.if_stmt.else_branch, info);
                }
                break;
            case Stmt::Kind::While:
                scan_expr_purity(*stmt.while_stmt.condition, info);
                scan_stmt_purity(*stmt.while_stmt.body, info);
                break;
            case Stmt::Kind::Return:
                if (stmt.return_stmt.value) {
                    scan_expr_purity(**stmt.return_stmt.value, info);
                }
                break;
        }
    }

    void classify_purity() {
        std::unordered_map<const FuncDef*, FuncInfo> infos;
        for (const auto& [name, fn] : functions_) {
            FuncInfo info;
            if (fn->body) {
                for (const std::unique_ptr<Stmt>& child : fn->body->body) {
                    if (child) {
                        scan_stmt_purity(*child, info);
                    }
                }
            }
            infos.emplace(fn, std::move(info));
        }

        for (const auto& [fn, info] : infos) {
            pure_[fn] = !info.touches_global_var;
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [fn, info] : infos) {
                if (!pure_[fn]) {
                    continue;
                }
                for (const FuncDef* callee : info.callees) {
                    auto it = pure_.find(callee);
                    if (it == pure_.end() || !it->second) {
                        pure_[fn] = false;
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    // ---- compilation: AST -> resolved CExpr/CStmt -------------------------

    bool compile_all() {
        // Create a CFunc for every function first so calls can resolve targets.
        for (const CompUnit::Item& item : unit_.items) {
            if (item.kind != CompUnit::ItemKind::FuncDef) {
                continue;
            }
            const FuncDef& fn = item.func_def;
            CFunc& cfunc = cfuncs_.emplace_back();
            cfunc.fn = &fn;
            cfunc.frame_size = frame_size_[&fn];
            cfunc.pure = is_pure(&fn);
            cfunc_by_def_[&fn] = &cfunc;
        }
        // Now compile bodies and global initializers.
        for (CFunc& cfunc : cfuncs_) {
            current_compile_fn_ = cfunc.fn;
            if (cfunc.fn->body) {
                cfunc.body = compile_block(*cfunc.fn->body);
            }
        }
        current_compile_fn_ = nullptr;
        for (const auto& [index, ast] : global_init_ast_) {
            const CExpr* init = compile_expr(*ast);
            global_inits_.push_back({index, init});
        }
        return true;
    }

    VarLoc lookup_loc(const void* decl) {
        auto it = loc_.find(decl);
        if (it == loc_.end()) {
            throw EvalAbort{};
        }
        return it->second;
    }

    CExpr* new_expr_mut() {
        cexprs_.emplace_back();
        return &cexprs_.back();
    }
    CStmt* new_stmt() {
        cstmts_.emplace_back();
        return &cstmts_.back();
    }

    const CExpr* compile_expr(const Expr& expr) {
        switch (expr.kind) {
            case Expr::Kind::IntLiteral: {
                CExpr* c = new_expr_mut();
                c->kind = CExpr::Kind::Const;
                c->const_value = static_cast<i32>(expr.int_literal.value);
                return c;
            }
            case Expr::Kind::Ident: {
                auto it = sema_.idents.find(&expr.ident);
                if (it == sema_.idents.end()) {
                    throw EvalAbort{};
                }
                CExpr* c = new_expr_mut();
                c->kind = CExpr::Kind::Load;
                c->loc = lookup_loc(it->second.decl);
                return c;
            }
            case Expr::Kind::Binary: {
                CExpr* c = new_expr_mut();
                if (expr.binary.op == BinaryOp::And) {
                    c->kind = CExpr::Kind::And;
                } else if (expr.binary.op == BinaryOp::Or) {
                    c->kind = CExpr::Kind::Or;
                } else {
                    c->kind = CExpr::Kind::Binary;
                    c->bop = expr.binary.op;
                }
                c->a = compile_expr(*expr.binary.lhs);
                c->b = compile_expr(*expr.binary.rhs);
                return c;
            }
            case Expr::Kind::Unary: {
                CExpr* c = new_expr_mut();
                c->kind = CExpr::Kind::Unary;
                c->uop = expr.unary.op;
                c->a = compile_expr(*expr.unary.operand);
                return c;
            }
            case Expr::Kind::Call: {
                auto it = sema_.calls.find(&expr.call);
                if (it == sema_.calls.end() || !it->second || !it->second->body) {
                    throw EvalAbort{};
                }
                auto cf = cfunc_by_def_.find(it->second);
                if (cf == cfunc_by_def_.end()) {
                    throw EvalAbort{};
                }
                CExpr* c = new_expr_mut();
                c->kind = CExpr::Kind::Call;
                c->callee = cf->second;
                c->args.reserve(expr.call.args.size());
                for (const std::unique_ptr<Expr>& arg : expr.call.args) {
                    c->args.push_back(compile_expr(*arg));
                }
                return c;
            }
        }
        throw EvalAbort{};
    }

    const CStmt* compile_block(const BlockStmt& block) {
        CStmt* s = new_stmt();
        s->kind = CStmt::Kind::Block;
        for (const std::unique_ptr<Stmt>& stmt : block.body) {
            if (stmt) {
                s->body.push_back(compile_stmt(*stmt));
            }
        }
        return s;
    }

    const CStmt* compile_stmt(const Stmt& stmt) {
        switch (stmt.kind) {
            case Stmt::Kind::Block:
                return compile_block(stmt.block);
            case Stmt::Kind::Empty: {
                CStmt* s = new_stmt();
                s->kind = CStmt::Kind::Block;  // empty block == no-op
                return s;
            }
            case Stmt::Kind::Expr: {
                CStmt* s = new_stmt();
                s->kind = CStmt::Kind::ExprStmt;
                s->expr = compile_expr(*stmt.expr.expr);
                return s;
            }
            case Stmt::Kind::Assign: {
                auto it = sema_.assigns.find(&stmt.assign);
                if (it == sema_.assigns.end()) {
                    throw EvalAbort{};
                }
                CStmt* s = new_stmt();
                s->kind = CStmt::Kind::Assign;
                s->loc = lookup_loc(it->second.decl);
                s->expr = compile_expr(*stmt.assign.value);
                return s;
            }
            case Stmt::Kind::ConstDecl: {
                CStmt* s = new_stmt();
                s->kind = CStmt::Kind::Assign;
                s->loc = lookup_loc(&stmt.const_decl);
                s->expr = compile_expr(*stmt.const_decl.init);
                return s;
            }
            case Stmt::Kind::VarDecl: {
                CStmt* s = new_stmt();
                s->kind = CStmt::Kind::Assign;
                s->loc = lookup_loc(&stmt.var_decl);
                s->expr = compile_expr(*stmt.var_decl.init);
                return s;
            }
            case Stmt::Kind::If: {
                CStmt* s = new_stmt();
                s->kind = CStmt::Kind::If;
                s->expr = compile_expr(*stmt.if_stmt.condition);
                s->then_stmt = compile_stmt(*stmt.if_stmt.then_branch);
                if (stmt.if_stmt.else_branch) {
                    s->else_stmt = compile_stmt(*stmt.if_stmt.else_branch);
                }
                return s;
            }
            case Stmt::Kind::While: {
                CStmt* s = new_stmt();
                s->kind = CStmt::Kind::While;
                s->expr = compile_expr(*stmt.while_stmt.condition);
                s->while_body = compile_stmt(*stmt.while_stmt.body);
                return s;
            }
            case Stmt::Kind::Break: {
                CStmt* s = new_stmt();
                s->kind = CStmt::Kind::Break;
                return s;
            }
            case Stmt::Kind::Continue: {
                CStmt* s = new_stmt();
                s->kind = CStmt::Kind::Continue;
                return s;
            }
            case Stmt::Kind::Return: {
                CStmt* s = new_stmt();
                if (!stmt.return_stmt.value) {
                    s->kind = CStmt::Kind::Return;
                    return s;
                }
                const Expr& value = **stmt.return_stmt.value;
                // Self tail call: `return self(...)` becomes a loop in
                // call_function so linear self-recursion cannot exhaust the stack.
                if (value.kind == Expr::Kind::Call) {
                    auto it = sema_.calls.find(&value.call);
                    if (it != sema_.calls.end() && it->second == current_compile_fn_) {
                        s->kind = CStmt::Kind::TailCall;
                        s->tail_args.reserve(value.call.args.size());
                        for (const std::unique_ptr<Expr>& arg : value.call.args) {
                            s->tail_args.push_back(compile_expr(*arg));
                        }
                        return s;
                    }
                }
                s->kind = CStmt::Kind::Return;
                s->expr = compile_expr(value);
                return s;
            }
        }
        throw EvalAbort{};
    }

    const CFunc* find_main() const {
        auto it = functions_.find("main");
        if (it == functions_.end()) {
            return nullptr;
        }
        auto cf = cfunc_by_def_.find(it->second);
        return cf == cfunc_by_def_.end() ? nullptr : cf->second;
    }

    bool is_pure(const FuncDef* fn) const {
        auto it = pure_.find(fn);
        return it != pure_.end() && it->second;
    }

    // ---- execution ---------------------------------------------------------

    i32 call_function(const CFunc& cfunc, std::vector<i32> args) {
        if (++depth_ > budget_.max_call_depth) {
            throw EvalAbort{};
        }
        const bool pure = cfunc.pure;
        if (pure) {
            auto it = memo_.find(MemoKey{cfunc.fn, args});
            if (it != memo_.end()) {
                --depth_;
                return it->second;
            }
        }
        std::vector<i32> memo_args = pure ? args : std::vector<i32>{};

        const std::size_t nparams = cfunc.fn->params.size();
        i32 result = 0;
        while (true) {  // loop instead of recursing on self tail calls
            Frame frame(cfunc.frame_size, 0);
            for (std::size_t i = 0; i < nparams && i < args.size(); ++i) {
                frame[i] = args[i];
            }
            i32 return_slot = 0;
            Flow flow = exec_stmt(*cfunc.body, frame, return_slot);
            if (flow == Flow::TailCall) {
                args = std::move(tail_args_);
                continue;
            }
            result = (flow == Flow::Return) ? return_slot : 0;
            break;
        }

        if (pure && memo_.size() < budget_.max_memo_entries) {
            memo_.emplace(MemoKey{cfunc.fn, std::move(memo_args)}, result);
        }
        --depth_;
        return result;
    }

    Flow exec_stmt(const CStmt& stmt, Frame& frame, i32& return_slot) {
        tick();
        switch (stmt.kind) {
            case CStmt::Kind::Block:
                for (const CStmt* child : stmt.body) {
                    Flow flow = exec_stmt(*child, frame, return_slot);
                    if (flow != Flow::Normal) {
                        return flow;
                    }
                }
                return Flow::Normal;
            case CStmt::Kind::ExprStmt:
                eval_expr(*stmt.expr, frame);
                return Flow::Normal;
            case CStmt::Kind::Assign: {
                i32 value = eval_expr(*stmt.expr, frame);
                if (stmt.loc.global) {
                    globals_[stmt.loc.index] = value;
                } else {
                    frame[stmt.loc.index] = value;
                }
                return Flow::Normal;
            }
            case CStmt::Kind::If:
                if (eval_expr(*stmt.expr, frame) != 0) {
                    return exec_stmt(*stmt.then_stmt, frame, return_slot);
                }
                if (stmt.else_stmt) {
                    return exec_stmt(*stmt.else_stmt, frame, return_slot);
                }
                return Flow::Normal;
            case CStmt::Kind::While:
                while (eval_expr(*stmt.expr, frame) != 0) {
                    tick();
                    Flow flow = exec_stmt(*stmt.while_body, frame, return_slot);
                    if (flow == Flow::Break) {
                        break;
                    }
                    if (flow == Flow::Continue) {
                        continue;
                    }
                    if (flow == Flow::Return || flow == Flow::TailCall) {
                        return flow;
                    }
                }
                return Flow::Normal;
            case CStmt::Kind::Break:
                return Flow::Break;
            case CStmt::Kind::Continue:
                return Flow::Continue;
            case CStmt::Kind::Return:
                if (stmt.expr) {
                    return_slot = eval_expr(*stmt.expr, frame);
                }
                return Flow::Return;
            case CStmt::Kind::TailCall: {
                std::vector<i32> args;
                args.reserve(stmt.tail_args.size());
                for (const CExpr* arg : stmt.tail_args) {
                    args.push_back(eval_expr(*arg, frame));
                }
                tail_args_ = std::move(args);
                return Flow::TailCall;
            }
        }
        return Flow::Normal;
    }

    i32 eval_expr(const CExpr& expr, Frame& frame) {
        tick();
        switch (expr.kind) {
            case CExpr::Kind::Const:
                return expr.const_value;
            case CExpr::Kind::Load:
                return expr.loc.global ? globals_[expr.loc.index]
                                       : frame[expr.loc.index];
            case CExpr::Kind::And:
                return (eval_expr(*expr.a, frame) != 0 &&
                        eval_expr(*expr.b, frame) != 0)
                           ? 1
                           : 0;
            case CExpr::Kind::Or:
                return (eval_expr(*expr.a, frame) != 0 ||
                        eval_expr(*expr.b, frame) != 0)
                           ? 1
                           : 0;
            case CExpr::Kind::Binary:
                return eval_binary(expr, frame);
            case CExpr::Kind::Unary: {
                const i32 value = eval_expr(*expr.a, frame);
                switch (expr.uop) {
                    case UnaryOp::Plus:
                        return value;
                    case UnaryOp::Minus:
                        return wrap32(-static_cast<i64>(value));
                    case UnaryOp::Not:
                        return value == 0 ? 1 : 0;
                }
                throw EvalAbort{};
            }
            case CExpr::Kind::Call: {
                std::vector<i32> args;
                args.reserve(expr.args.size());
                for (const CExpr* arg : expr.args) {
                    args.push_back(eval_expr(*arg, frame));
                }
                return call_function(*expr.callee, std::move(args));
            }
        }
        throw EvalAbort{};
    }

    i32 eval_binary(const CExpr& expr, Frame& frame) {
        const i32 lhs = eval_expr(*expr.a, frame);
        const i32 rhs = eval_expr(*expr.b, frame);
        switch (expr.bop) {
            case BinaryOp::Add:
                return wrap32(static_cast<i64>(lhs) + rhs);
            case BinaryOp::Sub:
                return wrap32(static_cast<i64>(lhs) - rhs);
            case BinaryOp::Mul:
                return wrap32(static_cast<i64>(lhs) * rhs);
            case BinaryOp::Div:
                if (rhs == 0) {
                    throw EvalAbort{};
                }
                return wrap32(static_cast<i64>(lhs) / rhs);
            case BinaryOp::Mod:
                if (rhs == 0) {
                    throw EvalAbort{};
                }
                return wrap32(static_cast<i64>(lhs) % rhs);
            case BinaryOp::Lt:
                return lhs < rhs ? 1 : 0;
            case BinaryOp::Le:
                return lhs <= rhs ? 1 : 0;
            case BinaryOp::Gt:
                return lhs > rhs ? 1 : 0;
            case BinaryOp::Ge:
                return lhs >= rhs ? 1 : 0;
            case BinaryOp::Eq:
                return lhs == rhs ? 1 : 0;
            case BinaryOp::Ne:
                return lhs != rhs ? 1 : 0;
            case BinaryOp::And:
            case BinaryOp::Or:
                break;  // handled via CExpr::Kind::And/Or
        }
        throw EvalAbort{};
    }

    const CompUnit& unit_;
    const SemaResult& sema_;
    EvalBudget budget_;

    std::unordered_map<std::string, const FuncDef*> functions_;
    std::unordered_map<const void*, VarLoc> loc_;
    std::unordered_map<const FuncDef*, std::uint32_t> frame_size_;
    std::unordered_map<const FuncDef*, bool> pure_;

    std::deque<CExpr> cexprs_;
    std::deque<CStmt> cstmts_;
    std::deque<CFunc> cfuncs_;
    std::unordered_map<const FuncDef*, const CFunc*> cfunc_by_def_;
    const FuncDef* current_compile_fn_ = nullptr;

    std::vector<std::pair<std::uint32_t, const Expr*>> global_init_ast_;
    std::vector<std::pair<std::uint32_t, const CExpr*>> global_inits_;

    std::vector<i32> globals_;
    std::unordered_map<MemoKey, i32, MemoKeyHash> memo_;

    std::vector<i32> tail_args_;
    std::uint64_t steps_ = 0;
    unsigned depth_ = 0;
    std::chrono::steady_clock::time_point deadline_;
};

}  // namespace

std::optional<std::int32_t> evaluate_program(const CompUnit& unit,
                                             const SemaResult& sema,
                                             const EvalBudget& budget) {
    Evaluator evaluator(unit, sema, budget);
    return evaluator.run();
}

}  // namespace toyc
