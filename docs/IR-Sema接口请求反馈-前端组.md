# IR-Sema 接口请求 —— 前端组反馈

> 回复对象：[`IR-Sema接口请求反馈.md`](./IR-Sema接口请求反馈.md)  
> 回复方：**前端组**（lexer / parser / AST）  
> 日期：2026-06-22  
> 状态：**已反馈，待 Sema / IRGen 确认**

---

## 1. 总览

前端组已阅读 IR 子模块设计（[`2026-06-22-toyc-ir-optim-design.md`](./superpowers/specs/2026-06-22-toyc-ir-optim-design.md) §3.1）与接口请求文档。对 **R4 / R5 关闭** 无异议；对 **R1 / R2 / R3** 的交付方式给出如下立场，并已在代码中落地前端侧配套接口。

| IR 需求 | 前端现状 | 前端立场 |
|---------|----------|----------|
| R4 统一遍历 | ✅ `ASTVisitor` + 细粒度 `visit_*` | 已增强，Sema/IRGen 可直接继承 |
| R5 内存稳定 | ✅ `unique_ptr` 树，节点地址稳定 | 见 `ast_contract.h` 约定 |
| R1 def-use | ❌ AST 无标注 | **支持方案 2**，不改 `ast.h` |
| R2 const 折叠 | ❌ init 未折叠 | **Sema 负责**，经 `SemaResult` 交付 |
| R3 类型 | ❌ Expr 无 type | **IRGen 自扫签名**；前端提供 helper |
| 全局 var 初值 | Parser 不检查 | **禁止运行期表达式**；Sema 编译期求值检查（Q5 已拍板） |

---

## 2. 对问题清单的答复（前端视角）

### Q1 — 标识符解析结果传递方式

**前端意见：方案 2（SemaResult side-table）✅**

- 不在 `ast.h` 增加 `resolved` / `const_value` 等标注字段（保持 Parser 产物纯净）。
- side-table 以 `const IdentExpr*`、`const AssignStmt*` 为 key 可行；前端保证 AST 在 Sema→IRGen 期间不移动/重建子树。

### Q2 — 常量折叠结果传递方式

**前端意见：Sema 在 `SemaResult::const_values`（或等价结构）中交付 ✅**

- const 编译期可定是 **Sema 硬需求**（任务要求 + 协作说明 §8）。
- 前端 **不** 在 Parser 阶段折叠，也 **不** 在 AST 节点上内嵌折叠结果。
- 若 IRGen 需要「const 引用当立即数」，从 SemaResult 查 `const IdentExpr* → int` 即可。

### Q3 — 函数返回类型

**前端意见：IRGen 自扫顶层 `FuncDef` 即可 ✅**

- `FuncDef::return_type` 已在 AST 签名上；无需 Sema 专门标注 `CallExpr`。
- 前端提供 **`build_func_signature_map(CompUnit)`**（`include/toyc/ast_access.h`），IRGen 启动时一行建表。

### Q4 — `SymbolRef` 应携带的信息

**前端建议 Sema 定义 `SymbolRef`，至少包含：**

```cpp
struct SymbolRef {
    SymbolStorageKind storage;  // 见 include/toyc/ast_contract.h
    const void* decl;           // 指向 GlobalConstDecl / GlobalVarDecl /
                                  // ConstDeclStmt / VarDeclStmt / Param
};
```

- `SymbolStorageKind` 已由前端在 `ast_contract.h` 提出五类枚举，Sema/IRGen 共用，避免 magic number。
- IRGen 至少需要区分：局部 vs 全局、const vs var、形参；上述字段足够。

### Q5 — 全局变量初值是否允许运行期表达式

**已拍板：不允许。**

- 全局变量初值 **不得** 含运行期表达式（如函数调用、对非常量变量的引用等）。
- 初值必须在 **编译期可求值**，规则与 `const` 折叠一致：数字字面量、已声明的 **const**、以及由它们组成的算术/逻辑运算。
- Parser 文法仍允许任意 `Expr` 作初值；**Sema 负责检查并报错**。
- IRGen 可假定全局变量初值为静态常量，直接写入 `.data`，无需运行期 init 代码。

### Q6 — 交付时机 / IRGen 是否先用方案 3

**非前端排期项。** 前端 AST / Visitor / 契约头文件 **现已就绪**，不阻塞 Sema 开工。

---

## 3. 前端已实现的配套交付

| 交付物 | 路径 | 用途 |
|--------|------|------|
| 符号存储类别枚举 | `include/toyc/ast_contract.h` | Sema `SymbolRef` / side-table 共用 |
| AST 类型安全访问 | `include/toyc/ast_access.h` | `as_ident` / `as_call` 等，减少 switch |
| 函数签名表 helper | `build_func_signature_map()` | IRGen 满足 R3，无需 Sema 标注 |
| 函数列表 helper | `collect_func_defs()` | IRGen 预扫 / 注册 |
| 细粒度 Visitor | `include/toyc/ast_visitor.h` | `visit_ident` / `visit_assign_stmt` 等 |
| AST 生命周期约定 | `ast_contract.h` 注释 | side-table 指针 key 合法性 |

---

## 4. 边界重申（与协作说明一致）

1. **Parser / AST 结构**：前端维护；Sema 需求的新语义字段 → 走 **SemaResult**，不改 Parser。
2. **`ast_contract.h` / `ast_access.h`**：前端维护的**下游契约头文件**；Sema 可 `#include` 使用 `SymbolStorageKind`，但 **SemaResult 定义在 `src/sema/`**，由语义组拥有。
3. **诊断**：Sema 使用 `DiagnosticStage::Sema`，格式与前端统一。

---

## 5. 建议的下游接口形状（供 Sema 参考，非强制）

```cpp
// src/sema/sema_result.h —— 由 Sema 组实现
struct SemaResult {
    std::unordered_map<const IdentExpr*, SymbolRef> idents;
    std::unordered_map<const AssignStmt*, SymbolRef> assigns;
    std::unordered_map<const CallExpr*, const FuncDef*> calls;  // 可选
    std::unordered_map<const void*, int> const_values;
    bool ok = false;
};

SemaResult analyze(const CompUnit& unit, DiagnosticEngine& diagnostics);
```

IRGen 入口建议：`void irgen(const CompUnit& unit, const SemaResult& sema, ...);`

---

## 6. 行动项

| 方 | 行动 |
|----|------|
| **Sema** | 确认采用方案 2；定义 `SymbolRef` + `SemaResult`；实现 `analyze()`；**全局/局部 VarDecl 初值与 const 一样做编译期可求值检查（Q5）** |
| **IRGen** | 确认 R3 用 `build_func_signature_map`；**全局变量初值一律静态常量（Q5）**，无需运行期 init |
| **前端** | ✅ 已完成本反馈与契约头文件；后续仅随 AST 文法变更同步 |
