# ToyC 编译器 —— IR + 优化 子模块设计

- 日期：2026-06-22
- 状态：已起草，待 review（含跨人接口待协调项）
- 负责人：IR+优化（分工第 3 人）
- 上游文档：`docs/superpowers/specs/2026-06-22-toyc-compiler-design.md`（总设计）、`任务要求.md`（权威文法与评分）
- 范围：本 spec 只覆盖**中端**——IR 数据结构、IRGen、mem2reg、核心优化 pass。前端/sema 与 codegen 仅以接口契约形式出现，不定义其内部实现。寄存器分配归 codegen（见 §10 待协调）。

---

## 1. 流水线定位

```
前端/sema ──AST──▶ 【IRGen】 ──非SSA IR──▶ 【mem2reg】 ──SSA IR──▶ 【优化pass】 ──优化后SSA IR──▶ codegen
                   (alloca/load/store)     (phi/重命名)           (常量传播/DCE/CSE/...)
```

- **输入**：经 sema 类型检查、常量折叠、作用域解析后的 AST（节点已携带 def-use link 与折叠结果，见 §3.1）。
- **输出**：优化后的 SSA IR（含 phi），交 codegen 做 deSSA + 寄存器分配 + 指令选择 + 汇编输出。
- **触发**：IRGen/mem2reg 在所有路径都跑；优化 pass 仅在 `-opt` 时跑（功能测试默认不优化，保功能分；见 §12）。

---

## 2. 设计目标与权衡基线

1. **功能优先**：任何时刻允许"跳过 mem2reg / 跳过优化"，退化为栈式非 SSA IR 也能产出正确汇编（降级回退，对应总设计 §14）。
2. **优化换分**：性能分占评测 25%，以 `gcc -O2` 运行时间为基准，故中端 SSA 化 + 核心优化是主要换分点。
3. **可独立测试**：IR 是可打印、可手工构造的数据结构；每个 pass 是 `IR→IR` 纯函数。
4. **接口最小**：对前端只提 5 条 AST 需求（§3.1），对 codegen 只交付 SSA IR 文本（§3.2），不互相侵入内部表示。

---

## 3. 接口契约

> 本节是跨人交接面。标 ⚠️ 的条目需与对应同学对齐后再定稿。

### 3.1 输入契约 —— IRGen 对 AST 的需求（⚠️ 待与前端/sema 对齐）

**前端 Parser 已落地**（lexer/parser/ast/visitor/printer，详见 [`docs/frontend-协作说明.md`](../../frontend-协作说明.md)），AST 结构与 `ASTVisitor` 遍历机制已定型。对照实测：**R4（遍历）、R5（内存）已满足并关闭**；R1/R2/R3（def-use / const 折叠 / 类型）属 Sema 职责、Sema 待实现，传递机制三方案与请求清单见 [`docs/IR-Sema接口请求反馈.md`](../../IR-Sema接口请求反馈.md)。IRGen 的需求：

| # | 需求 | 为什么 | 不满足的后果 |
|---|------|--------|--------------|
| R1 | `ID` 引用节点携带**指向其声明节点**（`VarDecl`/`ConstDecl`/`Param`）的指针，由 sema 作用域解析时填入；不要裸字符串 | IRGen 需把变量引用映射到它的存储槽；SSA 重命名也要此 link | IRGen 要重建作用域查表，与 sema 重复且易不一致 |
| R2 | **const 引用已折叠为字面量**（sema 的 `const_eval` 完成） | const 必须编译期可定（语义约束）；IRGen 直接当立即数 | IRGen 要重复实现常量折叠 |
| R3 | 表达式节点携带**结果类型**（至少区分 `int` / `void`） | void 函数调用不能出现在表达式位置（sema 已查）；IRGen 需知一个 CallExpr 是否 void | IRGen 无法区分"语句位置 void 调用"与"表达式求值" |
| R4 | AST 提供**统一的遍历机制**（见 §11，三种方案待选） | IRGen / sema / 未来的任何 AST 遍历共用 | 每个消费者各写一套 if-else |
| R5 | AST 内存模型明确（arena / `unique_ptr`），IRGen 可安全持有引用 | IRGen 在 IR 生成期间需引用 AST 节点 | 悬垂指针 / 所有权冲突 |

**节点种类（文法决定，前端必须提供）**：`CompUnit / ConstDecl / VarDecl / FuncDef / Param / Block / If / While / Break / Continue / Return / Assign / CallStmt / ExprStmt / Empty / Binary / Unary / Number / LValue(ID) / Call`。

