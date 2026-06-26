# OR Short-Circuit Skips RHS

## Summary

Logical OR short-circuit code generation can incorrectly skip the right-hand side even when the left-hand side is false. This breaks runtime side effects and return values for expressions like `0 || side_effect()`.

## Affected Tests

- `f08_short_circuit`
- `f30_short_circuit_global_side_effect`
- Potentially `f16_complex_syntax`, `f17_complex_expressions`, and `f20_comprehensive` when they contain `||`.

## Reproduction

```c
int g = 0;

int side() {
    g = g + 1;
    return g;
}

int main() {
    if (0 || side()) {
    }
    return g;
}
```

Observed with the Docker RISC-V oracle:

```text
ToyC exit: 0
GCC exit: 1
```

The RHS call must run because the LHS is false. In the generated ToyC program, the call is skipped.

## Cause

`IRGenerator::short_circuit` creates blocks in this order:

```cpp
BasicBlock* rhs_bb = entry_->parent()->create_block();
BasicBlock* short_bb = entry_->parent()->create_block();
BasicBlock* merge_bb = entry_->parent()->create_block();
builder_->create_cond_br(lhs_bool, is_and ? rhs_bb : short_bb,
                                  is_and ? short_bb : rhs_bb);
```

Relevant file:

- `src/ir/irgen.cpp`

For `||`, the true edge goes to `short_bb`, and the false edge goes to `rhs_bb`. Since `rhs_bb` is created first, it is usually the physical fallthrough block after the current block.

The backend conditional branch lowering emits the true-edge trampoline label immediately after handling the false edge:

```cpp
writer_.inst("bne", reg_name(RvReg::T0), reg_name(RvReg::Zero), true_copy_label);

if (!emit_phi_copies(false_target, *inst.parent())) {
    return false;
}
if (!is_fallthrough(*inst.parent(), false_target)) {
    writer_.inst("j", block_label(function_, false_target));
}

writer_.label(true_copy_label);
```

Relevant file:

- `src/codegen/codegen.cpp`

There is an equivalent shape in the direct compare branch path:

```cpp
if (!is_fallthrough(*branch.parent(), false_target)) {
    writer_.inst("j", block_label(function_, false_target));
}
writer_.label(true_copy_label);
```

Relevant file:

- `src/codegen/codegen.cpp`

When `false_target` is physically the next IR block, the backend omits the jump to it. But the next emitted assembly label is the true-edge trampoline, not the false target block label. As a result, the false path falls into the true-edge path and skips the RHS.

## Why AND Often Appears Fine

For `&&`, the common block order and branch direction happen to line up better in simple cases:

- true edge evaluates RHS;
- false edge stores the short-circuit result;
- the physical fallthrough often does not collide with the true-edge trampoline in the same way.

This does not make the lowering strategy safe in general. The confirmed failure is with `||`.

## Impact

This is a high-impact semantic bug because ToyC requires C-like short-circuit behavior. It changes whether function calls execute, whether global variables are mutated, and whether divisions or other potentially invalid RHS operations are evaluated.

