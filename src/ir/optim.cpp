#include "toyc/optim.h"

#include "toyc/ir.h"
#include "toyc/mem2reg.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace toyc {

namespace {

// Erase every instruction in `dead`: first detach it from its operands' use
// lists (so no dangling User* survives), then remove it from its block.
void erase_dead(Function& fn, const std::unordered_set<Instruction*>& dead) {
    for (const std::unique_ptr<BasicBlock>& owner : fn.blocks()) {
        for (const std::unique_ptr<Instruction>& inst : owner->insts()) {
            if (!dead.count(inst.get())) continue;
            for (unsigned k = 0; k < inst->num_operands(); ++k) {
                if (Value* op = inst->operand(k)) op->remove_use(inst.get());
            }
        }
    }
    for (const std::unique_ptr<BasicBlock>& owner : fn.blocks()) {
        std::list<std::unique_ptr<Instruction>>& insts = owner->insts();
        for (auto it = insts.begin(); it != insts.end();) {
            if (dead.count(it->get())) {
                it = insts.erase(it);
            } else {
                ++it;
            }
        }
    }
}

std::optional<int> eval_binary(Opcode op, int a, int b) {
    switch (op) {
        case Opcode::Add: return a + b;
        case Opcode::Sub: return a - b;
        case Opcode::Mul: return a * b;
        case Opcode::Sdiv: if (b == 0) return std::nullopt; return a / b;
        case Opcode::Srem: if (b == 0) return std::nullopt; return a % b;
        case Opcode::ICmpEq: return a == b ? 1 : 0;
        case Opcode::ICmpNe: return a != b ? 1 : 0;
        case Opcode::ICmpSlt: return a < b ? 1 : 0;
        case Opcode::ICmpSgt: return a > b ? 1 : 0;
        case Opcode::ICmpSle: return a <= b ? 1 : 0;
        case Opcode::ICmpSge: return a >= b ? 1 : 0;
        default: return std::nullopt;
    }
}

struct GvnKey {
    int op;
    std::uintptr_t a;
    std::uintptr_t b;
    bool operator==(const GvnKey&) const = default;
};

struct GvnKeyHash {
    std::size_t operator()(const GvnKey& k) const noexcept {
        return static_cast<std::size_t>(k.op) * 1315423911u ^
               (k.a * 2654435761u) ^ (k.b * 40503u);
    }
};

bool gvn_cseable(Opcode op) {
    switch (op) {
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
        case Opcode::Sdiv: case Opcode::Srem:
        case Opcode::ICmpEq: case Opcode::ICmpNe: case Opcode::ICmpSlt:
        case Opcode::ICmpSgt: case Opcode::ICmpSle: case Opcode::ICmpSge:
        case Opcode::Neg:
            return true;
        default:
            return false;
    }
}

GvnKey gvn_key(Instruction* inst) {
    Opcode op = inst->opcode();
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(inst->operand(0));
    std::uintptr_t b = inst->num_operands() >= 2
                           ? reinterpret_cast<std::uintptr_t>(inst->operand(1))
                           : 0;
    if (op == Opcode::ICmpSgt) { op = Opcode::ICmpSlt; std::swap(a, b); }
    if (op == Opcode::ICmpSge) { op = Opcode::ICmpSle; std::swap(a, b); }
    if (op == Opcode::Add || op == Opcode::Mul ||
        op == Opcode::ICmpEq || op == Opcode::ICmpNe) {
        if (a > b) std::swap(a, b);
    }
    return {static_cast<int>(op), a, b};
}

void gvn_walk(BasicBlock* bb, DominatorTree& dt,
              std::unordered_map<GvnKey, Value*, GvnKeyHash>& avail,
              std::unordered_set<Instruction*>& dead, bool& changed) {
    std::vector<GvnKey> inserted;
    for (const std::unique_ptr<Instruction>& inst_owner : bb->insts()) {
        Instruction* inst = inst_owner.get();
        if (!gvn_cseable(inst->opcode())) continue;
        GvnKey k = gvn_key(inst);
        auto it = avail.find(k);
        if (it != avail.end()) {
            inst->replace_all_uses_with(it->second);
            dead.insert(inst);
            changed = true;
        } else {
            avail.emplace(k, inst);
            inserted.push_back(k);
        }
    }
    for (BasicBlock* c : dt.children(bb)) gvn_walk(c, dt, avail, dead, changed);
    for (const GvnKey& k : inserted) avail.erase(k);
}

}  // namespace