### 3.2 输出契约 —— 交付 codegen 的 SSA IR（⚠️ 待与 codegen 对齐）

- **IR 文本格式**：类 LLVM 风格（§4.5 给完整示例与词法）。`-dump-ir` 复用同一格式。
- **SSA 性质保证**：每个结果值唯一命名（`%v.N`），phi 仅出现在基本块首，无 `copy`（copy 是 codegen 的 deSSA 产物，不属于本层 IR）。
- **phi 语义**：`%v = phi [val_a, bb_a], [val_b, bb_b]` —— 进入本块时，依来源前驱块取对应 value。要求每个 phi 的 incoming 列表覆盖全部前驱块。
- **deSSA / 寄存器分配**：⚠️ 归属 codegen（总设计 §7、分工第 4 人）。但 §10 流水线把寄存器分配列为优化第 7 步——**此歧义需协调定论**。本 spec 立场：寄存器分配属 codegen，中端止于 SSA IR。

---

## 4. IR 数据结构设计

### 4.1 类层次（类 LLVM Value 体系）

```
Value                       // 任何"有值"的东西：有名字 + use 列表
├── Constant                // 立即数：ConstantInt(value)
├── GlobalAddr              // 全局变量地址（名字）
├── BasicBlock              // 也是 Value（label）
├── Function                // 也是 Value（函数名）
└── User : Value            // 使用 operands 的实体
    └── Instruction         // 所有指令
        └── PHINode         // phi 单列子类，便于"必须在块首"的校验
```

- **决策：采用 Value/User 体系**。
  - *做什么*：每个产生结果的指令 `is-a Value`；任何消费 Value 的实体 `is-a User`，持有 `operands: vector<Value*>`。`Value` 反向持有 `uses: vector<User*>`。
  - *为什么*：use-def 与 def-use 链双向天然可得，常量传播/DCE/CSE 直接沿链走，无需额外数据结构。这是 SSA 优化的地基。
  - *怎么调*：若某 pass 需批量改 use，遍历 `value->uses()` 即可。

### 4.2 层次容器

```
Module
├── globals : list<GlobalVar>      // 全局变量（带初值）
├── functions : list<Function>
└── counter : int                  // 虚拟寄存器全局唯一编号

Function : Value
├── params   : vector<Value*>      // 形参虚拟寄存器
├── blocks   : list<BasicBlock*>   // entry 为首
├── ret_type : i32 | void
└── slot_map / 局部信息

BasicBlock : Value
├── insts    : list<Instruction*>  // 末尾唯一 terminator
├── preds    : vector<BasicBlock*>
└── succs    : vector<BasicBlock*> // 由 terminator 推导
```

### 4.3 命名与编号

- 虚拟寄存器：`%v.0, %v.1, ...`，Module 级单调递增计数器分配，全局唯一。
- 形参：`%arg.0, %arg.1, ...`（与一般 value 区分，便于读 IR）。
- 基本块 label：entry 块固定 `entry`；其余 `bb<N>`（数字编号，避免语义名冲突）。`-dump-ir` 可选附加语义注释。

### 4.4 类型系统

ToyC 只有 `int`（32 位有符号）。IR 显式类型集合：

| 类型 | 含义 | 出现于 |
|------|------|--------|
| `i32` | 32 位有符号整数 | 数值、运算结果、形参、返回值 |
| `ptr` | 指向 i32 的栈/全局地址 | `alloca` 结果、`GlobalAddr`、load/store 的指针操作数 |
| `void` | 无值 | void 函数返回、void call |
| `label` | 基本块标号 | terminator 的跳转目标 |

- **决策：显式 `ptr` 类型**。
  - *为什么*：`alloca` 结果是指针，load/store 的指针操作数需要类型校验；显式 ptr 让 IR 自描述、`-dump-ir` 可读、codegen 易做栈帧布局。
  - *怎么调*：若未来 ToyC 加数组/指针（目前文法无），需扩展 ptr 指向类型；当前固定为 `ptr→i32`。

### 4.5 IR 文本格式（交付 codegen 与 `-dump-ir` 共用）

