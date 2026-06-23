# IR Foundation Implementation Plan (P0 + P1 + P2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the linear-SSA IR data structure (`Value`/`User` hierarchy + `Module`/`Function`/`BasicBlock`), its text printer (`-dump-ir` format), and a construction helper (`IRBuilder`) — all testable by hand-constructing IR, independent of the (not-yet-existing) Sema/IRGen/codegen.

**Architecture:** LLVM-style `Value`/`User` graph (bidirectional use-def chains, the substrate for later SSA optimization). `Module` owns `Function`s own `BasicBlock`s own `Instruction`s via `unique_ptr`; values are referenced by raw `Value*`. Naming is `%v.N` (registers), `%arg.N` (params), `entry`/`bbN` (blocks), `@name` (globals/functions), bare integer literals (constants). The printer emits the format frozen in `docs/superpowers/specs/2026-06-22-toyc-ir-optim-design.md` §4.5.

**Tech Stack:** C++20, CMake 3.20+, CTest. No third-party libs (project rule: no LLVM/Clang backend). No unit-test framework — a ~25-line in-repo check helper + one test binary.

## Global Constraints

- C++20, `-Wall -Wextra -Wpedantic` (already set in `CMakeLists.txt`).
- Headers: `#pragma once`, `#include "toyc/xxx.h"` for project headers, `<std>` for stdlib, `namespace toyc { ... }  // namespace toyc`.
- snake_case for all names; `enum class` with PascalCase enumerators; structs/classes with public members or accessors.
- IR uses **inheritance-based `Value`/`User`** (deliberately diverging from frontend's tagged-union `ast.h` — the design doc §4.1 mandates this for SSA use-def chains).
- Fail fast: no silent fallbacks (project global rule). Raw `Value*` are non-owning and assumed valid while the owning `Module` is alive.
- Ownership: `Module` is the single owner; destruction is wholesale (no partial teardown). Pass-side deletion is handled by later plans (mem2reg/opt).
- Test commands assume the build dir is `build/` (already exists & configured). If re-configuring is needed: `cmake -S . -B build`.
- Authority for IR semantics/format: `docs/superpowers/specs/2026-06-22-toyc-ir-optim-design.md` (this plan implements §4, §4.5, §7 instruction set).

---

## File Structure

| File | Responsibility |
|------|----------------|
| `include/toyc/ir.h` | Core IR: `Type`, `Value`, `User`, `ConstantInt`, `GlobalAddr`, `GlobalVar`, `Opcode`, `Instruction` + all subclasses, `BasicBlock`, `Function`, `Module`. (One cohesive header, mirrors `ast.h` granularity.) |
| `src/ir/ir.cpp` | Implementations: virtual dtors, use-list maintenance, `replace_all_uses_with`, numbering/factories, `name()`, `*_name()` helpers. |
| `include/toyc/ir_printer.h` | `print_module(const Module&, std::ostream&)`. |
| `src/ir/ir_printer.cpp` | Format per design §4.5 (globals, `define`, blocks, instructions, `}`). |
| `include/toyc/ir_builder.h` | `IRBuilder`: block-positioning + `create_*` convenience. |
| `src/ir/ir_builder.cpp` | Builder implementations. |
| `test/ir/check.h` | Minimal test helpers (`check`, `check_eq_str`, `report`). |
| `test/ir/ir_tests.cpp` | All foundation unit tests; `main()` runs them. |
| `CMakeLists.txt` | Add `src/ir/*.cpp` to the compiler target; add `toyc-ir-tests` executable + CTest entry. |

**Decomposition rationale:** one header per concern (matches frontend: `ast.h`, `lexer.h`, …). `ir.h` is the representation; `ir_printer` is its serialization; `ir_builder` is its construction sugar. Tests are one binary with a tiny helper (not a framework — avoids needless abstraction).

---

## Task 1: Type system + `Value`/`User` core with use-lists

**Files:**
- Create: `include/toyc/ir.h`, `src/ir/ir.cpp`, `test/ir/check.h`, `test/ir/ir_tests.cpp`
- Modify: `CMakeLists.txt:8-19` (add `src/ir/ir.cpp`), `CMakeLists.txt:36` (append test target after existing `add_test`)

**Interfaces:**
- Produces: `enum class Type { I32, Ptr, Void, Label }`; `const char* type_name(Type)`; `class Value` (with `type()`, `value_kind()`, `name()`, `uses()`, `replace_all_uses_with(Value*)`); `enum class ValueKind`; `class User : public Value` (`operand(i)`, `num_operands()`, `set_operand(i, v)`).

- [ ] **Step 1: Write the failing test**

Create `test/ir/check.h`:

```cpp
#pragma once

#include <iostream>
#include <string>

namespace toyc::test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void check(bool cond, const std::string& msg) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cerr << "  FAIL: " << msg << "\n";
    }
}

inline void check_eq_str(const std::string& expected, const std::string& actual, const std::string& msg) {
    ++g_checks;
    if (expected == actual) {
        return;
    }
    ++g_failures;
    std::cerr << "  FAIL: " << msg << "\n--- expected ---\n" << expected
              << "\n--- actual ---\n" << actual << "\n";
}

inline int report() {
    std::cerr << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}

}  // namespace toyc::test
```

Create `test/ir/ir_tests.cpp` with only Task 1's test for now (other tests appended in later tasks):

```cpp
#include "toyc/ir.h"

#include "check.h"

#include <vector>

using namespace toyc;

namespace {

// User needs operands; for Task 1 we test the lowest layer with a minimal User subclass
// declared inside ir.h (TestUser) — see implementation step. This test wires two Values
// into a User and checks the bidirectional use-list.
void test_use_list_wiring() {
    Module m;  // Module exists from Task 4 onward; for Task 1 use a local stand-in below.
}

}  // namespace
```

> Note: Task 1's test needs concrete `Value`/`User` instances. Since `Module` (the id/source factory) lands in Task 4, Task 1 introduces a tiny internal `TestUser` class in `ir.h` under `#ifdef TOYC_IR_TESTS` so the use-list logic is testable in isolation. Keep this guarded helper — later tasks retire it once `Module` factories exist.

Rewrite the test body concretely:

```cpp
void test_use_list_wiring() {
    // Two leaf Values and one User that consumes them.
    Value a(Type::I32, ValueKind::Register, /*id=*/0);
    Value b(Type::I32, ValueKind::Register, /*id=*/1);
    TestUser u;
    u.set_operand(0, &a);
    u.set_operand(1, &b);

    toyc::test::check(u.num_operands() == 2, "user has 2 operands");
    toyc::test::check(u.operand(0) == &a && u.operand(1) == &b, "operands stored");

    // Use-list: a and b each record u as a user.
    toyc::test::check(a.uses().size() == 1 && a.uses()[0] == &u, "a used by u");
    toyc::test::check(b.uses().size() == 1 && b.uses()[0] == &u, "b used by u");

    // Replace operand 0 with b: a's use-list loses u, b's gains a second entry.
    u.set_operand(0, &b);
    toyc::test::check(a.uses().empty(), "a no longer used");
    toyc::test::check(b.uses().size() == 2, "b used twice");

    // replace_all_uses_with: redirect every use of b to a.
    b.replace_all_uses_with(&a);
    toyc::test::check(u.operand(0) == &a && u.operand(1) == &a, "both operands now a");
    toyc::test::check(b.uses().empty(), "b fully replaced");
    toyc::test::check(a.uses().size() == 2, "a used twice after RAUW");
}
```

`main()` (top of file, appended-to each task):

```cpp
int main() {
    test_use_list_wiring();
    return toyc::test::report();
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build -j 2>&1 | tail -5
```
Expected: compile error — `ir.h`, `ir.cpp`, `Module`, `TestUser` do not exist.

- [ ] **Step 3: Implement the minimal core**

Create `include/toyc/ir.h`:

```cpp
#pragma once

#include <cstdint>
#include <list>
#include <memory>
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

    // Name per design §4.3/§4.5. Subclasses (ConstantInt/GlobalAddr/BasicBlock/Function)
    // override; the default covers Register/Param.
    virtual std::string name() const;

    const std::vector<User*>& uses() const { return uses_; }
    void add_use(User* user);
    void remove_use(User* user);
    void replace_all_uses_with(Value* other);

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

private:
    std::vector<Value*> operands_;
};

#ifdef TOYC_IR_TESTS
// Test-only helper: a User with a fixed operand count, for exercising use-lists
// before Module/instruction factories exist. Retired once real users exist.
class TestUser : public User {
public:
    TestUser() : User(Type::Void, ValueKind::Register, 0) {
        operands_.resize(2, nullptr);  // access via parent: expose below
    }
    void resize(unsigned n) { operands_.resize(n, nullptr); }
};
#endif

}  // namespace toyc
```

Wait — `TestUser` cannot touch `operands_` (private). Promote `operands_` access: add a `protected` accessor. Revise the `User` block: change `private:` to `protected:` for `operands_`:

```cpp
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
```

And simplify `TestUser`:

```cpp
#ifdef TOYC_IR_TESTS
class TestUser : public User {
public:
    TestUser() : User(Type::Void, ValueKind::Register, 0) { operands_.resize(2, nullptr); }
};
#endif
```

Create `src/ir/ir.cpp`:

```cpp
#include "toyc/ir.h"

#include <sstream>

namespace toyc {

const char* type_name(Type type) {
    switch (type) {
        case Type::I32:   return "i32";
        case Type::Ptr:   return "ptr";
        case Type::Void:  return "void";
        case Type::Label: return "label";
    }
    return "?";
}

std::string Value::name() const {
    // Register and Param share the numeric id; distinguished by prefix.
    std::ostringstream os;
    if (kind_ == ValueKind::Param) {
        os << "%arg." << id_;
    } else {
        os << "%v." << id_;
    }
    return os.str();
}

void Value::add_use(User* user) { uses_.push_back(user); }

void Value::remove_use(User* user) {
    for (auto it = uses_.begin(); it != uses_.end(); ++it) {
        if (*it == user) {
            uses_.erase(it);
            return;
        }
    }
}

void User::add_operand(Value* value) {
    operands_.push_back(value);
    value->add_use(this);
}

void User::set_operand(unsigned i, Value* value) {
    Value* old = operands_[i];
    if (old == value) {
        return;
    }
    old->remove_use(this);
    operands_[i] = value;
    value->add_use(this);
}

void Value::replace_all_uses_with(Value* other) {
    if (other == this) {
        return;
    }
    // Snapshot users: rewriting their operands mutates our uses_.
    std::vector<User*> users = uses_;
    for (User* user : users) {
        for (unsigned i = 0; i < user->num_operands(); ++i) {
            if (user->operand(i) == this) {
                user->set_operand(i, other);
            }
        }
    }
    uses_.clear();
}

}  // namespace toyc
```

Modify `CMakeLists.txt` — add ir.cpp to the executable (insert after `src/frontend/ast_access.cpp` line):

```cmake
    src/ir/ir.cpp
```

Append the test target after the existing `add_test(...)` block (before the final newline):

```cmake
add_executable(toyc-ir-tests
    test/ir/ir_tests.cpp
)
target_include_directories(toyc-ir-tests PRIVATE include test/ir)
target_compile_definitions(toyc-ir-tests PRIVATE TOYC_IR_TESTS)
if(NOT MSVC)
    target_compile_options(toyc-ir-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME ir_tests
    COMMAND toyc-ir-tests
)
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake -S . -B build && cmake --build build -j 2>&1 | tail -5
./build/toyc-ir-tests
```
Expected: `7/7 checks passed`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add include/toyc/ir.h src/ir/ir.cpp test/ir/check.h test/ir/ir_tests.cpp CMakeLists.txt
git commit -m "feat(ir): Value/User core with use-lists and RAUW"
```

---

## Task 2: `ConstantInt`, `GlobalAddr`, `GlobalVar`, and the `Module` constant pool

**Files:**
- Modify: `include/toyc/ir.h`, `src/ir/ir.cpp`
- Test: `test/ir/ir_tests.cpp` (append)

**Interfaces:**
- Produces: `class ConstantInt : public Value` (construct via `Module::get_constant(int)`, uniqued; `name()` returns the decimal literal; `int value()`); `class GlobalAddr : public Value` (ptr type, `name()` = `"@" + label`, `const std::string& label()`); `struct GlobalVar { GlobalAddr* addr; ConstantInt* init; bool is_const; }`. `Module` is forward-declared now but fully built in Task 4; Task 2 only adds the constant-pool + global-registry members/methods to it.

- [ ] **Step 1: Write the failing test**

Append to `test/ir/ir_tests.cpp` (and register in `main()`):

```cpp
void test_constant_pool_and_globals() {
    Module m;
    ConstantInt* c1 = m.get_constant(42);
    ConstantInt* c2 = m.get_constant(42);   // same value → uniqued
    ConstantInt* c3 = m.get_constant(7);

    toyc::test::check(c1 == c2, "constant 42 uniqued");
    toyc::test::check(c1 != c3, "constant 7 distinct");
    toyc::test::check(c1->value() == 42, "constant value 42");
    toyc::test::check_eq_str("42", c1->name(), "constant name is literal");

    GlobalVar* g = m.create_global("g_count", 0, /*is_const=*/false);
    toyc::test::check_eq_str("@g_count", g->addr->name(), "global addr name");
    toyc::test::check(g->addr->type() == Type::Ptr, "global addr is ptr");
    toyc::test::check(g->init->value() == 0, "global init value");
    toyc::test::check(!g->is_const, "global is var not const");
}
```

Add to `main()`: `test_constant_pool_and_globals();`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build -j 2>&1 | tail -5
```
Expected: compile error — `ConstantInt`/`GlobalAddr`/`GlobalVar`/`Module`/`get_constant`/`create_global` undefined.

- [ ] **Step 3: Implement**

Append to `include/toyc/ir.h` (before the closing `}  // namespace toyc`), and forward-declare `class Module;` near the top (after `class User;`):

```cpp
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

class Module;  // forward (full definition in Task 4)
```

In Task 4 `Module` will own these; for Task 2 add a **partial `Module`** with just the constant pool and global registry. To avoid two definitions, Task 2 introduces the full `Module` skeleton now (Tasks 3–4 extend it). Add to `ir.h`:

```cpp
class Function;  // forward (Task 4)

class Module {
public:
    Module() = default;

    // Constant pool (uniqued by value, Module-owned).
    ConstantInt* get_constant(int value);

    // Globals (Module-owned). init_value must be pre-folded (design §5.6/Q5).
    GlobalVar* create_global(const std::string& name, int init_value, bool is_const);

    unsigned fresh_id() { return value_counter_++; }

    // Functions: added in Task 4.
    const std::vector<std::unique_ptr<GlobalVar>>& globals() const { return globals_; }

private:
    std::unordered_map<int, std::unique_ptr<ConstantInt>> constants_;
    std::vector<std::unique_ptr<GlobalVar>> globals_;
    unsigned value_counter_ = 0;
};
```

> Note `Module` needs `<memory>`, `<unordered_map>`, `<vector>` — already included at top of `ir.h`. `GlobalVar` stores raw owning pointers inside a `unique_ptr<GlobalVar>` owned by Module: each `GlobalVar`'s `addr`/`init` are themselves Module-owned (constant pool / a `unique_ptr<GlobalAddr>`). Add `std::vector<std::unique_ptr<GlobalAddr>> global_addrs_;` to `Module` and append to it in `create_global`.

Revise `Module` to own the `GlobalAddr`s too. Final Task-2 `Module` additions:

```cpp
class Module {
public:
    Module() = default;

    ConstantInt* get_constant(int value);
    GlobalVar* create_global(const std::string& name, int init_value, bool is_const);

    unsigned fresh_id() { return value_counter_++; }

    const std::vector<std::unique_ptr<GlobalVar>>& globals() const { return globals_; }

private:
    std::unordered_map<int, std::unique_ptr<ConstantInt>> constants_;
    std::vector<std::unique_ptr<GlobalAddr>> global_addrs_;
    std::vector<std::unique_ptr<GlobalVar>> globals_;
    unsigned value_counter_ = 0;
};
```

Append to `src/ir/ir.cpp`:

```cpp
ConstantInt* Module::get_constant(int value) {
    auto it = constants_.find(value);
    if (it != constants_.end()) {
        return it->second.get();
    }
    auto owned = std::make_unique<ConstantInt>(value);
    ConstantInt* raw = owned.get();
    constants_.emplace(value, std::move(owned));
    return raw;
}

GlobalVar* Module::create_global(const std::string& name, int init_value, bool is_const) {
    auto addr = std::make_unique<GlobalAddr>(name);
    GlobalAddr* addr_raw = addr.get();
    global_addrs_.push_back(std::move(addr));

    auto gv = std::make_unique<GlobalVar>();
    gv->addr = addr_raw;
    gv->init = get_constant(init_value);
    gv->is_const = is_const;
    GlobalVar* gv_raw = gv.get();
    globals_.push_back(std::move(gv));
    return gv_raw;
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build build -j && ./build/toyc-ir-tests
```
Expected: `15/15 checks passed` (7 prior + 8 new), exit 0.

- [ ] **Step 5: Commit**

```bash
git add include/toyc/ir.h src/ir/ir.cpp test/ir/ir_tests.cpp
git commit -m "feat(ir): ConstantInt pool, GlobalAddr, GlobalVar"
```

---

## Task 3: `Opcode` + `Instruction` hierarchy (full §7 instruction set)

**Files:**
- Modify: `include/toyc/ir.h`, `src/ir/ir.cpp`
- Test: `test/ir/ir_tests.cpp` (append)

**Interfaces:**
- Produces: `enum class Opcode { Add, Sub, Mul, Sdiv, Srem, Neg, ICmpEq, ICmpNe, ICmpSlt, ICmpSgt, ICmpSle, ICmpSge, Alloca, Load, Store, Br, CondBr, Ret, Call, Phi, Shl, Shr }`; `const char* opcode_name(Opcode)`; `class Instruction : public User` (`opcode()`, `parent()`, `bool is_terminator()`, `bool has_result()`); subclasses `BinaryInst`, `ICmpInst`, `NegInst`, `AllocaInst`, `LoadInst`, `StoreInst`, `BrInst`, `CondBrInst`, `RetInst`, `CallInst`, `PhiInst` (with `add_incoming(Value*, BasicBlock*)`), `ShlInst`, `ShrInst`.
- Consumes: `Value`/`User` (Task 1), `ConstantInt` (Task 2), `Module::fresh_id()` (Task 2) for result naming.

- [ ] **Step 1: Write the failing test**

Append:

```cpp
void test_instruction_construction() {
    Module m;
    Value* a = m.create_register(Type::I32);
    Value* b = m.create_register(Type::I32);

    auto add = std::make_unique<BinaryInst>(Opcode::Add, a, b, m.fresh_id());
    toyc::test::check(add->opcode() == Opcode::Add, "add opcode");
    toyc::test::check(add->type() == Type::I32, "add result i32");
    toyc::test::check(add->has_result(), "add has result");
    toyc::test::check(add->operand(0) == a && add->operand(1) == b, "add operands");
    toyc::test::check(add->num_operands() == 2, "add 2 operands");
    toyc::test::check(!add->is_terminator(), "add not terminator");
    toyc::test::check(a->uses().size() == 1, "a used by add");

    auto slt = std::make_unique<ICmpInst>(Opcode::ICmpSlt, a, b, m.fresh_id());
    toyc::test::check(slt->opcode() == Opcode::ICmpSlt && slt->type() == Type::I32, "icmp i32 result");

    ConstantInt* one = m.get_constant(1);
    auto ret = std::make_unique<RetInst>(one);
    toyc::test::check(ret->is_terminator(), "ret terminator");
    toyc::test::check(!ret->has_result(), "ret has no result");
    toyc::test::check(ret->operand(0) == one, "ret operand");

    auto ret_void = std::make_unique<RetInst>(/*value=*/nullptr);
    toyc::test::check(ret_void->num_operands() == 0, "void ret 0 operands");
}
```

Add `m.create_register(Type)` requires a Module method — add it in this task (it only calls `fresh_id()`). Register in `main()`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build -j 2>&1 | tail -5
```
Expected: compile error — `Opcode`, `Instruction`, subclasses, `create_register` undefined.

- [ ] **Step 3: Implement**

Add to `include/toyc/ir.h` (forward-declare `class BasicBlock;` near other forwards):

```cpp
enum class Opcode {
    Add, Sub, Mul, Sdiv, Srem, Neg,
    ICmpEq, ICmpNe, ICmpSlt, ICmpSgt, ICmpSle, ICmpSge,
    Alloca, Load, Store,
    Br, CondBr, Ret, Call,
    Phi,
    Shl, Shr,  // introduced only by optimization (design §7/§8.6)
};
const char* opcode_name(Opcode opcode);

class BasicBlock;  // forward

class Instruction : public User {
public:
    Instruction(Opcode opcode, Type type, unsigned id)
        : User(type, ValueKind::Register, id), opcode_(opcode) {}

    Opcode opcode() const { return opcode_; }
    BasicBlock* parent() const { return parent_; }
    void set_parent(BasicBlock* bb) { parent_ = bb; }

    bool is_terminator() const {
        return opcode_ == Opcode::Br || opcode_ == Opcode::CondBr || opcode_ == Opcode::Ret;
    }
    bool has_result() const {
        return !(opcode_ == Opcode::Store || opcode_ == Opcode::Br ||
                 opcode_ == Opcode::CondBr || opcode_ == Opcode::Ret ||
                 opcode_ == Opcode::Call /* void call determined per-instance */);
    }

private:
    Opcode opcode_;
    BasicBlock* parent_ = nullptr;
};
```

> `CallInst` overrides `has_result()` (int call → true, void call → false). So `Instruction::has_result()` above should NOT hardcode Call. Revise: remove `Opcode::Call` from the base `has_result()`; `CallInst` overrides.

Revised base:

```cpp
    bool has_result() const {
        return !(opcode_ == Opcode::Store || opcode_ == Opcode::Br ||
                 opcode_ == Opcode::CondBr || opcode_ == Opcode::Ret);
    }
```

Subclasses (append):

```cpp
class BinaryInst : public Instruction {
public:
    BinaryInst(Opcode opcode, Value* lhs, Value* rhs, unsigned id)
        : Instruction(opcode, Type::I32, id) {
        add_operand(lhs);
        add_operand(rhs);
    }
};

class ICmpInst : public Instruction {
public:
    ICmpInst(Opcode opcode, Value* lhs, Value* rhs, unsigned id)
        : Instruction(opcode, Type::I32, id) {
        add_operand(lhs);
        add_operand(rhs);
    }
};

class NegInst : public Instruction {
public:
    NegInst(Value* operand, unsigned id) : Instruction(Opcode::Neg, Type::I32, id) {
        add_operand(operand);
    }
};

class AllocaInst : public Instruction {
public:
    explicit AllocaInst(unsigned id) : Instruction(Opcode::Alloca, Type::Ptr, id) {}
};

class LoadInst : public Instruction {
public:
    LoadInst(Value* ptr, unsigned id) : Instruction(Opcode::Load, Type::I32, id) {
        add_operand(ptr);
    }
};

class StoreInst : public Instruction {
public:
    // store <ptr>, <val>  (pointer first — design §4.5 decision)
    StoreInst(Value* ptr, Value* val) : Instruction(Opcode::Store, Type::Void, 0) {
        add_operand(ptr);
        add_operand(val);
    }
};

class BrInst : public Instruction {
public:
    explicit BrInst(BasicBlock* target);
};

class CondBrInst : public Instruction {
public:
    CondBrInst(Value* cond, BasicBlock* t, BasicBlock* f);
};

class RetInst : public Instruction {
public:
    explicit RetInst(Value* value = nullptr) : Instruction(Opcode::Ret, Type::Void, 0) {
        if (value) {
            add_operand(value);
        }
    }
};

class CallInst : public Instruction {
public:
    // returns_void selects has_result(). callee_name is the @f symbol (not a Value operand).
    CallInst(std::string callee_name, std::vector<Value*> args, bool returns_void, unsigned id)
        : Instruction(Opcode::Call, returns_void ? Type::Void : Type::I32, id),
          callee_name_(std::move(callee_name)), returns_void_(returns_void) {
        for (Value* a : args) {
            add_operand(a);
        }
    }
    bool has_result() const override { return !returns_void_; }
    const std::string& callee_name() const { return callee_name_; }

private:
    std::string callee_name_;
    bool returns_void_;
};

class PhiInst : public Instruction {
public:
    explicit PhiInst(unsigned id) : Instruction(Opcode::Phi, Type::I32, id) {}
    // incoming pairs kept in parallel vectors (operand + predecessor block).
    void add_incoming(Value* value, BasicBlock* block);
    const std::vector<BasicBlock*>& incoming_blocks() const { return incoming_blocks_; }

private:
    std::vector<BasicBlock*> incoming_blocks_;
};

class ShlInst : public Instruction {
public:
    ShlInst(Value* value, unsigned amount, unsigned id)
        : Instruction(Opcode::Shl, Type::I32, id), amount_(amount) {
        add_operand(value);
    }
    unsigned amount() const { return amount_; }
private:
    unsigned amount_;
};

class ShrInst : public Instruction {
public:
    ShrInst(Value* value, unsigned amount, unsigned id)
        : Instruction(Opcode::Shr, Type::I32, id), amount_(amount) {
        add_operand(value);
    }
    unsigned amount() const { return amount_; }
private:
    unsigned amount_;
};
```

`BrInst`/`CondBrInst` reference `BasicBlock` (label operand). Since `BasicBlock` is a `Value`, store as operands but block objects aren't constructed yet (Task 4). Their constructors are defined **out-of-line in `ir.cpp`** (after `BasicBlock` is complete) — declared in the header now, implemented in Task 4's `ir.cpp` additions. For Task 3's test (which doesn't use Br/CondBr yet) declare them but defer bodies to Task 4. Mark with a comment.

Add `Module::create_register` to `Module` (header, public):

```cpp
    Value* create_register(Type type) {
        auto owned = std::make_unique<Value>(type, ValueKind::Register, fresh_id());
        Value* raw = owned.get();
        registers_.push_back(std::move(owned));  // Module owns standalone registers
        return raw;
    }
```

> But `Value`'s constructor is public and `name()` works for Register. Standalone registers (not yet attached to an instruction) are owned by `Module::registers_`. Add member `std::vector<std::unique_ptr<Value>> registers_;` to `Module`. In practice IRGen/Builder create registers as instruction results; the standalone path exists for tests/fixtures. Add the member.

Add to `Module` private: `std::vector<std::unique_ptr<Value>> registers_;`.

Append to `src/ir/ir.cpp`:

```cpp
const char* opcode_name(Opcode opcode) {
    switch (opcode) {
        case Opcode::Add: return "add";
        case Opcode::Sub: return "sub";
        case Opcode::Mul: return "mul";
        case Opcode::Sdiv: return "sdiv";
        case Opcode::Srem: return "srem";
        case Opcode::Neg: return "neg";
        case Opcode::ICmpEq: return "icmp eq";
        case Opcode::ICmpNe: return "icmp ne";
        case Opcode::ICmpSlt: return "icmp slt";
        case Opcode::ICmpSgt: return "icmp sgt";
        case Opcode::ICmpSle: return "icmp sle";
        case Opcode::ICmpSge: return "icmp sge";
        case Opcode::Alloca: return "alloca";
        case Opcode::Load: return "load";
        case Opcode::Store: return "store";
        case Opcode::Br: return "br";
        case Opcode::CondBr: return "cond_br";
        case Opcode::Ret: return "ret";
        case Opcode::Call: return "call";
        case Opcode::Phi: return "phi";
        case Opcode::Shl: return "shl";
        case Opcode::Shr: return "shr";
    }
    return "?";
}
```

`PhiInst::add_incoming` (in `ir.cpp`):

```cpp
void PhiInst::add_incoming(Value* value, BasicBlock* block) {
    add_operand(value);
    incoming_blocks_.push_back(block);
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build build -j && ./build/toyc-ir-tests
```
Expected: `24/24 checks passed`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add include/toyc/ir.h src/ir/ir.cpp test/ir/ir_tests.cpp
git commit -m "feat(ir): Instruction hierarchy with full §7 opcode set"
```

---

## Task 4: `BasicBlock`, `Function`, `Module` ownership + numbering

**Files:**
- Modify: `include/toyc/ir.h`, `src/ir/ir.cpp`
- Test: `test/ir/ir_tests.cpp` (append)

**Interfaces:**
- Produces: `class BasicBlock : public Value` (`name()` = `"entry"` if it is the function's first block else `"bb" + id`; `push_back`/`push_front` instruction; `Instruction* terminator()`; `std::list<unique_ptr<Instruction>>& insts()`; `Function* parent()`; predecessor/successor accessors `preds()`/`succs()` — populated lazily by `recompute_cfg()`, added now but heavily used by mem2reg plan); `class Function : public Value` (`name()` = `"@" + name`, `create_block()`, `entry()`, `BasicBlock list`, `params()`, `ret_type()`); full `Module` with `create_function(name, ret_type, param_count)`, function list, `Value* param(Function*, unsigned)`.
- Consumes: `Instruction` (Task 3), `GlobalVar`/constant pool (Task 2).

- [ ] **Step 1: Write the failing test**

Append:

```cpp
void test_function_and_blocks() {
    Module m;
    Function* f = m.create_function("add", FuncReturnType::Int, /*params=*/2);

    toyc::test::check_eq_str("@add", f->name(), "function name");
    toyc::test::check(f->ret_type() == FuncReturnType::Int, "function ret int");
    toyc::test::check(f->params().size() == 2, "2 params");
    toyc::test::check_eq_str("%arg.0", f->params()[0]->name(), "param 0 name");
    toyc::test::check_eq_str("%arg.1", f->params()[1]->name(), "param 1 name");

    BasicBlock* entry = f->create_block();  // first block → "entry"
    BasicBlock* bb1 = f->create_block();    // → "bb1"
    toyc::test::check_eq_str("entry", entry->name(), "entry label");
    toyc::test::check_eq_str("bb1", bb1->name(), "bb1 label");
    toyc::test::check(f->entry() == entry, "entry is first block");

    Value* a = f->params()[0];
    Value* one = m.get_constant(1);
    auto add = std::make_unique<BinaryInst>(Opcode::Add, a, one, m.fresh_id());
    auto ret = std::make_unique<RetInst>(nullptr);
    Instruction* add_raw = add.get();
    entry->push_back(std::move(add));
    entry->push_back(std::move(ret));
    toyc::test::check(add_raw->parent() == entry, "inst parent set");
    toyc::test::check(entry->terminator()->opcode() == Opcode::Ret, "terminator is ret");
    toyc::test::check_eq_str("%v.0", add_raw->name(), "add result name %v.0");
}
```

Need `FuncReturnType` — it already lives in `ast.h`. `ir.h` must `#include "toyc/ast.h"` OR re-declare. To avoid pulling all of AST into IR, define IR-local `enum class FuncRet { Int, Void }`. **Decision:** use IR-local enum `FuncRet` in `ir.h` (no AST dependency). Update test to use `FuncRet::Int`. Register in `main()`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build -j 2>&1 | tail -5
```
Expected: compile error — `Function`/`BasicBlock`/`create_function`/`create_block` undefined.

- [ ] **Step 3: Implement**

Add to `include/toyc/ir.h` (near other enums):

```cpp
enum class FuncRet { Int, Void };
```

Add `BasicBlock` and `Function` (append before `Module`, since `Module` references them):

```cpp
class BasicBlock : public Value {
public:
    explicit BasicBlock(unsigned id, Function* parent)
        : Value(Type::Label, ValueKind::BasicBlock, id), parent_(parent) {}

    std::string name() const override;  // "entry" if id_==0 else "bb"+id_
    Function* parent() const { return parent_; }

    void push_back(std::unique_ptr<Instruction> inst);
    void push_front(std::unique_ptr<Instruction> inst);
    std::list<std::unique_ptr<Instruction>>& insts() { return insts_; }
    const std::list<std::unique_ptr<Instruction>>& insts() const { return insts_; }

    Instruction* terminator() const { return insts_.empty() ? nullptr : insts_.back().get(); }

    const std::vector<BasicBlock*>& preds() const { return preds_; }
    const std::vector<BasicBlock*>& succs() const { return succs_; }
    void add_pred(BasicBlock* b) { preds_.push_back(b); }

private:
    Function* parent_;
    std::list<std::unique_ptr<Instruction>> insts_;
    std::vector<BasicBlock*> preds_;
    std::vector<BasicBlock*> succs_;
    friend class Function;
};

class Function : public Value {
public:
    Function(std::string name, FuncRet ret_type, unsigned param_count, Module* module)
        : Value(Type::Void, ValueKind::Function, 0),
          name_(std::move(name)), ret_type_(ret_type), module_(module) {
        for (unsigned i = 0; i < param_count; ++i) {
            auto p = std::make_unique<Value>(Type::I32, ValueKind::Param, i);
            params_.push_back(std::move(p));
        }
    }

    std::string name() const override { return "@" + name_; }
    const std::string& short_name() const { return name_; }
    FuncRet ret_type() const { return ret_type_; }
    Module* module() const { return module_; }

    const std::vector<std::unique_ptr<Value>>& params() const { return params_; }
    Value* param(unsigned i) const { return params_[i].get(); }

    BasicBlock* create_block();  // assigns block id; first created is entry
    BasicBlock* entry() const { return blocks_.empty() ? nullptr : blocks_.front().get(); }
    std::list<std::unique_ptr<BasicBlock>>& blocks() { return blocks_; }
    const std::list<std::unique_ptr<BasicBlock>>& blocks() const { return blocks_; }

private:
    std::string name_;
    FuncRet ret_type_;
    Module* module_;
    std::vector<std::unique_ptr<Value>> params_;
    std::list<std::unique_ptr<BasicBlock>> blocks_;
    unsigned block_counter_ = 0;
};
```

Extend `Module` (add function ownership + `create_function`):

```cpp
class Module {
public:
    // ... existing members (Task 2/3) ...

    Function* create_function(const std::string& name, FuncRet ret_type, unsigned param_count) {
        auto f = std::make_unique<Function>(name, ret_type, param_count, this);
        Function* raw = f.get();
        functions_.push_back(std::move(f));
        return raw;
    }
    const std::vector<std::unique_ptr<Function>>& functions() const { return functions_; }

private:
    // ... existing ...
    std::vector<std::unique_ptr<Function>> functions_;
};
```

Append to `src/ir/ir.cpp`:

```cpp
std::string BasicBlock::name() const {
    return id() == 0 ? "entry" : ("bb" + std::to_string(id()));
}

void BasicBlock::push_back(std::unique_ptr<Instruction> inst) {
    inst->set_parent(this);
    insts_.push_back(std::move(inst));
}

void BasicBlock::push_front(std::unique_ptr<Instruction> inst) {
    inst->set_parent(this);
    insts_.push_front(std::move(inst));
}

BasicBlock* Function::create_block() {
    unsigned id = block_counter_++;
    auto bb = std::make_unique<BasicBlock>(id, this);
    BasicBlock* raw = bb.get();
    blocks_.push_back(std::move(bb));
    return raw;
}
```

Now define the deferred `BrInst`/`CondBrInst` constructors (referenced `BasicBlock*` as operands; a block is a `Value` so `add_operand` works, and this also records the edge — but `succs_` is a separate vector; keep CFG explicit via a later `recompute_cfg`, and for now the terminator operands hold the targets). Bodies in `ir.cpp`:

```cpp
BrInst::BrInst(BasicBlock* target) : Instruction(Opcode::Br, Type::Void, 0) {
    add_operand(target);
}
CondBrInst::CondBrInst(Value* cond, BasicBlock* t, BasicBlock* f)
    : Instruction(Opcode::CondBr, Type::Void, 0) {
    add_operand(cond);
    add_operand(t);
    add_operand(f);
}
```

> Edge bookkeeping (`preds_`/`succs_`) is rebuilt wholesale by `recompute_cfg()` in the mem2reg plan — not maintained incrementally here, to avoid stale-edge bugs. Document this in a comment on `preds()`/`succs()`.

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build build -j && ./build/toyc-ir-tests
```
Expected: `35/35 checks passed`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add include/toyc/ir.h src/ir/ir.cpp test/ir/ir_tests.cpp
git commit -m "feat(ir): BasicBlock, Function, Module ownership and numbering"
```

---

## Task 5: IR printer (`-dump-ir` format, design §4.5)

**Files:**
- Create: `include/toyc/ir_printer.h`, `src/ir/ir_printer.cpp`
- Modify: `CMakeLists.txt` (add `src/ir/ir_printer.cpp` to both targets), `test/ir/ir_tests.cpp` (append)

**Interfaces:**
- Produces: `void print_module(const Module& module, std::ostream& out)`; `void print_function(const Function& fn, std::ostream& out)`.

- [ ] **Step 1: Write the failing test**

Append:

```cpp
#include "toyc/ir_printer.h"
#include <sstream>

void test_printer_basic() {
    Module m;
    m.create_global("g_count", 0, /*is_const=*/false);
    Function* f = m.create_function("f", FuncRet::Int, /*params=*/1);
    BasicBlock* entry = f->create_block();
    Value* a = f->params()[0];
    Value* slot = entry->push_back_register<AllocaInst>(m);  // builder-free helper, see impl
    // NOTE: prefer IRBuilder in Task 6; here use direct construction for a deterministic print.
    // Rewind: construct directly:
    // (Replace the above placeholder line with explicit construction below.)
}
```

> The placeholder above is a plan smell — replace with fully explicit construction. Concrete test:

```cpp
void test_printer_basic() {
    Module m;
    m.create_global("g_count", 0, /*is_const=*/false);
    Function* f = m.create_function("f", FuncRet::Int, /*params=*/1);
    BasicBlock* entry = f->create_block();
    Value* a = f->param(0);

    auto alloca = std::make_unique<AllocaInst>(m.fresh_id());          // %v.0 = alloca i32
    Value* slot = alloca.get();
    auto store = std::make_unique<StoreInst>(slot, a);                  // store %v.0, %arg.0
    auto load = std::make_unique<LoadInst>(slot, m.fresh_id());         // %v.1 = load %v.0
    Value* loaded = load.get();
    ConstantInt* one = m.get_constant(1);
    auto add = std::make_unique<BinaryInst>(Opcode::Add, loaded, one, m.fresh_id()); // %v.2 = add %v.1, 1
    Value* addv = add.get();
    auto ret = std::make_unique<RetInst>(addv);                         // ret %v.2
    entry->push_back(std::move(alloca));
    entry->push_back(std::move(store));
    entry->push_back(std::move(load));
    entry->push_back(std::move(add));
    entry->push_back(std::move(ret));

    std::ostringstream out;
    print_module(m, out);

    std::string expected =
        "@g_count = global i32 0\n"
        "\n"
        "define i32 @f(i32 %arg.0) {\n"
        "entry:\n"
        "  %v.0 = alloca i32\n"
        "  store %v.0, %arg.0\n"
        "  %v.1 = load %v.0\n"
        "  %v.2 = add %v.1, 1\n"
        "  ret %v.2\n"
        "}\n";
    toyc::test::check_eq_str(expected, out.str(), "module print");
}
```

Register in `main()`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build -j 2>&1 | tail -5
```
Expected: compile error — `ir_printer.h`/`print_module` undefined.

- [ ] **Step 3: Implement**

Create `include/toyc/ir_printer.h`:

```cpp
#pragma once

#include <iosfwd>

namespace toyc {

class Module;
class Function;

void print_module(const Module& module, std::ostream& out);
void print_function(const Function& function, std::ostream& out);

}  // namespace toyc
```

Create `src/ir/ir_printer.cpp`:

```cpp
#include "toyc/ir_printer.h"
#include "toyc/ir.h"

#include <ostream>

namespace toyc {

namespace {

// Format the result-prefix of an instruction: "%v.N = " if it has a result, else "".
std::string result_prefix(const Instruction& inst) {
    return inst.has_result() ? inst.name() + " = " : "";
}

void print_operand(Value* v, std::ostream& out) {
    // Operands render by their own name(): registers %v.N, params %arg.N, constants as literals,
    // globals @name, blocks as their label (entry/bbN).
    if (v->value_kind() == ValueKind::BasicBlock) {
        out << "label " << v->name();
    } else {
        out << v->name();
    }
}

void print_instruction(const Instruction& inst, std::ostream& out) {
    out << "  " << result_prefix(inst);
    switch (inst.opcode()) {
        case Opcode::Alloca:
            out << "alloca i32";
            break;
        case Opcode::Load:
            out << "load ";
            print_operand(inst.operand(0), out);
            break;
        case Opcode::Store:
            out << "store ";
            print_operand(inst.operand(0), out);
            out << ", ";
            print_operand(inst.operand(1), out);
            break;
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
        case Opcode::Sdiv: case Opcode::Srem:
            out << opcode_name(inst.opcode()) << " ";
            print_operand(inst.operand(0), out);
            out << ", ";
            print_operand(inst.operand(1), out);
            break;
        case Opcode::Neg:
            out << "neg ";
            print_operand(inst.operand(0), out);
            break;
        case Opcode::ICmpEq: case Opcode::ICmpNe: case Opcode::ICmpSlt:
        case Opcode::ICmpSgt: case Opcode::ICmpSle: case Opcode::ICmpSge:
            out << opcode_name(inst.opcode()) << " ";
            print_operand(inst.operand(0), out);
            out << ", ";
            print_operand(inst.operand(1), out);
            break;
        case Opcode::Br:
            out << "br ";
            print_operand(inst.operand(0), out);
            break;
        case Opcode::CondBr:
            out << "cond_br ";
            print_operand(inst.operand(0), out);
            out << ", ";
            print_operand(inst.operand(1), out);
            out << ", ";
            print_operand(inst.operand(2), out);
            break;
        case Opcode::Ret:
            out << "ret";
            if (inst.num_operands() == 1) {
                out << " ";
                print_operand(inst.operand(0), out);
            }
            break;
        case Opcode::Call: {
            const CallInst& c = static_cast<const CallInst&>(inst);
            out << "call @" << c.callee_name();
            for (unsigned i = 0; i < c.num_operands(); ++i) {
                out << ", ";
                print_operand(c.operand(i), out);
            }
            break;
        }
        case Opcode::Phi: {
            const PhiInst& p = static_cast<const PhiInst&>(inst);
            out << "phi ";
            for (unsigned i = 0; i < p.num_operands(); ++i) {
                if (i) {
                    out << ", ";
                }
                out << "[";
                print_operand(p.operand(i), out);
                out << ", " << p.incoming_blocks()[i]->name() << "]";
            }
            break;
        }
        case Opcode::Shl: case Opcode::Shr: {
            const Instruction& base = inst;  // both Shl/Shr carry amount()
            unsigned amount = (inst.opcode() == Opcode::Shl)
                                  ? static_cast<const ShlInst&>(base).amount()
                                  : static_cast<const ShrInst&>(base).amount();
            out << opcode_name(inst.opcode()) << " ";
            print_operand(inst.operand(0), out);
            out << ", " << amount;
            break;
        }
    }
    out << "\n";
}

}  // namespace

void print_function(const Function& fn, std::ostream& out) {
    out << "define " << (fn.ret_type() == FuncRet::Int ? "i32" : "void") << " " << fn.name() << "(";
    for (unsigned i = 0; i < fn.params().size(); ++i) {
        if (i) {
            out << ", ";
        }
        out << "i32 " << fn.params()[i]->name();
    }
    out << ") {\n";
    for (const std::unique_ptr<BasicBlock>& bb : fn.blocks()) {
        out << bb->name() << ":\n";
        for (const std::unique_ptr<Instruction>& inst : bb->insts()) {
            print_instruction(*inst, out);
        }
    }
    out << "}\n";
}

void print_module(const Module& module, std::ostream& out) {
    for (const std::unique_ptr<GlobalVar>& g : module.globals()) {
        out << g->addr->name() << " = " << (g->is_const ? "const" : "global") << " i32 "
            << g->init->value() << "\n";
    }
    if (!module.globals().empty()) {
        out << "\n";
    }
    for (const std::unique_ptr<Function>& fn : module.functions()) {
        print_function(*fn, out);
    }
}

}  // namespace toyc
```

Modify `CMakeLists.txt`: add `src/ir/ir_printer.cpp` to the `toyc-compiler` sources and to the `toyc-ir-tests` sources.

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build build -j && ./build/toyc-ir-tests
```
Expected: `36/36 checks passed`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add include/toyc/ir_printer.h src/ir/ir_printer.cpp CMakeLists.txt test/ir/ir_tests.cpp
git commit -m "feat(ir): -dump-ir printer per design §4.5"
```

---

## Task 6: `IRBuilder` + fixture helpers; retire `TestUser`

**Files:**
- Create: `include/toyc/ir_builder.h`, `src/ir/ir_builder.cpp`
- Modify: `CMakeLists.txt` (add `ir_builder.cpp`), `include/toyc/ir.h` (drop `#ifdef TOYC_IR_TESTS` `TestUser` once nothing uses it), `test/ir/ir_tests.cpp` (append; remove `TOYC_IR_TESTS` usage)

**Interfaces:**
- Produces: `class IRBuilder` with `set_insert_point(BasicBlock*)`, `set_function(Function*)`, and `create_*` methods that allocate result ids from the function's `Module` and append to the current insert block: `create_alloca`, `create_load`, `create_store`, `create_binary`, `create_icmp`, `create_neg`, `create_br`, `create_cond_br`, `create_ret`, `create_call`, `create_phi`, `create_shl`, `create_shr`. Each returns the raw `Instruction*` (or its result `Value*` where convenient).

- [ ] **Step 1: Write the failing test**

Append (build the same function as Task 5, but via the builder, and assert identical printed output):

```cpp
#include "toyc/ir_builder.h"

void test_builder_matches_printer() {
    Module m;
    Function* f = m.create_function("f", FuncRet::Int, 1);
    BasicBlock* entry = f->create_block();
    IRBuilder b(m, entry);

    Value* slot = b.create_alloca();                          // %v.0 = alloca i32
    b.create_store(slot, f->param(0));                        // store %v.0, %arg.0
    Value* loaded = b.create_load(slot);                      // %v.1 = load %v.0
    Value* sum = b.create_binary(Opcode::Add, loaded, m.get_constant(1)); // %v.2 = add %v.1, 1
    b.create_ret(sum);                                        // ret %v.2

    std::ostringstream out;
    print_module(m, out);

    std::string expected =
        "define i32 @f(i32 %arg.0) {\n"
        "entry:\n"
        "  %v.0 = alloca i32\n"
        "  store %v.0, %arg.0\n"
        "  %v.1 = load %v.0\n"
        "  %v.2 = add %v.1, 1\n"
        "  ret %v.2\n"
        "}\n";
    toyc::test::check_eq_str(expected, out.str(), "builder output matches direct construction");
}
```

Register in `main()`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build -j 2>&1 | tail -5
```
Expected: compile error — `ir_builder.h`/`IRBuilder` undefined.

- [ ] **Step 3: Implement**

Create `include/toyc/ir_builder.h`:

```cpp
#pragma once

#include "toyc/ir.h"

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
```

Create `src/ir/ir_builder.cpp`:

```cpp
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
```

Add `#include <type_traits>` to `ir_builder.h` (for `std::is_base_of_v`). Add `src/ir/ir_builder.cpp` to both CMake targets.

Retire `TestUser`: delete the `#ifdef TOYC_IR_TESTS` block in `ir.h` and the `TOYC_IR_TESTS` define in CMake `toyc-ir-tests` target (Task 1's use-list test now has no `TestUser` usage — but it DID use `TestUser`; re-home that test onto a real `User` subclass instead). Rewrite `test_use_list_wiring` to use a `StoreInst` (a real 2-operand user) + two registers:

```cpp
void test_use_list_wiring() {
    Module m;
    Value* a = m.create_register(Type::I32);
    Value* b = m.create_register(Type::I32);
    auto u = std::make_unique<StoreInst>(a, b);  // 2 operands: a(ptr), b(val)
    User* user = u.get();

    toyc::test::check(user->num_operands() == 2, "user has 2 operands");
    toyc::test::check(user->operand(0) == a && user->operand(1) == b, "operands stored");
    toyc::test::check(a->uses().size() == 1, "a used");
    toyc::test::check(b->uses().size() == 1, "b used");

    user->set_operand(0, b);
    toyc::test::check(a->uses().empty(), "a no longer used");
    toyc::test::check(b->uses().size() == 2, "b used twice");

    b->replace_all_uses_with(a);
    toyc::test::check(user->operand(0) == a && user->operand(1) == a, "RAUW rewires");
    toyc::test::check(b->uses().empty(), "b replaced out");
}
```

Remove the `TOYC_IR_TESTS` compile definition from CMake and the `TestUser` block from `ir.h`.

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build build -j && ./build/toyc-ir-tests
```
Expected: all checks pass (count unchanged or +1), exit 0. Also run the full suite:
```bash
ctest --test-dir build --output-on-failure
```
Expected: both `ir_tests` and `parser_regression` pass.

- [ ] **Step 5: Commit**

```bash
git add include/toyc/ir_builder.h src/ir/ir_builder.cpp include/toyc/ir.h test/ir/ir_tests.cpp CMakeLists.txt
git commit -m "feat(ir): IRBuilder; retire TestUser in favor of real users"
```

---

## Self-Review (run after writing, before execution)

1. **Spec coverage:** §4.1 Value/User hierarchy → Task 1. §4.2 containers (Module/Function/BasicBlock) → Tasks 2,4. §4.3 numbering → Tasks 1,4. §4.4 types → Task 1. §4.5 text format → Task 5. §7 instruction set → Task 3 (incl. opt-only `shl`/`shr`). P2 construction helper → Task 6. `store` operand order (ptr first) → Task 3 `StoreInst` + Task 5 printer. `!e`/`icmp eq 0` and `sdiv/srem` are IRGen concerns (later plan), not foundation. ✓
2. **Placeholder scan:** Task 5 Step 1 originally contained a placeholder line (`push_back_register`) — replaced with explicit construction. No remaining TBD/TODO. The single deferred bodies (`BrInst`/`CondBrInst` ctors declared Task 3, defined Task 4) are cross-task-annotated, not placeholders.
3. **Type consistency:** `Opcode` enumerators used consistently in Task 3 defs and Task 5 printer switch. `ValueKind`, `Type`, `FuncRet` consistent across tasks. `Module::fresh_id()` / `create_register` introduced in Task 2/3 and reused unchanged in Task 6 builder. `PhiInst::incoming_blocks()` defined Task 3, consumed Task 5. `BasicBlock::name()` rule ("entry" iff id 0) consistent Task 4 def ↔ Task 5 expected output.

## Out of scope for this plan (handled by later plans)

- **mem2reg** (design §6): CFG/preds/succs rebuild, Cooper-Harvey-Kennedy dominance, dominance frontiers, phi insertion, renaming. → Plan 2.
- **Optimization passes** (design §8): PassManager, ConstProp, DCE, CSE/GVN, CFS, AlgebraicSimplify. → Plan 3.
- **Driver wiring** (P5): `-dump-ir` flag + PassManager hookup. Blocked on IRGen (needs Sema). → Plan 3 tail / when Sema lands.
- **IRGen** (AST→non-SSA IR): blocked on Sema; contract frozen, implementation deferred.

---

## 执行状态（2026-06-22 更新）

**状态：✅ Plan 1 全部完成。** 6 个任务全部落地、测试通过、逐任务提交。

### 交付清单

| Task | Commit | 交付物 |
|------|--------|--------|
| 1 | `ce97d7f` | `Value`/`User` 核心 + use-list 双向链 + RAUW；`Type`、`ValueKind`、`check.h` 测试骨架、`toyc-ir-tests` CMake target |
| 2 | `108c297` | `ConstantInt`（uniqued 池）、`GlobalAddr`、`GlobalVar`、`Module::create_register` |
| 3 | `40a4e95` | `Opcode` 全集 + `Instruction` 层次（§7：Binary/ICmp/Neg/Alloca/Load/Store/Br/CondBr/Ret/Call/Phi/Shl/Shr）+ `opcode_name` |
| 4 | `a478f5c` | `BasicBlock`/`Function`/`Module` 所有权 + 编号（`%v.N`/`%arg.N`/`entry`/`bbN`）+ `create_function`/`create_block` |
| 5 | `b699f96` | IR printer（§4.5 格式，`store ptr,val` 顺序、phi `[val,bb]` 语法） |
| 6 | `c30f532` | `IRBuilder`（`create_*` 全套）；退役 `TestUser`，`test_use_list_wiring` 改用 `StoreInst` |

### 验证

- `./build/toyc-ir-tests` → **46/46 checks passed**
- `ctest --test-dir build --output-on-failure` → **2/2 passed**（`ir_tests` + `parser_regression` 全过）

### 实现期相对计划的偏差（均为实现细节，未改设计）

1. **`TestUser` 落点调整**：计划里 `TestUser` 曾规划放在 `ir.h` 的 `#ifdef TOYC_IR_TESTS` 内，最终落在 **`test/ir/ir_tests.cpp` 内**（不污染 `ir.h`），Task 6 直接删除，无 CMake `TOYC_IR_TESTS` 宏——比计划更干净。
2. **`set_operand` 的 nullptr 防御**：Task 1 调试时 `TestUser` 预填 `operands_` 为 `nullptr`，`set_operand` 解引用 `old->remove_use` 段错误；加了对 `old`/`value` 为 nullptr 的判空。这是唯一一处偏离"不加假设性防御"原则的代码，但属内部不变量维护（User 初始 operands 可能为 null），非用户输入边界验证。
3. **`FuncRet` 用 IR 本地枚举**：`Function` 返回类型用 IR 自己的 `enum class FuncRet { Int, Void }`，不 `#include "toyc/ast.h"`，IR 不依赖前端 AST。
4. **CMake：`toyc-ir-tests` 显式列出 IR 源文件**：测试 target 不复用 `toyc-compiler` 的源列表，而是单独列 `src/ir/*.cpp`，避免链接器找不到符号。

### 环境交付

- 装了 **CMake 4.0.3**（`pip3 install cmake==4.0.3`，二进制在 `/Users/kalin/miniconda3/bin/cmake`）。
- 安装方式 + 构建命令追加进 `CLAUDE.md` 的「开发环境 / 工具链」一节。

---

## 接下来要做的（Plan 2 / Plan 3，尚未起草）

Plan 1 之后的纯 IR 工作拆为两份独立 plan，依赖 Plan 1 已冻结的接口（`Module`/`Function`/`BasicBlock`/`Instruction`/`IRBuilder`/printer）。

### Plan 2 —— mem2reg（对应总计划 P3、设计 §6）

把非 SSA IR（alloca/load/store）提升为 SSA（phi + 重命名）。设计文档自评"最复杂、最易出 bug"，需强 TDD。

**需实现的算法链（设计 §6.2）：**
1. **CFG 重建**：从 terminator 推导每个块的 `succs`，反推 `preds`。Plan 1 的 `BasicBlock` 预留了 `preds_/succs_` 但未维护，需加 `recompute_cfg(Function&)`。
2. **支配树**：Cooper-Harvey-Kennedy 迭代算法（设计选型，~30 行，ToyC 规模够用），求每个块的 `idom`。
3. **支配边界 DF**：按定义点集合迭代计算。
4. **phi 插入**（worklist）：对每个可提升 alloca，从其 store 所在块出发，沿 DF 插 phi 至收敛。
5. **重命名**（支配树 DFS）：每 alloca 维护当前值栈；`load`→栈顶替换、`store`→压栈；后继块 phi 入边填值。
6. **清理**：删被提升的 alloca、被替换的 load、被删的 store。

**可提升条件**：ToyC 无 `&`，所有 alloca 理论上可提升（设计 §6.1）。

**测试策略**：手写非 SSA IR（含 if/while/短路，参考 `test/regression/parser/valid/*.tc` 的控制流形态）→ 跑 mem2reg → printer 比对 SSA 形态 + 断言 phi 正确性（incoming 覆盖全部前驱）。设计 §6.3 自检清单逐条覆盖。

**产物**：`include/toyc/mem2reg.h` + `src/ir/mem2reg.cpp`（含 CFG 支配工具，可能拆 `cfg_analysis.h`）。

### Plan 3 —— 优化 pass + driver 接线（对应 P4+P5、设计 §8/§9）

**优化 pass（设计 §8，`-opt` 时启用）：**
- `PassManager`：`run(Module/Function)`，不动点循环 `while(changed && iter<MAX_ITER=10)`。
- **ConstProp**：操作数全常量则折叠（含 `sdiv x,0` 不折叠的边界）。
- **DCE**：从副作用指令反向标 live。
- **CSE/GVN**：值编号哈希 + icmp 可交换性归一化（设计 §8.4 易错点）。
- **CFS**：块合并、不可达块删除、常量分支折叠、平凡 phi 化简。
- **AlgebraicSimplify**：`x+0`/`x*1`/`x*2^n→shl`（设计 §8.6，引入 `shl`，Plan 1 已支持）、`neg neg`。

**pass 顺序**（设计 §9）：`mem2reg → repeat{ConstProp,DCE,CSE,CFS,AlgebraicSimp} until !changed`。

**driver 接线（P5）：**
- `options` 加 `-dump-ir`（参考已有 `dump_ast`/`opt_mode`）。
- PassManager 作为可调用单元就位；但因无 IRGen（阻塞于 Sema），driver 暂不产 IR，端到端不通。

**测试策略**：每个 pass 手写 IR→跑→printer 比对；pass 间组合用设计 §12 的"开/关优化退出码一致"降级对照（需 codegen，故部分降级对照延后）。

**产物**：`include/toyc/opt/*.h` + `src/opt/*.cpp`；`options.h/cpp` 改动。

### 阻塞项（非本组可解）

- **Plan 2/3 的端到端验证**依赖 codegen（分工第 4 人，未实现）+ IRGen（依赖 Sema，`src/sema/` 仍空）。中端的 pass 级正确性可凭手写 IR 单测独立保证，但"优化保持语义"的退出码对照要等 codegen。
- IRGen↔Sema 接口已冻结（`IR-Sema接口请求反馈-前端组.md`），待 Sema 实现 `analyze()` 即可接 IRGen。