// Filled in by Tasks 2-5; wired by Task 6.
bool constprop(Function& fn) {
    bool changed = false;
    bool local_changed = true;
    while (local_changed) {
        local_changed = false;
        std::unordered_set<Instruction*> dead;
        for (const std::unique_ptr<BasicBlock>& owner : fn.blocks()) {
            for (const std::unique_ptr<Instruction>& inst_owner : owner->insts()) {
                Instruction* inst = inst_owner.get();
                if (!inst->has_result()) continue;
                Opcode op = inst->opcode();
                ConstantInt* folded = nullptr;

                if (op == Opcode::Phi) {
                    if (inst->num_operands() == 0) continue;
                    Value* first = inst->operand(0);
                    if (first->value_kind() != ValueKind::Constant) continue;
                    int v = static_cast<ConstantInt*>(first)->value();
                    bool same = true;
                    for (unsigned k = 1; k < inst->num_operands(); ++k) {
                        Value* o = inst->operand(k);
                        if (o->value_kind() != ValueKind::Constant ||
                            static_cast<ConstantInt*>(o)->value() != v) { same = false; break; }
                    }
                    if (same) folded = fn.module()->get_constant(v);
                } else if (op == Opcode::Neg) {
                    Value* o = inst->operand(0);
                    if (o->value_kind() == ValueKind::Constant) {
                        folded = fn.module()->get_constant(-static_cast<ConstantInt*>(o)->value());
                    }
                } else if (inst->num_operands() == 2) {
                    Value* a = inst->operand(0);
                    Value* b = inst->operand(1);
                    if (a->value_kind() == ValueKind::Constant &&
                        b->value_kind() == ValueKind::Constant) {
                        auto r = eval_binary(op,
                            static_cast<ConstantInt*>(a)->value(),
                            static_cast<ConstantInt*>(b)->value());
                        if (r) folded = fn.module()->get_constant(*r);
                    }
                }

                if (folded) {
                    inst->replace_all_uses_with(folded);
                    dead.insert(inst);
                    changed = local_changed = true;
                }
            }
        }
        if (!dead.empty()) erase_dead(fn, dead);
    }
    return changed;
}
bool dce(Function& fn) {
    std::unordered_set<Instruction*> live;
    std::vector<Instruction*> work;
    for (const std::unique_ptr<BasicBlock>& owner : fn.blocks()) {
        for (const std::unique_ptr<Instruction>& inst_owner : owner->insts()) {
            Instruction* inst = inst_owner.get();
            Opcode op = inst->opcode();
            bool essential = (op == Opcode::Store || op == Opcode::Call ||
                              op == Opcode::Ret || op == Opcode::Br || op == Opcode::CondBr);
            if (essential) {
                live.insert(inst);
                work.push_back(inst);
            }
        }
    }
    while (!work.empty()) {
        Instruction* inst = work.back();
        work.pop_back();
        for (unsigned k = 0; k < inst->num_operands(); ++k) {
            Value* op = inst->operand(k);
            if (!op) continue;  // null operand (e.g. void ret)
            if (op->value_kind() != ValueKind::Register) continue;  // constants/params/blocks: not instructions
            // Not all ValueKind::Register are Instructions (e.g. Module::create_register)
            // Check if it's actually an instruction by checking if it has an opcode (Instructions do)
            Instruction* def = dynamic_cast<Instruction*>(op);
            if (!def) continue;  // skip non-instruction registers
            if (!live.count(def)) {
                live.insert(def);
                work.push_back(def);
            }
        }
    }
    std::unordered_set<Instruction*> dead;
    for (const std::unique_ptr<BasicBlock>& owner : fn.blocks()) {
        for (const std::unique_ptr<Instruction>& inst_owner : owner->insts()) {
            if (!live.count(inst_owner.get())) dead.insert(inst_owner.get());
        }
    }
    if (dead.empty()) return false;
    erase_dead(fn, dead);
    return true;
}
bool gvn(Function& fn) {
    if (!fn.entry()) return false;
    DominatorTree dt;
    dt.analyze(fn);
    std::unordered_map<GvnKey, Value*, GvnKeyHash> avail;
    std::unordered_set<Instruction*> dead;
    bool changed = false;
    gvn_walk(fn.entry(), dt, avail, dead, changed);
    if (!dead.empty()) erase_dead(fn, dead);
    return changed;
}
bool cfs(Function& /*fn*/) { return false; }

bool run_optim(Module& /*module*/) { return false; }

}  // namespace toyc