```
; ---- Module 头 ----
@g_count = global i32 0          ; 全局变量（初值常量）
@answer = const i32 42           ; 全局常量（已折叠，codegen 可放 .rodata）

define i32 @add(i32 %arg.0, i32 %arg.1) {
entry:
  %v.0 = alloca i32
  store %v.0, %arg.0             ; 形参下沉到栈槽（见 §5.3）
  %v.1 = load %v.0
  %v.2 = add %v.1, 1
  %v.3 = icmp slt %v.2, 10
  cond_br %v.3, label %bb1, label %bb2
bb1:                            ; 循环头：%v.4 是归纳变量（phi 引用本块下游的 %v.6 表示上一轮迭代值）
  %v.4 = phi [%v.1, %entry], [%v.6, %bb1]
  %v.5 = call @foo, %v.4
  %v.6 = add %v.4, 1
  %v.7 = icmp slt %v.6, 10
  cond_br %v.7, label %bb1, label %bb2
bb2:
  ret %v.2
}
```

- **决策：store 写作 `store <ptr>, <val>`（指针在前）**。
  - *为什么*：与"写到哪、写什么"的语序一致，读 IR 顺；与 LLVM 相反是有意为之，ToyC 体量小不必照搬。
  - *怎么调*：codegen 同学若坚持 LLVM 习惯（`store val, ptr`），改格式化一处即可，不影响语义。

---

## 5. IRGen（AST → 非 SSA IR）

本阶段产物**非 SSA**：局部变量经 `alloca/load/store`。SSA 化交给 §6 mem2reg。

### 5.1 局部变量 → 栈槽

- 在 `entry` 块**开头**为每个局部变量声明生成一条 `%v = alloca i32`。
  - *为什么集中放 entry*：alloca 不会在控制流中间执行，栈帧布局在函数入口一次确定，利于 codegen 计算栈大小。
  - *怎么调*：若担心 entry 过长，可按作用域分块，但无收益（mem2reg 后多数 alloca 会被删）。

### 5.2 表达式求值（递归，每 Expr 产一个 Value）

| AST 节点 | 生成指令 |
|----------|----------|
| `Number(n)` | `Constant(n)`（不生成指令，作为操作数） |
| `LValue`（变量引用，R1 link → 其 alloca） | `load <alloca>`；const 引用（R2 已折叠）→ `Constant` |
| `Binary(op,l,r)`（算术 `+ - * / %`） | 递归 l→`%a`、r→`%b`，产 `add/sub/mul/sdiv/srem %a, %b` |
| `Binary(op,l,r)`（关系 `< > <= >= == !=`） | `icmp slt/sgt/sle/sge/eq/ne %a, %b`（结果 0/1） |
| `Binary(&&,\|\|,l,r)` | 短路，控制流建模（§5.5），非单条指令 |
| `Unary(-,e)` | 递归 e→`%a`，产 `neg %a`（或 `sub 0, %a`） |
| `Unary(!,e)` | 递归 e→`%a`，产 `icmp eq %a, 0`（逻辑非 = 是否为零） |
| `Unary(+,e)` | 透传 `%a`（正号无操作） |
| `Call(f,args)` | 递归各 arg，产 `call @f, <args...>`；void call 无结果值（R3） |
| `(e)` | 透传 |

- **决策：`!e` 直接用 `icmp eq e, 0`，不设独立 `lnot` 指令**。
  - *为什么*：减少指令种类，逻辑非本质是比较；icmp 已覆盖，代数化简（`!!x`）也落在 icmp 上更统一。
  - *怎么调*：若 codegen 发现 icmp eq 0 的模式低效，可在指令选择层合并，不影响 IR 层。
- **决策：算术除/模用 `sdiv`/`srem`（有符号）**。
  - *为什么*：ToyC int 是 32 位有符号，C 语义 `/` `%` 对负数向零取整，RISC-V `div`/`rem` 正好向零取整，语义一致。
  - *怎么调*：若未来要支持无符号（ToyC 无），需另设 `udiv`/`urem`。

### 5.3 形参处理

- **决策：形参也下沉到 alloca**。函数入口对每个形参 `store <alloca_arg.i>, %arg.i`；函数体内对形参的读写一律经该 alloca。
  - *为什么*：与局部变量统一走 mem2reg；形参虽不可被赋值（非左值），但统一槽模型让 mem2reg 算法不必区分形参/局部，正确性更稳。
  - *代价*：多 N 条 store，mem2reg 后消除。
  - *怎么调*：性能敏感时可直接让形参作为 SSA value 进入函数体（跳过 alloca），但要改 mem2reg 的"可提升集合"判定，复杂度上升，暂不做。

