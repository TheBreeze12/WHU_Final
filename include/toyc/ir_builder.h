#pragma once

#include "toyc/ir.h"

#include <type_traits>

namespace toyc {

class IRBuilder {
public:
    IRBuilder(Module& module, BasicBlock* insert = nullptr)
        : module_(module), insert_(insert) {}

    void set_insert_point(BasicBlock* bb) { insert_ = bb; }
    BasicBlock* insert_point() const { return insert_; }

    Value* create_alloca();
    Value* create_load(Value* ptr);
    void   create_store(Value* ptr, Value* value);
    Value* create_binary(Opcode opcode, Value* lhs, Value* rhs);
    Value* create_icmp(Opcode opcode, Value* lhs, Value* rhs);
    Value* create_neg(Value* operand);
    void   create_br(BasicBlock* target);
    void   create_cond_br(Value* cond, BasicBlock* t, BasicBlock* f);
    void   create_ret(Value* value = nullptr);
    Value* create_call(const std::string& callee, std::vector<Value*> args, bool returns_void);
    PhiInst* create_phi();
    Value* create_shl(Value* value, unsigned amount);
    Value* create_shr(Value* value, unsigned amount);

private:
    template <typename InstT>
    InstT* emit(std::unique_ptr<InstT> inst) {
        static_assert(std::is_base_of_v<Instruction, InstT>);
        InstT* raw = inst.get();
        insert_->push_back(std::move(inst));
        return raw;
    }

    Module& module_;
    BasicBlock* insert_;
};

}  // namespace toyc
