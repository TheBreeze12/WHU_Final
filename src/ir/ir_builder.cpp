#include "toyc/ir_builder.h"

namespace toyc {

Value* IRBuilder::create_alloca() {
    return emit(std::make_unique<AllocaInst>(module_.fresh_id()));
}

Value* IRBuilder::create_load(Value* ptr) {
    return emit(std::make_unique<LoadInst>(ptr, module_.fresh_id()));
}

void IRBuilder::create_store(Value* ptr, Value* value) {
    emit(std::make_unique<StoreInst>(ptr, value));
}

Value* IRBuilder::create_binary(Opcode opcode, Value* lhs, Value* rhs) {
    return emit(std::make_unique<BinaryInst>(opcode, lhs, rhs, module_.fresh_id()));
}

Value* IRBuilder::create_icmp(Opcode opcode, Value* lhs, Value* rhs) {
    return emit(std::make_unique<ICmpInst>(opcode, lhs, rhs, module_.fresh_id()));
}

Value* IRBuilder::create_neg(Value* operand) {
    return emit(std::make_unique<NegInst>(operand, module_.fresh_id()));
}

void IRBuilder::create_br(BasicBlock* target) {
    emit(std::make_unique<BrInst>(target));
}

void IRBuilder::create_cond_br(Value* cond, BasicBlock* t, BasicBlock* f) {
    emit(std::make_unique<CondBrInst>(cond, t, f));
}

void IRBuilder::create_ret(Value* value) {
    emit(std::make_unique<RetInst>(value));
}

Value* IRBuilder::create_call(const std::string& callee, std::vector<Value*> args, bool returns_void) {
    return emit(std::make_unique<CallInst>(callee, std::move(args), returns_void, module_.fresh_id()));
}

PhiInst* IRBuilder::create_phi() {
    return emit(std::make_unique<PhiInst>(module_.fresh_id()));
}

Value* IRBuilder::create_shl(Value* value, unsigned amount) {
    return emit(std::make_unique<ShlInst>(value, amount, module_.fresh_id()));
}

Value* IRBuilder::create_shr(Value* value, unsigned amount) {
    return emit(std::make_unique<ShrInst>(value, amount, module_.fresh_id()));
}

}  // namespace toyc