### 5.4 控制流建模

- **`if (c) T else E`**：
  ```
  %cond = <eval c>
  %c0 = icmp ne %cond, 0          ; 非零为真
  cond_br %c0, label %then, label %else
  then:  ...; br label %join
  else:  ...; br label %join      ; 无 else 时 else 块直接 br %join
  join:  ...
  ```
- **`while (c) B`**：四块式 `cond → body → cond` + `exit`：
  ```
  br label %while.cond
  while.cond:
    %cond = <eval c>; %c0 = icmp ne %cond, 0
    cond_br %c0, label %while.body, label %while.exit
  while.body:
    <B>; br label %while.cond
  while.exit:
  ```
- **`break` / `continue`**：维护**循环栈**（编译期数据结构，非 IR），每层记录 `{cond_block, exit_block}`。
  - `break` → `br <exit_block>`；`continue` → `br <cond_block>`。
  - *为什么用 cond 块而非 step 块*：ToyC 的 while 无 for 的 step 子句，continue 回到条件判断即正确。
  - sema 已保证 break/continue 仅出现在循环内（R3 之外的语义保证，由 sema 检查清单覆盖）。

### 5.5 短路求值（`&&` / `||`）

显式建模为基本块 + 条件跳转，**不**用单条 and/or 指令。以 `a && b` 为例（结果即整体表达式值）：

```
; a && b  ==  a==0 ? 0 : (b!=0)
  %a = <eval a>
  %c0 = icmp eq %a, 0
  cond_br %c0, label %false, label %rhs
rhs:
  %b = <eval b>
  %cb = icmp ne %b, 0
  br label %join             ; 真分支结果 = %cb
false:
  br label %join             ; 假分支结果 = 0
join:
  %r = load <slot_and>       ; mem2reg 前：用 alloca 槽
```

- mem2reg **前**：`%r` 经 entry 的 `<slot_and> = alloca`，在 rhs `store <slot_and>, %cb`、在 false `store <slot_and>, 0`、join `load`。
- mem2reg **后**：join 块首插 phi：`%r = phi [%cb, %rhs], [0, %false]`。
- `||` 对偶：`a==0 ? (b!=0) : 1`。
  - *为什么这样切块*：保证短路语义（b 仅在 a 非零时求值），且 phi 的 incoming 与控制流边严格对应，mem2reg 正确性可追溯。

### 5.6 全局变量 / 全局常量

- 全局变量 → Module 的 `globals` 列表，登记初值。
  - **初值要求**：⚠️ 总设计 §8.3/§15 开放问题 3。本 spec 假设"全局变量初值为常量折叠可求值"（任务要求"所有声明必须带初始化"，且 const 约束已让前端折叠器覆盖多数情况）。
  - 引用全局变量 → `load @g_x`（`@g_x` 是 `GlobalAddr`，ptr 类型）。
- 全局常量 → 前端已折叠（R2），IRGen 直接当 `Constant` 内联，**不**生成 load、不占运行期存储。
  - *为什么*：常量内联是最强优化（零访问成本），且语义等价。

### 5.7 函数返回

- `ret <val>`（int 函数）/ `ret`（void 函数）。
- sema 已保证 int 函数所有路径 return、void 函数 return 无值（R3/检查清单）。
- main 的退出码：`ret %main_ret` 的值最终经 codegen 落到 `a0`（epilogue 形式见总设计 §8.4，属 codegen）。

---

## 6. mem2reg（非 SSA → SSA）

经典支配边界算法（Cytron et al.）。**本模块最复杂、最易出 bug 处。**

### 6.1 可提升条件

- 一个 alloca 可提升 ⟺ 它只被 `load`/`store` 使用，且无取地址（ToyC 无 `&`，**理论上全部可提升**）。
- **决策：默认尝试提升所有 alloca**；保留 `-opt` 或独立开关"跳过 mem2reg"作降级（§12）。

### 6.2 算法步骤

1. **构建 CFG**：计算每个块的 `preds/succs`（由 terminator 推导）。
2. **支配树**：用 Cooper-Harvey-Kennedy 迭代算法（ToyC 函数小，够用；Lengauer-Tarjan 无必要）。
   - *为什么选 Cooper*：实现 ~30 行，O(N·E) 对 ToyC 规模绰绰有余，且支配树正确性易用 Oracle 验证。
