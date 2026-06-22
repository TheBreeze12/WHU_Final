#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace toyc {

enum class Type { I32, Ptr, Void, Label };
const char* type_name(Type type);

enum class ValueKind { Constant, GlobalAddr, BasicBlock, Function, Register, Param };

class User;

class Value {
public:
    Value(Type type, ValueKind kind, unsigned id = 0)
        : type_(type), kind_(kind), id_(id) {}
    virtual ~Value() = default;

    Type type() const { return type_; }
    ValueKind value_kind() const { return kind_; }
    unsigned id() const { return id_; }
    void set_id(unsigned id) { id_ = id; }

    virtual std::string name() const;

    const std::vector<User*>& uses() const { return uses_; }
    void add_use(User* user);
    void remove_use(User* user);
    void replace_all_uses_with(Value* other);

protected:
    ValueKind kind() const { return kind_; }

private:
    Type type_;
    ValueKind kind_;
    unsigned id_ = 0;
    std::vector<User*> uses_;
};

class User : public Value {
public:
    using Value::Value;

    unsigned num_operands() const { return static_cast<unsigned>(operands_.size()); }
    Value* operand(unsigned i) const { return operands_[i]; }
    const std::vector<Value*>& operands() const { return operands_; }

    void add_operand(Value* value);
    void set_operand(unsigned i, Value* value);

protected:
    std::vector<Value*> operands_;
};

class ConstantInt : public Value {
public:
    explicit ConstantInt(int value) : Value(Type::I32, ValueKind::Constant, 0), value_(value) {}
    std::string name() const override { return std::to_string(value_); }
    int value() const { return value_; }
private:
    int value_;
};

class GlobalAddr : public Value {
public:
    explicit GlobalAddr(std::string label)
        : Value(Type::Ptr, ValueKind::GlobalAddr, 0), label_(std::move(label)) {}
    std::string name() const override { return "@" + label_; }
    const std::string& label() const { return label_; }
private:
    std::string label_;
};

struct GlobalVar {
    GlobalAddr* addr = nullptr;
    ConstantInt* init = nullptr;
    bool is_const = false;
};

class Function;  // forward
class Module;

class Module {
public:
    Module() = default;

    ConstantInt* get_constant(int value);
    GlobalVar* create_global(const std::string& name, int init_value, bool is_const);

    unsigned fresh_id() { return value_counter_++; }
    Value* create_register(Type type);

    const std::vector<std::unique_ptr<GlobalVar>>& globals() const { return globals_; }

private:
    std::unordered_map<int, std::unique_ptr<ConstantInt>> constants_;
    std::vector<std::unique_ptr<GlobalAddr>> global_addrs_;
    std::vector<std::unique_ptr<GlobalVar>> globals_;
    std::vector<std::unique_ptr<Value>> registers_;
    unsigned value_counter_ = 0;
};

}  // namespace toyc
