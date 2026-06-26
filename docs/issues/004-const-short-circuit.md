# Const Short-Circuit Not Honored

## Summary

Compile-time constant evaluation does not honor short-circuit semantics for `&&` and `||`. It evaluates both operands before deciding the logical result, so valid constant expressions can be rejected.

## Affected Tests

- `f23_const_chain`
- Potentially `f20_comprehensive`
- Any test that uses constant chains with short-circuit expressions.

## Reproduction

```c
const int a = 0 && (1 / 0);

int main() {
    return a;
}
```

Expected result:

```text
0
```

Observed compiler diagnostics:

```text
error [sema] 1:25: division by zero in compile-time constant expression
error [sema] 1:11: global const 'a' initializer is not a compile-time constant
error [sema] 1:49: use of undeclared identifier 'a'
```

The same issue occurs with:

```c
const int a = 1 || (1 / 0);
```

The RHS should not be evaluated under C-like short-circuit semantics.

## Cause

`Analyzer::eval_const_expr` evaluates both sides of every binary expression before switching on the operator:

```cpp
auto lhs = eval_const_expr(*expr.binary.lhs);
auto rhs = eval_const_expr(*expr.binary.rhs);
if (!lhs || !rhs) {
    return std::nullopt;
}

switch (expr.binary.op) {
    ...
    case BinaryOp::And:
        return (*lhs != 0) && (*rhs != 0);
    case BinaryOp::Or:
        return (*lhs != 0) || (*rhs != 0);
}
```

Relevant file:

- `src/sema/sema.cpp`

This order is correct for arithmetic and relational operators, but not for logical `&&` and `||`. Those operators must decide whether the RHS is needed after evaluating the LHS.

## Impact

This is a semantic false rejection. The compiler rejects valid ToyC programs whose constant expressions rely on short-circuit behavior to avoid evaluating the RHS.

The bug can also cascade: once a global or local const declaration is rejected, later references to that const produce additional undeclared identifier diagnostics, making the root cause look noisier than it is.