3. **支配边界（DF）**：对每个块按定义计算 DF。
4. **phi 插入**（worklist）：
   - 对每个可提升 alloca X，收集所有 `store X` 所在块 `defs(X)`。
   - worklist = `defs(X)`；对其中每个块 B，对 B 的每个 DF 块 Y：若 Y 尚无 X 的 phi，插入一个，并把 Y 加入 worklist（迭代至收敛）。
5. **重命名**（支配树 DFS）：为每个 alloca 维护"当前值栈"。
   - 进入块 B：先把 B 首所有 phi（针对各 alloca）压栈作为该 alloca 的新定义。
   - 遍历 B 内指令：
     - `load X` → 用 X 栈顶值替换 load 的所有 use，标记 load 待删。
     - `store X, v` → 压 v 为 X 当前值，删 store。
   - 遍历 B 的每个后继 S：对 S 首的每个 phi（针对 X），用 X 栈顶值填入对应 `(B, value)` 入边。
   - 递归支配树子节点。
   - 退出块 B：弹出本块压入的所有值（恢复栈）。
6. **清理**：删除被完全提升的 alloca、所有被替换的 load、被删的 store。

### 6.3 正确性条件（自检清单）

- [ ] 支配树与 DF 计算正确（用小图例 + Oracle 双重验证）。
- [ ] phi 插入位置恰在 DF（最小化插入）。
- [ ] 重命名严格按支配树 DFS，栈压/退严格配对。
- [ ] 每个 phi 的 incoming 覆盖全部前驱块（无遗漏边）。
- [ ] 删除 alloca 前，确认其已无 load/store 残留。

---

## 7. IR 指令集（完整）

| 类别 | 指令 | 语义 | 结果类型 |
|------|------|------|----------|
| 二元算术 | `add/sub/mul/sdiv/srem a, b` | `a±*/% b`（有符号） | i32 |
| 比较 | `icmp {eq,ne,slt,sgt,sle,sge} a, b` | 关系比较，结果 0/1 | i32 |
| 一元 | `neg a` | `-a` | i32 |
| 控制流 | `br label L` | 无条件跳 | — |
| 控制流 | `cond_br c, label T, label F` | c≠0 跳 T 否则 F | — |
| 控制流 | `ret [v]` | 返回（可带值） | — |
| 内存 | `alloca i32` | 分配 i32 栈槽 | ptr |
| 内存 | `load p` | 读 *p | i32 |
| 内存 | `store p, v` | *p ← v | — |
| 调用 | `call @f, a1, ...` | 调用，结果在 a0 | i32 或 void |
| SSA | `phi [v1,b1], [v2,b2], ...` | 依来源前驱取值 | i32 |
| *(deSSA)* | `copy v` | ⚠️ 不在本层；codegen deSSA 产物 | — |

**刻意不设的指令**（决策点）：
- 无 `and/or/shl/shr` 作为源语义指令：ToyC 文法**无位运算**（只有 `&& || !`，已由控制流/cmp 建模）。
  - *例外*：代数化简（§8.5）做 `x*2^n → x shl n` 时，**允许**优化阶段引入 `shl`/`shr` 指令（标注来源 = 优化产生，非源语义）。codegen 需支持。
- 无 `lnot`：逻辑非用 `icmp eq, 0`。
- 无 `copy`：phi 未消除前不需要；消除后是 codegen 的事。

---

## 8. 优化 pass（核心，`-opt` 时启用）

### 8.1 通用框架

- 每个 pass 实现 `bool run(Function&)`（或 `run(Module&)`），返回 `changed`。
- **PassManager**：按 §9 顺序跑；整体可 `while (any_changed && iter < MAX_ITER) { run_all(); }`。
  - *为什么加不动点迭代*：常量传播会暴露新的 DCE 机会、DCE 又会暴露新的 CSE 机会，需多轮收敛。
  - *怎么调*：`MAX_ITER` 默认 10（ToyC 函数小，通常 2–3 轮收敛）；达上限即停，保证终止。
- 每个 pass 是**纯函数**（输入 IR，输出/原地改 IR，无外部状态），可独立单测。

### 8.2 常量折叠 / 传播（ConstProp）

- 对每条指令：若**所有操作数均为 `Constant`**，则编译期求值，用新 `Constant` 替换该指令的所有 use，删该指令。
  - `add 2, 3 → 5`；`icmp slt 2, 3 → 1`；`sdiv 6, 2 → 3`。
