# 前端测试 Bug 记录与补丁

> 范围：Lexer / Parser / AST 开发与回归测试期间发现的问题。  
> 关联文档：[`frontend-协作说明.md`](./frontend-协作说明.md)、[`test/regression/parser/README.md`](../test/regression/parser/README.md)

---

## 总览

| ID | 模块 | 现象 | 严重度 | 状态 |
|----|------|------|--------|------|
| BUG-001 | Lexer | 关键字输出为 `IDENT("const")` 等 | 高 | 已修复 |
| BUG-002 | Parser | AST 中标识符名变成 `=`、函数名丢失 | 高 | 已修复 |
| BUG-003 | Lexer | Token 列号从 2 开始（首字符应为 1） | 低 | 已修复 |
| BUG-004 | Parser | `a + 1;` 类表达式语句解析不完整 | 中 | 已修复 |
| BUG-005 | Lexer | `error_message()` 可能返回悬垂引用 | 低 | 已修复 |
| BUG-006 | 诊断 | 未闭合注释后连续报 lex + parse 两条错 | — | 预期行为 |

---

## BUG-001：关键字被识别为 IDENT

### 现象

对 `const int N = 10;` 运行 `-dump-tokens` 或 `-dump-ast`，关键字全部显示为 `IDENT`：

```
IDENT("const")@1:2
IDENT("int")@1:8
...
```

`lookup_keyword("const")` 在独立测试中应返回 `KW_CONST`，但集成后始终为 `IDENT`。

### 根因

**C++ 函数实参求值顺序未指定**。在 `scan_identifier_or_keyword()` 中：

```cpp
// 错误写法
return make_token(lookup_keyword(lexeme), std::move(lexeme), start_line, start_column);
```

部分编译器/平台上会先执行 `std::move(lexeme)`，再调用 `lookup_keyword(lexeme)`，此时 `lexeme` 已为空，查表失败，退回 `IDENT`。

### 补丁

**文件**：`src/frontend/lexer.cpp`

```cpp
const auto token_type = lookup_keyword(lexeme);
return make_token(token_type, std::move(lexeme), start_line, start_column);
```

先完成关键字查表，再 `move` 词素。

### 验证

```powershell
build\Debug\toyc-compiler.exe -dump-tokens < test\sample.tc
# 首 token 应为 KW_CONST("const")@1:1
```

---

## BUG-002：`Parser::expect()` 返回悬垂引用

### 现象

修复 BUG-001 后，`-dump-ast` 仍异常：

```
GlobalConst(=) @1:13      # 应为 GlobalConst(N)
FuncDef int add(...)      # 曾出现函数名丢失：FuncDef int (())
VarDecl(=) @8:12          # 应为 VarDecl(x)
```

标识符节点的 `name` 字段被写成了**下一个** token 的内容（如 `=`）。

### 根因

`expect()` 曾返回 `const Token&`，且指向成员 `current_`：

```cpp
// 错误写法
const Token& Parser::expect(TokenType type, const char* message) {
    ...
    const Token& token = current_;
    advance();           // current_ 已被更新为下一个 token
    return token;        // 返回的引用实际指向「新」current_
}
```

调用方 `const Token name = expect(IDENT, ...)` 拷贝时，读到的已是 **advance 之后** 的 token。

### 补丁

**文件**：`include/toyc/parser.h`、`src/frontend/parser.cpp`

- 返回类型改为 **按值** `Token`
- 在 `advance()` **之前** 拷贝到局部变量

```cpp
Token Parser::expect(TokenType type, const char* message) {
    if (!check(type)) {
        report_error(current_, message);
        return current_;
    }
    Token token = current_;  // 先拷贝
    advance();
    return token;            // 再返回副本
}
```

### 验证

```powershell
build\Debug\toyc-compiler.exe -dump-ast < test\sample.tc
# 应出现 GlobalConst(N)、FuncDef int add(...)、VarDecl(x) 等
```

---

## BUG-003：SourceLoc 列号偏移 +1

### 现象

文件第一个 token `const` 的位置显示为 `@1:2`，正确应为 `@1:1`。

### 根因

Lexer 构造时 `column_` 初值为 `1`，构造函数内又调用 `advance()` 读取首字符并 `++column_`，导致第一个 token 开始时列号已是 2。

### 补丁

**文件**：`include/toyc/lexer.h`

```cpp
std::uint32_t column_ = 0;  // 原为 1
```

首字符读入后列号变为 1，与源码列对齐。

