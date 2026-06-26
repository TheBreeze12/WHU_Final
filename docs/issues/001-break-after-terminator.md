# Break Executes Following Statements

## Summary

Statements placed after `break` in the same block can still be emitted into the same IR basic block and executed at runtime. This makes valid ToyC programs diverge from C semantics when `break` is followed by additional statements.

## Affected Tests

- `f06_break_continue`
- Potentially any comprehensive control-flow test that places code after `break` or mixes `break` with nested `if`/`while` statements.

## Reproduction

```c
int main() {
    int x = 1;
    while (x < 5) {
        break;
        x = 99;
    }
    return x;
}
```

Observed with the Docker RISC-V oracle:

```text
ToyC exit: 99
GCC exit: 1
```

The assignment after `break` should be unreachable. The generated ToyC assembly still lets it run.

## Actual IR Shape

The compiler emits the `break` branch, but then keeps appending following statements into the same basic block:

```text
bb2:
  br label bb3
  store %v.0, 99
  br label bb1
```

This is malformed control-flow IR for the intended semantics. The first `br` should terminate the block, but the current IR builder and AST walk allow more instructions to be appended afterward.

## Cause

`IRGenerator::visit_break_stmt` only emits a branch to the loop exit:

```cpp
void IRGenerator::visit_break_stmt(const BreakStmt&) {
    builder_->create_br(loops_.back().exit);
}
```

Relevant file:

- `src/ir/irgen.cpp`

The generic block walker then continues visiting later statements unconditionally:

```cpp
void walk_block(const BlockStmt& block, ASTVisitor& visitor) {
    for (const std::unique_ptr<Stmt>& stmt : block.body) {
        if (stmt) {
            visitor.visit_stmt(*stmt);
        }
    }
}
```

Relevant file:

- `src/frontend/ast_visitor.cpp`

The backend makes this worse by dropping an unconditional jump when the target block is the next block in the function layout:

```cpp
if (!is_fallthrough(*inst.parent(), target)) {
    writer_.inst("j", block_label(function_, target));
}
```

Relevant file:

- `src/codegen/codegen.cpp`

For a loop body followed by the loop exit block, `break` often targets the next block. Because the backend treats that as fallthrough, it removes the required jump. However, the current basic block still contains later instructions, so execution falls into those later instructions instead of the exit block label.

## Impact

This is a correctness bug, not just an IR cleanliness issue. It changes observable program results whenever code after `break` mutates state or affects the return value.

The issue is easiest to trigger with direct code after `break`, but it can also surface through nested statement shapes where `break` occurs before additional statements in the same block.