- 工作表（worklist）：沿 def-use 传播——某值变常量后，把其所有 user 加入工作表重算。
- **边界**：`sdiv x, 0` 不折叠（保持原指令，运行期行为由评测保证无除零）；`call` 不折叠。

### 8.3 死代码消除（DCE）

- 标记存活：从有副作用的指令（`store/call/ret/br/cond_br`）出发，沿 use 链反向标记所有 operand 指令为 live。
- 删除：未被标记 live、且其结果无 use 的纯计算指令（`add/sub/.../icmp/neg/alloca(无load/store引用)/phi(无use)`）。
- **决策：`alloca` 仅在仍被 load/store 引用时 live**（mem2reg 残留的不可提升 alloca 才会留下，一般已无）。
- **迭代**：删一批后可能产生新的死指令，重复到不动点（或在 §8.1 的外层循环里跑）。

### 8.4 公共子表达式消除 / 全局值编号（CSE / GVN）

- **GVN**：给每个 value 一个"值编号"（相同计算 → 同编号）。
  - `add a, b` 与 `add c, d`：若 `a,c` 同编号且 `b,d` 同编号，则两者等价，后者替换为前者。
- 实现：扫描指令，按 `(opcode, operand编号)` 入 hash 表；命中则用表中已有 value 替换当前指令的所有 use，删当前指令。
- **注意的关系化简**（决策点，易错）：
  - `icmp eq/ne` 操作数可交换：`eq a,b ≡ eq b,a`。
  - `icmp slt a,b ≡ icmp sgt b,a`；`sle a,b ≡ sge b,a`。
  - 哈希时对比较指令**归一化操作数序**（如按值编号排序），否则漏判。
- *怎么调*：保守起见，GVN 仅对**无副作用**的纯计算指令生效（call 不参与）。

### 8.5 控制流简化（CFS）

- **合并相邻块**：A 的唯一后继是 B，且 B 的唯一前驱是 A → 合并（A 的 terminator 删，指令并入 B）。
- **删不可达块**：从 entry BFS/DFS 标记可达，删不可达块（及其被引用 phi 入边的清理）。
- **常量条件分支折叠**：`cond_br Constant c, T, F` → `c≠0 ? br T : br F`，并删掉不可达的那一支。
- **空/单入 phi 化简**：phi 仅一个 incoming → 用该值替换 phi；phi incoming 全相同 → 用该值替换。

### 8.6 代数化简 / 窥孔（AlgebraicSimplify）

局部模式匹配，**仅当更优**才替换：
- `x+0 → x`；`x-0 → x`；`x*1 → x`；`x*0 → 0`；`0-x → neg x`。
- `neg neg x → x`；`icmp eq (neg x), 0` 类不展开。
- **强度削弱（乘法）**：`x * 2^n → x shl n`（引入 `shl` 指令，见 §7）。
  - *为什么只做乘法不做除法*：`x / 2^n` 对负数不等价于算术右移（C 向零取整 vs 右移向下取整），错误化简会破坏语义。**默认不做除法强度削弱。**
  - *怎么调*：若确认某除数是 2 的幂且能证明非负（如循环计数器），可条件化简；初期不做。

---

## 9. pass 顺序与组合

```
mem2reg           ; 必须最先：打开 SSA 优化空间
repeat {
  ConstProp       ; 折叠常量
  DCE             ; 清理 ConstProp 产生的死代码
  CSE/GVN         ; 合并等价计算
  CFS             ; 常量分支折叠 + 块合并（吃 ConstProp 的红利）
  AlgebraicSimp   ; 代数/强度削弱
} until (!changed || iter >= MAX_ITER)
```

- **为什么这个顺序**：ConstProp 制造可消除的死代码 → DCE 跟进；ConstProp 还把条件变常量 → CFS 折叠分支；CSE/CFS 又可能暴露新的 ConstProp 机会 → 故外层循环。
- *怎么调*：加新 pass 时插进 repeat 循环；若某 pass 依赖 SSA，确保在 mem2reg 之后。

---

## 10. 寄存器分配归属（⚠️ 待协调）

- 总设计 §10 流水线把"寄存器分配"列为优化第 7 步；但 §7 与分工第 4 人把图染色归 codegen。
- **本 spec 立场**：寄存器分配（图染色 + deSSA + spill）属 codegen。中端止于 SSA IR。
- 理由：寄存器分配与目标机寄存器集合、调用约定、栈帧布局强耦合，属后端职责；SSA IR 是干净的交接面。
- **行动项**：与 codegen 同学确认；若双方同意寄存器分配归中端，则本 spec 需扩展 §7 增补分配章节。

