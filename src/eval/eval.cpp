#include "toyc/eval.h"

#include "toyc/ast.h"
#include "toyc/sema.h"

#include <chrono>
#include <cstdint>
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

// Storage location of a declaration, resolved once in the pre-pass. Locals and
// params index into the current call frame's flat vector; globals index into the
// shared global vector. Flat indexing avoids per-access hashing in hot loops.
struct VarLoc {
    bool global = false;
    std::uint32_t index = 0;
};

using Frame = std::vector<i32>;

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
            prepass();
            const FuncDef* main = find_main();
            if (!main || !main->body) {
                return std::nullopt;
            }
            classify_purity();
            std::vector<i32> no_args;
            return call_function(*main, std::move(no_args));
        } catch (const EvalAbort&) {
            return std::nullopt;
        }
    }

private:
    inline void tick() {
        // Check the wall clock / step ceiling only periodically to keep the hot
        // path cheap.
        if ((++steps_ & 0xFFFFu) == 0) {
            if (steps_ > budget_.max_steps ||
                std::chrono::steady_clock::now() > deadline_) {
                throw EvalAbort{};
            }
        }
    }

    // ---- pre-pass: assign flat storage slots and evaluate global inits -----

    void prepass() {
        Frame empty;
        std::uint32_t gidx = 0;
        for (const CompUnit::Item& item : unit_.items) {
            switch (item.kind) {
                case CompUnit::ItemKind::GlobalConst: {
                    loc_[&item.global_const] = VarLoc{true, gidx};
                    globals_.push_back(0);
                    globals_[gidx] = eval_expr(*item.global_const.init, empty);
                    ++gidx;
                    break;
                }
                case CompUnit::ItemKind::GlobalVar: {
                    loc_[&item.global_var] = VarLoc{true, gidx};
                    globals_.push_back(0);
                    globals_[gidx] = eval_expr(*item.global_var.init, empty);
                    ++gidx;
                    break;
                }
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

    const FuncDef* find_main() const {
        auto it = functions_.find("main");
        return it == functions_.end() ? nullptr : it->second;
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

        // Start optimistic (pure unless it touches a global var), then propagate
        // impurity across call edges to a fixed point.
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

    bool is_pure(const FuncDef* fn) const {
        auto it = pure_.find(fn);
        return it != pure_.end() && it->second;
    }

    // ---- variable access ---------------------------------------------------

    i32 load_var(const void* decl, Frame& frame) {
        auto it = loc_.find(decl);
        if (it == loc_.end()) {
            throw EvalAbort{};
        }
        return it->second.global ? globals_[it->second.index]
                                 : frame[it->second.index];
    }

    void store_var(const void* decl, Frame& frame, i32 value) {
        auto it = loc_.find(decl);
        if (it == loc_.end()) {
            throw EvalAbort{};
        }
        if (it->second.global) {
            globals_[it->second.index] = value;
        } else {
            frame[it->second.index] = value;
        }
    }

    // ---- execution ---------------------------------------------------------

    i32 call_function(const FuncDef& fn, std::vector<i32> args) {
        if (++depth_ > budget_.max_call_depth) {
            throw EvalAbort{};
        }
        const bool pure = is_pure(&fn);
        if (pure) {
            auto it = memo_.find(MemoKey{&fn, args});
            if (it != memo_.end()) {
                --depth_;
                return it->second;
            }
        }
        const std::vector<i32> memo_args = pure ? args : std::vector<i32>{};

        std::uint32_t size = static_cast<std::uint32_t>(fn.params.size());
        if (auto it = frame_size_.find(&fn); it != frame_size_.end()) {
            size = it->second;
        }

        i32 result = 0;
        while (true) {  // loop instead of recursing on self tail calls
            Frame frame(size, 0);
            for (std::size_t i = 0; i < fn.params.size() && i < args.size(); ++i) {
                frame[i] = args[i];
            }
            current_fn_ = &fn;
            i32 return_slot = 0;
            Flow flow = exec_block(*fn.body, frame, return_slot);
            if (flow == Flow::TailCall) {
                args = std::move(tail_args_);
                continue;
            }
            result = (flow == Flow::Return) ? return_slot : 0;
            break;
        }

        if (pure && memo_.size() < budget_.max_memo_entries) {
            memo_.emplace(MemoKey{&fn, memo_args}, result);
        }
        --depth_;
        return result;
    }

    Flow exec_block(const BlockStmt& block, Frame& frame, i32& return_slot) {
        for (const std::unique_ptr<Stmt>& stmt : block.body) {
            if (!stmt) {
                continue;
            }
            Flow flow = exec_stmt(*stmt, frame, return_slot);
            if (flow != Flow::Normal) {
                return flow;
            }
        }
        return Flow::Normal;
    }

    Flow exec_stmt(const Stmt& stmt, Frame& frame, i32& return_slot) {
        tick();
        switch (stmt.kind) {
            case Stmt::Kind::Block:
                return exec_block(stmt.block, frame, return_slot);
            case Stmt::Kind::Empty:
                return Flow::Normal;
            case Stmt::Kind::Expr:
                eval_expr(*stmt.expr.expr, frame);
                return Flow::Normal;
            case Stmt::Kind::Assign: {
                i32 value = eval_expr(*stmt.assign.value, frame);
                auto it = sema_.assigns.find(&stmt.assign);
                if (it == sema_.assigns.end()) {
                    throw EvalAbort{};
                }
                store_var(it->second.decl, frame, value);
                return Flow::Normal;
            }
            case Stmt::Kind::ConstDecl:
                store_var(&stmt.const_decl, frame,
                          eval_expr(*stmt.const_decl.init, frame));
                return Flow::Normal;
            case Stmt::Kind::VarDecl:
                store_var(&stmt.var_decl, frame,
                          eval_expr(*stmt.var_decl.init, frame));
                return Flow::Normal;
            case Stmt::Kind::If: {
                if (eval_expr(*stmt.if_stmt.condition, frame) != 0) {
                    return exec_stmt(*stmt.if_stmt.then_branch, frame, return_slot);
                }
                if (stmt.if_stmt.else_branch) {
                    return exec_stmt(*stmt.if_stmt.else_branch, frame, return_slot);
                }
                return Flow::Normal;
            }
            case Stmt::Kind::While: {
                while (eval_expr(*stmt.while_stmt.condition, frame) != 0) {
                    tick();
                    Flow flow = exec_stmt(*stmt.while_stmt.body, frame, return_slot);
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
            }
            case Stmt::Kind::Break:
                return Flow::Break;
            case Stmt::Kind::Continue:
                return Flow::Continue;
            case Stmt::Kind::Return: {
                if (!stmt.return_stmt.value) {
                    return Flow::Return;
                }
                const Expr& value = **stmt.return_stmt.value;
                // Self tail call: `return self(...)` runs as a loop in
                // call_function so linear self-recursion cannot exhaust the stack.
                if (value.kind == Expr::Kind::Call) {
                    auto it = sema_.calls.find(&value.call);
                    if (it != sema_.calls.end() && it->second == current_fn_) {
                        std::vector<i32> args;
                        args.reserve(value.call.args.size());
                        for (const std::unique_ptr<Expr>& arg : value.call.args) {
                            args.push_back(eval_expr(*arg, frame));
                        }
                        tail_args_ = std::move(args);
                        return Flow::TailCall;
                    }
                }
                return_slot = eval_expr(value, frame);
                return Flow::Return;
            }
        }
        return Flow::Normal;
    }

    i32 eval_expr(const Expr& expr, Frame& frame) {
        tick();
        switch (expr.kind) {
            case Expr::Kind::IntLiteral:
                return static_cast<i32>(expr.int_literal.value);
            case Expr::Kind::Ident: {
                auto it = sema_.idents.find(&expr.ident);
                if (it == sema_.idents.end()) {
                    throw EvalAbort{};
                }
                return load_var(it->second.decl, frame);
            }
            case Expr::Kind::Binary:
                return eval_binary(expr.binary, frame);
            case Expr::Kind::Unary:
                return eval_unary(expr.unary, frame);
            case Expr::Kind::Call:
                return eval_call(expr.call, frame);
        }
        throw EvalAbort{};
    }

    i32 eval_binary(const BinaryExpr& binary, Frame& frame) {
        if (binary.op == BinaryOp::And) {
            return (eval_expr(*binary.lhs, frame) != 0 &&
                    eval_expr(*binary.rhs, frame) != 0)
                       ? 1
                       : 0;
        }
        if (binary.op == BinaryOp::Or) {
            return (eval_expr(*binary.lhs, frame) != 0 ||
                    eval_expr(*binary.rhs, frame) != 0)
                       ? 1
                       : 0;
        }

        const i32 lhs = eval_expr(*binary.lhs, frame);
        const i32 rhs = eval_expr(*binary.rhs, frame);
        switch (binary.op) {
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
                break;  // handled above
        }
        throw EvalAbort{};
    }

    i32 eval_unary(const UnaryExpr& unary, Frame& frame) {
        const i32 value = eval_expr(*unary.operand, frame);
        switch (unary.op) {
            case UnaryOp::Plus:
                return value;
            case UnaryOp::Minus:
                return wrap32(-static_cast<i64>(value));
            case UnaryOp::Not:
                return value == 0 ? 1 : 0;
        }
        throw EvalAbort{};
    }

    i32 eval_call(const CallExpr& call, Frame& frame) {
        auto it = sema_.calls.find(&call);
        if (it == sema_.calls.end() || !it->second || !it->second->body) {
            throw EvalAbort{};
        }
        const FuncDef& callee = *it->second;
        std::vector<i32> args;
        args.reserve(call.args.size());
        for (const std::unique_ptr<Expr>& arg : call.args) {
            args.push_back(eval_expr(*arg, frame));
        }
        return call_function(callee, std::move(args));
    }

    const CompUnit& unit_;
    const SemaResult& sema_;
    EvalBudget budget_;

    std::unordered_map<std::string, const FuncDef*> functions_;
    std::unordered_map<const void*, VarLoc> loc_;
    std::unordered_map<const FuncDef*, std::uint32_t> frame_size_;
    std::vector<i32> globals_;
    std::unordered_map<const FuncDef*, bool> pure_;
    std::unordered_map<MemoKey, i32, MemoKeyHash> memo_;

    const FuncDef* current_fn_ = nullptr;
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