### 验证

`-dump-tokens` 首行第一个 token 的 column 为 1。

---

## BUG-004：以 IDENT 开头的表达式语句缺少乘除/加减层

### 现象

语句 `add(x, y) + 1;` 或 `a * 2;` 若以「先读 IDENT 再分支」路径解析，曾只接入 `rel/land/lor` 层，**漏掉** `mul/add` 层，导致 AST 结构错误（在引入该路径后的早期版本中出现）。

### 根因

赋值与表达式语句对 `IDENT` 做了前瞻分流：非 `=` 时手动构造 primary，但未像 `parse_expr()` 一样完整走优先级链。

### 补丁

**文件**：`src/frontend/parser.cpp`（`parse_stmt` 中 IDENT 分支）

```cpp
expr = parse_mul_expr_from_left(std::move(expr));
expr = parse_add_expr_from_left(std::move(expr));
expr = parse_rel_expr_from_left(std::move(expr));
expr = parse_land_expr_from_left(std::move(expr));
expr = parse_lor_expr_from_left(std::move(expr));
```

与 `parse_expr()` 的优先级链保持一致。

### 验证

可在 `test/regression/parser/valid/function_call.tc` 等用例上 `-dump-ast` 人工确认；回归集 14 用例 CTest 通过。

---

## BUG-005：`Lexer::error_message()` 悬垂引用

### 现象

MSVC/G++ 编译警告：

```
warning: returning reference to temporary [-Wreturn-local-addr]
return error_message_.value_or(kEmpty);
```

### 根因

`optional::value_or(kEmpty)` 在 `error_message_` 为空时返回 **临时副本**，函数返回 `const std::string&` 会绑定到即将销毁的临时对象。

### 补丁

**文件**：`src/frontend/lexer.cpp`

```cpp
const std::string& Lexer::error_message() const {
    static const std::string kEmpty;
    if (error_message_) {
        return *error_message_;
    }
    return kEmpty;
}
```

（后续统一诊断以 `DiagnosticEngine` 为主，该 API 保留兼容。）

---

## BUG-006：未闭合注释的「双错误」输出（非 Bug）

### 现象

`test/regression/parser/invalid/unclosed_comment.tc` 测试时 stderr：

```
error [lex] 5:1: unterminated block comment
error [parse] 5:1: expected '}'
```

### 说明

这是 **预期行为**，不是逻辑错误：

1. Lexer 在 `/*` 未闭合时报告词法错误；
2. Parser 在错误状态下继续消耗 token，至文件末尾仍缺 `}`，再报语法错误。

回归脚本只检查 **退出码非 0**，不要求单条诊断。若需「一词法错误即停、不再 parse」，属于体验优化，当前设计为 fail fast + 测试通过即可。

---

## 补丁后完整测试清单

每次合并前端改动后建议执行：

```powershell
cmake -S . -B build
cmake --build build --config Debug
cmake --build build --config Release

ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build -C Release --output-on-failure

powershell -ExecutionPolicy Bypass -File test/regression/parser/run_tests.ps1 `
  -Compiler build\Debug\toyc-compiler.exe

build\Debug\toyc-compiler.exe -dump-tokens < test\sample.tc
build\Debug\toyc-compiler.exe -dump-ast < test\sample.tc
```

最近一次完整测试结果：**Debug/Release 构建成功，CTest 1/1 通过，14 个 parser 回归用例全部 PASS**。

---

## 经验总结

1. **同一表达式里不要对同一变量既查表又 `std::move`**，拆成两步或先查后移。
2. **不要把指向成员变量的引用/指针在成员被更新后返回给调用方**；`expect()` 类 API 应返回按值副本。
3. **Lexer 行列号**：建议在「读入字符后再更新 column」，初值设 0 或 1 要与更新逻辑一致。
4. **Parser 多条解析路径**（赋值 vs 表达式语句）应复用同一套优先级函数（`*_from_left`），避免漏层。
5. **回归测试** 是捕获 BUG-001/BUG-002 类问题的最有效手段；无效用例目录用于锁定诊断与退出码。

---

## 变更文件索引

| Bug | 主要修改文件 |
|-----|----------------|
| BUG-001 | `src/frontend/lexer.cpp` |
| BUG-002 | `include/toyc/parser.h`, `src/frontend/parser.cpp` |
| BUG-003 | `include/toyc/lexer.h` |
| BUG-004 | `src/frontend/parser.cpp` |
| BUG-005 | `src/frontend/lexer.cpp` |