---

## 11. AST 遍历方式

> ✅ **已确认**：前端已采用 **Visitor 模式**（`ASTVisitor` + `walk_comp_unit/stmt/expr/block`，见 `include/toyc/ast_visitor.h`）。本节关闭，仅留方案对比备查。

三种方案：

| 方案 | 形式 | 优 | 劣 |
|------|------|-----|-----|
| **A. Visitor 模式**（推荐） | 节点 `accept(V&)`，IRGen 实现 `Visitor` | 类型安全；结构/动作解耦；加新遍历不改节点 | 节点类要写 accept 样板 |
| B. `std::variant`+`visit` | `using Node = variant<...>` | 值语义、无指针 | AST 递归（Stmt→Block→Stmt）需 box 包装，variant 处理自递归麻烦 |
| C. 继承+`dyn_cast` if-else | 基类 + 子类 + `if(auto*p=dyn_cast<X>)` | 无样板、直接 | if-else 长链；加节点类型要改所有遍历点 |

- **推荐 A**：IRGen、sema、未来任何 AST 消费者共用 Visitor，最干净。
- *怎么调*：若团队求快上手、AST 节点不多，C 也可接受；但一旦有 ≥3 个消费者，A 的解耦优势显现。

---

## 12. 测试与验证（最大杠杆）

沿用总设计 §11 的 Oracle 对照法：

1. **Oracle 对照（金标准）**：我们的 `.s` → 汇编链接运行 → 退出码；`gcc` 编译同一 `.tc` → 退出码；逐用例比对。
   - 功能正确性的最强保证，对 mem2reg / 优化 pass 的语义正确性尤其关键。
2. **阶段 dump**：`-dump-ir` 输出各阶段 IR（IRGen 后 / mem2reg 后 / 各 pass 后），人眼/脚本对照 SSA 形态、phi 正确性。
3. **pass 单元测试**：每个 pass 构造小 IR（直接 new 指令拼装），断言输出；不依赖前端。
4. **回归集**：`test/regression/` 覆盖短路求值、嵌套作用域、全局/局部常量、递归、循环、break/continue。
5. **降级对照**：同一用例分别跑"开 mem2reg+优化"与"全关"，退出码必须一致（证明优化保持语义）。

---

## 13. 降级与风险对策

| 风险 | 对策 |
|------|------|
| mem2reg 支配树/重命名出错 → 语义破坏 | `-dump-ir` + Oracle；保留"跳过 mem2reg"开关，退回栈式 IR（§5） |
| 优化 pass 破坏语义（CSE 误合、DCE 误删） | 每个 pass 上 Oracle；`-opt` 默认关，功能测试不优化 |
| 接口未对齐（AST / SSA IR / 分配归属） | §3、§10、§11 标注待协调，M3 启动前开会钉死 |
| 全局变量初值含运行期表达式 | 假设"初值常量可求值"（§5.6）；若助教否决，需 IRGen 产 init 代码（扩展点） |
| pass 不收敛 | `MAX_ITER=10` 上限（§8.1） |

---

## 14. 待协调 / 开放问题清单

1. ⚠️ AST 接口 5 条需求（§3.1 R1–R5）—— 与前端/sema。
2. ⚠️ SSA IR 文本格式细节（如 `store` 操作数序、phi 语法）—— 与 codegen。
3. ⚠️ 寄存器分配 / deSSA 归属（§10）—— 与 codegen。
4. ⚠️ AST 遍历方式（§11，推荐 Visitor）—— 与前端/sema。
5. ⚠️ 全局变量初值是否允许含运行期表达式（§5.6）—— 与助教（总设计 §15 开放问题 3）。
6. main 的 epilogue 形式（总设计 §8.4）—— 属 codegen，但 `ret` 的 IR 形式需与 epilogue 对齐。

---

## 15. 里程碑对照（总设计 §12）

- 本 spec 服务于 **M3（IR + 优化）**：引入线性 SSA IR、代码生成迁到 IR、上核心优化。
- M1/M2 阶段四人合力打通管线时，本模块的 IRGen 可先以"非 SSA 栈式 IR"参与（§5 已是此形态），mem2reg 与优化 pass 在 M3 补齐。
- 完成判据：Oracle 对照回归集全过；开/关优化退出码一致。
