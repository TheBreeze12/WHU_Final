# Large Stack Offsets Break Assembly

## Summary

Programs with many local variables or many temporary expression results can generate stack frame sizes and stack offsets outside the RV32I 12-bit immediate range. The emitted assembly then fails to assemble.

## Affected Tests

- `f17_complex_expressions`
- `f18_many_variables`
- `f20_comprehensive`
- Any large expression or large local-variable stress test.

## Reproduction

Generated stress cases with hundreds of local variables or a long arithmetic expression fail during RISC-V assembly.

Observed assembler errors:

```text
Error: illegal operands `addi sp,sp,-8400'
Error: illegal operands `addi t0,sp,2048'
Error: illegal operands `sw t1,2048(sp)'
Error: illegal operands `lw t0,2048(sp)'
```

Smaller cases pass. For example, a case with around 80 local variables returned the same result as GCC. Larger cases fail before execution because the assembly is invalid.

## Cause

The frame layout assigns every alloca and result-producing instruction a stack slot:

```cpp
if (inst->opcode() == Opcode::Alloca || is_result_slot_inst(*inst)) {
    slots_.emplace(inst.get(), next_offset_);
    next_offset_ += 4;
}
frame_size_ = align_to(next_offset_, 16);
```

Relevant file:

- `src/codegen/codegen.cpp`

The prologue and epilogue emit the full frame size directly as an `addi` immediate:

```cpp
writer_.inst("addi", reg_name(RvReg::Sp), reg_name(RvReg::Sp),
             std::to_string(-frame_.frame_size()));
```

Relevant file:

- `src/codegen/codegen.cpp`

Loads, stores, and local address materialization also emit stack offsets directly:

```cpp
writer_.inst("lw", reg_name(dst), offset_addr(frame_.slot_offset(value), RvReg::Sp));
writer_.inst("sw", reg_name(src), offset_addr(frame_.slot_offset(&inst), RvReg::Sp));
writer_.inst("addi", reg_name(dst), reg_name(RvReg::Sp),
             std::to_string(frame_.slot_offset(ptr)));
```

Relevant file:

- `src/codegen/codegen.cpp`

RV32I immediate forms such as `addi`, `lw`, and `sw` require signed 12-bit immediates. Offsets like `2048` and frame sizes like `8400` cannot be encoded directly.

## Impact

This is an assembly validity bug. The compiler can successfully parse, analyze, and generate IR for a valid ToyC program, but the final assembly is rejected by the RISC-V assembler.

The failure threshold depends on the number of locals, temporaries, saved registers, outgoing stack arguments, and phi temporaries. Hidden tests named `many_variables`, `complex_expressions`, and `comprehensive` are natural triggers.

