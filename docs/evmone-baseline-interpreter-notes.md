# evmone Baseline 解释器深度分析笔记

> 基于 evmone v0.21.0 源码分析，C++20，Apache 2.0 许可证。

---

## 目录

- [1. 项目概览](#1-项目概览)
- [2. lib/evmone/ 目录架构](#2-libevmone-目录架构)
- [3. 核心设计：两阶段指令执行](#3-核心设计两阶段指令执行)
- [4. 双解释器对比](#4-双解释器对比)
- [5. X-宏代码生成机制](#5-x-宏代码生成机制)
- [6. Baseline 解释器核心流程](#6-baseline-解释器核心流程)
  - [6.1 四阶段总览](#61-四阶段总览)
  - [6.2 VM 实例创建](#62-vm-实例创建)
  - [6.3 执行入口](#63-执行入口)
  - [6.4 代码分析 (JUMPDEST 位集)](#64-代码分析-jumpdest-位集)
  - [6.5 解释循环 (dispatch)](#65-解释循环-dispatch)
- [7. 执行时上下文 (ExecutionState)](#7-执行时上下文-executionstate)
- [8. 执行时数据来源](#8-执行时数据来源)
  - [8.1 evmc_message (调用参数)](#81-evmc_message-调用参数)
  - [8.2 evmc_host_interface (链上状态)](#82-evmc_host_interface-链上状态)
  - [8.3 CodeAnalysis (代码分析结果)](#83-codeanalysis-代码分析结果)
  - [8.4 CostTable (修订版 gas 表)](#84-costtable-修订版-gas-表)
  - [8.5 evmc_tx_context (懒加载)](#85-evmc_tx_context-懒加载)
  - [8.6 StackSpace (栈内数据)](#86-stackspace-栈内数据)
  - [8.7 Memory (内存)](#87-memory-内存)
- [9. 指令签名与 invoke 桥接](#9-指令签名与-invoke-桥接)
- [10. check_requirements 详解](#10-check_requirements-详解)
- [11. Gas 计费层次](#11-gas-计费层次)
- [12. 示例一：简单存储合约 store(42)](#12-示例一简单存储合约-store42)
- [13. 示例二：合约间调用 Caller → Callee](#13-示例二合约间调用-caller--callee)
  - [13.1 合约代码](#131-合约代码)
  - [13.2 Caller 执行流程 (depth=0)](#132-caller-执行流程-depth0)
  - [13.3 CALL 指令内部流程](#133-call-指令内部流程)
  - [13.4 Callee 执行流程 (depth=1)](#134-callee-执行流程-depth1)
  - [13.5 返回结果处理](#135-返回结果处理)
  - [13.6 ExecutionState 隔离](#136-executionstate-隔离)
  - [13.7 Gas 流转明细](#137-gas-流转明细)
- [14. 关键设计模式](#14-关键设计模式)
- [15. LRU Cache 使用情况](#15-lru-cache-使用情况)

---

## 1. 项目概览

**evmone** 是由 Ipsilon 团队维护的高性能以太坊虚拟机 (EVM) C++ 实现，通过 EVMC (Ethereum Client-VM Connector) API 作为独立 EVM 执行引擎，可被 Geth 等以太坊客户端集成。

**关键特性**：

- EVMC ABI 版本 12
- 双解释器：Baseline (默认) + Advanced
- 使用 `intx` 库实现 256 位整数运算
- 使用 `ethash` 库实现 Keccak256 哈希
- 支持 Frontier 到 Amsterdam (含 Experimental) 所有 EVM 修订版
- 实现全部预编译合约 (ecrecover, SHA-256, RIPEMD-160, BN254, BLS, KZG, Blake2b 等)

**依赖**：

| 库 | 版本 | 用途 |
|---|---|---|
| intx | 0.15.0 | 256 位整数运算 |
| ethash | (via Hunter) | Keccak256 哈希 |
| GTest | 1.17.0 | 单元测试 |
| benchmark | 1.9.5 | 性能基准 |
| CLI11 | 2.5.0 | 命令行解析 |
| nlohmann_json | 3.12.0 | JSON 解析 |

---

## 2. lib/evmone/ 目录架构

共 **27 个文件，约 156 KB 源码**，是 evmone 的核心 EVM 解释器实现。

```
lib/evmone/
├── 入口层
│   ├── vm.hpp / vm.cpp              — EVMC 接口，VM 实例，选项管理
│   └── constants.hpp                — MAX_CODE_SIZE (0x6000), MAX_INITCODE_SIZE
│
├── 运行时状态
│   └── execution_state.hpp          — ExecutionState / StackSpace / Memory
│
├── 指令系统
│   ├── instructions_opcodes.hpp     — Opcode 枚举 (256 个操作码)
│   ├── instructions_traits.hpp      — 编译期 gas 成本表 + 指令元数据
│   ├── instructions_xmacro.hpp      — MAP_OPCODES X-宏 (代码生成核心)
│   ├── instructions.hpp             — 所有核心指令实现 (35KB，最大文件)
│   ├── instructions_calls.cpp       — CALL/CREATE 系列指令
│   └── instructions_storage.cpp     — SLOAD/SSTORE 指令
│
├── Baseline 解释器
│   ├── baseline.hpp                 — CodeAnalysis, BitsetSpan 声明
│   ├── baseline_analysis.cpp        — JUMPDEST 位集分析 + 代码填充
│   ├── baseline_execution.cpp       — 主循环 (switch + computed goto)
│   └── baseline_instruction_table.* — 每修订版 gas 成本查找表
│
├── Advanced 解释器
│   ├── advanced_analysis.hpp/cpp    — 基本块分解，PUSH 值提取
│   ├── advanced_execution.hpp/cpp   — 指针追逐主循环
│   └── advanced_instructions.cpp    — 指令包装器 + OpTable 构建
│
├── EIP-7702 委托
│   └── delegation.hpp / delegation.cpp — 0xef0100 前缀检测与委托解析
│
├── 追踪 / 诊断
│   ├── tracing.hpp / tracing.cpp    — InstructionTracer / HistogramTracer / Counter
│   └── lru_cache.hpp                — O(1) LRU 缓存模板
│
└── 平台
    └── cpu_check.cpp                — x86-64 微架构级别运行时验证
```

---

## 3. 核心设计：两阶段指令执行

这是整个架构最关键的设计决策——**指令实现与 gas/栈检查解耦**：

```
┌─────────────────────────────────────────────────────────┐
│  解释器循环 (负责检查)                                      │
│  ├─ check_requirements<Op>()  ← Baseline: 每条指令检查     │
│  └─ opx_beginblock            ← Advanced: 每个基本块检查    │
├─────────────────────────────────────────────────────────┤
│  instr::core::impl<Op>        ← 共享的核心指令逻辑          │
│  (假设 gas 和栈已由上层处理)                                 │
└─────────────────────────────────────────────────────────┘
```

**好处**：同一套指令实现服务于两个解释器，各自有不同的开销模型。

---

## 4. 双解释器对比

| 维度 | Baseline | Advanced |
|------|----------|----------|
| **分析阶段** | 轻量：JUMPDEST 位集 + 33 字节填充 | 完整：基本块分解 + PUSH 值提取 |
| **分发机制** | switch / computed goto | 函数指针追逐 (`instr->fn(instr, state)`) |
| **gas 检查** | 每条指令 | 每个基本块 (单次扣除) |
| **栈检查** | 每条指令 | 每个基本块 |
| **PUSH 处理** | 运行时从字节码读取 | 分析阶段预提取内联 |
| **死代码** | 不处理 | 无条件终止符后裁剪 |
| **gas 校正** | 不需要 | CALL/SSTORE/GAS 需还原逐指令 gas |
| **DUPN/SWAPN/EXCHANGE** | 完整实现 | 标记为 undefined |

---

## 5. X-宏代码生成机制

`MAP_OPCODES` 是整个代码生成的骨架，定义在 `instructions_xmacro.hpp` 中：

```cpp
// 消费者重定义 ON_OPCODE_IDENTIFIER / ON_OPCODE_UNDEFINED 后展开
#define ON_OPCODE_IDENTIFIER(OPCODE, IDENTIFIER) ...
#define ON_OPCODE_UNDEFINED(BYTE) ...
MAP_OPCODES  // 展开 256 个 opcode 槽位
```

被三处使用：

1. **Baseline switch 语句** — 生成 `case OP_ADD: ...` 等
2. **Advanced 函数指针表** — 生成 `instruction_implementations[256]`
3. **`instr::core::impl<>` 模板特化** — 生成 `template<> constexpr auto impl<OP_ADD> = add;`

---

## 6. Baseline 解释器核心流程

### 6.1 四阶段总览

```
evmc_create_evmone()          // 1. 创建 VM 实例 (一次性)
        │
        ▼
execute(vm, host, ctx, ...)   // 2. 入口：准备执行环境
        │
        ▼
analyze(code)                 // 3. 代码分析：JUMPDEST 位集 + 填充
        │
        ▼
dispatch(cost_table, state,   // 4. 解释循环：逐条执行指令
         gas, code)
```

### 6.2 VM 实例创建

文件：`vm.cpp:74-86`

```cpp
VM::VM() noexcept
  : evmc_vm{
        EVMC_ABI_VERSION,
        "evmone", PROJECT_VERSION,
        evmone::destroy,
        evmone::baseline::execute,  // ← 默认执行函数
        evmone::get_capabilities,
        evmone::set_option,
    }
{
    m_execution_states.reserve(1025);  // 预分配 1025 个调用深度
}
```

`VM` 类继承 `evmc_vm`，持有：

- `m_execution_states`: 按调用深度索引的 `ExecutionState` 池
- `m_first_tracer`: 追踪器链表头
- `cgoto`: 是否使用 computed goto 分发

运行时可通过 `set_option()` 切换：

- `"advanced"` — 切换到 Advanced 解释器
- `"cgoto"` — 启用/禁用 computed goto
- `"trace"` — 指令追踪
- `"histogram"` — 操作码直方图

### 6.3 执行入口

文件：`baseline_execution.cpp:265-316`

有两个重载：

**EVMC 兼容签名** (外部入口)：

```cpp
evmc_result execute(evmc_vm* c_vm, const evmc_host_interface* host,
    evmc_host_context* ctx, evmc_revision rev, const evmc_message* msg,
    const uint8_t* code, size_t code_size) noexcept
{
    auto vm = static_cast<VM*>(c_vm);
    bytes_view container{code, code_size};
    const auto code_analysis = analyze(container);  // 代码分析
    return execute(*vm, *host, ctx, rev, *msg, code_analysis);
}
```

**内部签名** (预分析代码)：

```cpp
evmc_result execute(VM& vm, const evmc_host_interface& host,
    evmc_host_context* ctx, evmc_revision rev, const evmc_message& msg,
    const CodeAnalysis& analysis) noexcept
{
    const auto code = analysis.code();                // ① 获取填充后的代码
    auto gas = msg.gas;                                // ② 初始 gas

    auto& state = vm.get_execution_state(msg.depth);   // ③ 按深度获取状态
    state.reset(msg, rev, host, ctx, code);             // ④ 重置状态

    state.analysis.baseline = &analysis;                // ⑤ 关联代码分析

    const auto& cost_table = get_baseline_cost_table(rev); // ⑥ 获取 gas 表

    auto* tracer = vm.get_tracer();
    if (tracer != nullptr)
        gas = dispatch<true>(cost_table, state, gas, code, tracer);
    else if (vm.cgoto)
        gas = dispatch_cgoto(cost_table, state, gas, code);
    else
        gas = dispatch<false>(cost_table, state, gas, code);

    // ⑦ 构造返回结果
    const auto gas_left = (status == SUCCESS || REVERT) ? gas : 0;
    const auto gas_refund = (status == SUCCESS) ? state.gas_refund : 0;
    return evmc::make_result(status, gas_left, gas_refund, output_data, output_size);
}
```

### 6.4 代码分析 (JUMPDEST 位集)

文件：`baseline_analysis.cpp:35-60`

```
原始字节码 → analyze_legacy():

  ┌─────────────────────────────────────────────────────────┐
  │ 1. 分配 padded_code: code_size + 33 字节                │
  │    - 32 字节: PUSH32 可能越界的数据空间                   │
  │    - 1 字节:  STOP 终止保证                              │
  │                                                         │
  │ 2. 复制原始代码到 padded_code                            │
  │                                                         │
  │ 3. 在 padded_code 尾部对齐位置分配 JUMPDEST 位集          │
  │                                                         │
  │ 4. analyze_jumpdests():                                 │
  │    遍历字节码，遇到 PUSH1-PUSH32 跳过立即数，              │
  │    遇到 JUMPDEST 在位集中标记对应位                       │
  └─────────────────────────────────────────────────────────┘
```

**`analyze_jumpdests` 关键细节**：利用 `OP_PUSH32 == 0x7f == INT8_MAX` 的特性，用 `static_cast<int8_t>(op) >= OP_PUSH1` 一次判断是否为任意 PUSH 指令。

**位集结构** (`BitsetSpan`)：

```cpp
struct BitsetSpan {
    using word_type = uint64_t;
    word_type* m_array = nullptr;

    bool test(size_t index) const noexcept {
        auto [word, bit_mask] = get_ref(index);
        return (word & bit_mask) != 0;
    }
    void set(size_t index) noexcept {
        auto& [word, bit_mask] = get_ref(index);
        word |= bit_mask;
    }
};
```

### 6.5 解释循环 (dispatch)

文件：`baseline_execution.cpp:164-261`

三种分发模式：

| 模式 | 条件 | 机制 |
|------|------|------|
| `dispatch<false>` | 默认 (无 tracer, 非 cgoto) | `switch(op)` 生成跳转表 |
| `dispatch<true>` | 有 tracer | 同上，每条指令前调用 `tracer->notify_instruction_start()` |
| `dispatch_cgoto` | GCC/Clang, `vm.cgoto==true` | `goto* cgoto_table[op]` 直接跳转 |

**switch 分发核心**：

```cpp
template <bool TracingEnabled>
int64_t dispatch(const CostTable& cost_table, ExecutionState& state,
    int64_t gas, const uint8_t* code, Tracer* tracer = nullptr) noexcept
{
    const auto stack_bottom = state.stack_space.bottom();
    Position position{code, stack_bottom};

    while (true)  // 由 padded_code 末尾的 STOP 保证终止
    {
        if constexpr (TracingEnabled) { /* 追踪 */ }

        const auto op = *position.code_it;
        switch (op) {
            #define ON_OPCODE(OPCODE) \
                case OPCODE: \
                    if (const auto next = invoke<OPCODE>(...); \
                        next.code_it == nullptr) \
                        return gas; \
                    else \
                        position = next; \
                    break;
            MAP_OPCODES
            #undef ON_OPCODE
            default:
                state.status = EVMC_UNDEFINED_INSTRUCTION;
                return gas;
        }
    }
}
```

**computed goto 分发核心**：

```cpp
int64_t dispatch_cgoto(...) {
    static constexpr void* cgoto_table[] = {
        #define ON_OPCODE(OPCODE) &&TARGET_##OPCODE,
        #define ON_OPCODE_UNDEFINED(_) &&TARGET_OP_UNDEFINED,
        MAP_OPCODES
        ...
    };

    Position position{code, stack_bottom};
    goto* cgoto_table[*position.code_it];

    #define ON_OPCODE(OPCODE) \
        TARGET_##OPCODE: \
            if (const auto next = invoke<OPCODE>(...); \
                next.code_it == nullptr) \
                return gas; \
            else \
                position = next; \
            goto* cgoto_table[*position.code_it];
    MAP_OPCODES
    #undef ON_OPCODE
}
```

---

## 7. 执行时上下文 (ExecutionState)

文件：`execution_state.hpp`

```cpp
class ExecutionState {
public:
    // ════════════════════════ 来自 EVMC 宿主 ════════════════════════
    const evmc_message* msg;        // 调用消息
    evmc::HostContext host;          // 宿主接口
    evmc_revision rev;              // EVM 修订版

    // ════════════════════════ 运行时状态 ════════════════════════
    int64_t gas_refund;             // 累积 gas 退款
    Memory memory;                  // EVM 内存 (4KB 初始, 倍增)
    bytes return_data;              // 上一次调用的返回数据
    bytes_view original_code;       // 原始字节码引用
    evmc_status_code status;        // 执行状态码
    size_t output_offset;           // 输出数据在 memory 中的偏移
    size_t output_size;             // 输出数据大小

    // ════════════════════════ 懒加载 ════════════════════════
    evmc_tx_context m_tx;           // 交易上下文 (首次访问时获取)

    // ════════════════════════ 解释器专用 ════════════════════════
    union {
        const CodeAnalysis* baseline;
        const AdvancedCodeAnalysis* advanced;
    } analysis;

    // ════════════════════════ 栈空间 ════════════════════════
    StackSpace stack_space;         // 1024 个 uint256, 32 字节对齐
};
```

**各字段在执行流程中的角色**：

```
execute() 入口
    │
    ├─ msg.gas ──────────────→ 初始 gas
    ├─ msg.depth ────────────→ 索引 VM 的状态池
    ├─ msg.flags & EVMC_STATIC → in_static_mode()
    ├─ msg.sender ───────────→ CALLER 指令
    ├─ msg.value ────────────→ CALLVALUE 指令
    ├─ msg.input_data ───────→ CALLDATALOAD/CALLDATASIZE
    │
    ├─ host ─────────────────→ SLOAD/SSTORE/BALANCE/CALL 等
    ├─ rev ──────────────────→ 指令可用性、gas 成本、行为差异
    ├─ memory ───────────────→ MLOAD/MSTORE/KECCAK256 等
    ├─ stack_space ──────────→ 所有栈操作
    ├─ analysis.baseline ────→ JUMP/JUMPI 校验
    └─ m_tx (懒加载) ───────→ ORIGIN/GASPRICE/TIMESTAMP 等
```

**关键组件**：

**StackSpace**：固定 1024 个 `uint256` 项 (32KB)，32 字节对齐，堆分配避免宿主栈溢出。

**Memory**：`std::realloc`/`std::free`，初始 4KB，倍增策略，`grow()` 零填充新区域，扩展成本公式：`3 * words + words² / 512`。

**交易上下文懒加载**：`m_tx.block_timestamp == 0` 作为"未加载"哨兵，首次访问时通过 `host.get_tx_context()` 获取。

---

## 8. 执行时数据来源

### 8.1 evmc_message (调用参数)

由以太坊客户端直接传入，作为 `execute()` 参数。

```
evmc_message
├── gas          → 初始 gas 预算
├── depth        → 调用深度 (0=外部交易, 1+=合约调用)
├── sender       → CALLER 指令
├── recipient    → ADDRESS 指令
├── value        → CALLVALUE 指令
├── input_data   → CALLDATALOAD/CALLDATASIZE/CALLDATACOPY
├── flags        → EVMC_STATIC 标志 (静态模式)
├── create2_salt → CREATE2 的 salt
└── code_address → DELEGATECALL/STATICCALL 的实际代码地址
```

### 8.2 evmc_host_interface (链上状态)

通过 EVMC 宿主回调访问链上状态，是所有外部数据的桥梁。

```
host 接口
├── account_exists(address)  → EXTCODESIZE/EXTCODECOPY
├── get_storage(addr, key)   → SLOAD
├── set_storage(addr, key)   → SSTORE
├── get_balance(address)     → BALANCE
├── get_code_size(address)   → EXTCODESIZE
├── get_code_hash(address)   → EXTCODEHASH
├── copy_code(address, ...)  → EXTCODECOPY
├── get_tx_context()         → ORIGIN/GASPRICE/COINBASE/TIMESTAMP...
├── get_block_hash(number)   → BLOCKHASH
├── emit_log(addr, data...)  → LOG0-LOG4
├── call(msg)                → CALL/CALLCODE/DELEGATECALL/STATICCALL
├── create(msg)              → CREATE/CREATE2
└── access_account(address)  → EIP-2929 冷/热访问
```

### 8.3 CodeAnalysis (代码分析结果)

`analyze()` 分析字节码的产物。

```
CodeAnalysis
├── m_code (bytes_view)          → 执行代码 (指向 padded_code)
├── m_padded_code (unique_ptr)   → 填充后的代码副本 (代码 + 33字节填充)
└── m_jumpdest_bitset (BitsetSpan) → JUMPDEST 有效位图
```

### 8.4 CostTable (修订版 gas 表)

`baseline_instruction_table.cpp` 中 `constexpr` 构造。

```
get_baseline_cost_table(rev) → const int16_t[256]
├── [OP_ADD] = 3
├── [OP_MUL] = 5
├── [OP_SLOAD] = 根据 rev 变化
├── [OP_SSTORE] = 根据 rev 变化
├── [OP_UNDEFINED] = -1
└── ...
```

### 8.5 evmc_tx_context (懒加载)

首次访问时通过 `host.get_tx_context()` 获取并缓存。

```
evmc_tx_context
├── tx_gas_price     → GASPRICE 指令
├── tx_origin        → ORIGIN 指令
├── block_coinbase   → COINBASE 指令
├── block_number     → NUMBER 指令
├── block_timestamp  → TIMESTAMP 指令
├── block_gas_limit  → GASLIMIT 指令
├── block_prev_randao→ PREVRANDAO 指令 (post-Merge)
├── chain_id         → CHAINID 指令
└── block_base_fee   → BASEFEE 指令
```

### 8.6 StackSpace (栈内数据)

所有指令的输入输出都经过栈：

```cpp
push(val): *stack_end = val; stack_end++;
pop():     stack_end--; return *stack_end;
top():     return *(stack_end - 1);
[n]:       return *(stack_end - 1 - n);  // 0=栈顶, 1=次顶
```

### 8.7 Memory (内存)

按需扩展的临时数据存储：

- `MLOAD`/`MSTORE` — 直接读写
- `KECCAK256` — 读取 memory[offset:offset+size]
- `CALLDATACOPY`/`CODECOPY`/`RETURNDATACOPY` — 写入
- `CALL`/`CREATE` — 读取作为 input
- `RETURN`/`REVERT` — 读取作为 output
- `LOG0-LOG4` — 读取作为 log data

---

## 9. 指令签名与 invoke 桥接

指令实现有 5 种签名，`invoke()` 的 5 个重载负责适配：

| 签名 | 用途 | invoke 行为 |
|------|------|-------------|
| `void(StackTop)` | 简单栈操作 (ADD, MUL, POP, DUP, SWAP) | 直接调用，返回 code_it+1 |
| `Result(StackTop, int64_t, ExecutionState&)` | 消耗额外 gas (KECCAK, MLOAD, EXP) | 检查 status，更新 gas |
| `void(StackTop, ExecutionState&)` | 需要状态但不扣额外 gas (ADDRESS, CALLER) | 直接调用 |
| `code_iterator(StackTop, ExecutionState&, code_iterator)` | 控制流 (JUMP, JUMPI, PC, PUSH) | 返回新代码位置 |
| `TermResult(StackTop, int64_t, ExecutionState&)` | 终止操作 (STOP, RETURN, REVERT) | 总是返回 nullptr |

---

## 10. check_requirements 详解

文件：`baseline_execution.cpp:43-90`

```cpp
template <Opcode Op>
evmc_status_code check_requirements(const CostTable& cost_table,
    int64_t& gas_left, const uint256* stack_top, const uint256* stack_bottom) noexcept
{
    // 步骤 1: 获取 gas 成本
    //   has_const_gas_cost(Op)? → gas_costs[FRONTIER][Op] (编译期常量)
    //   否则 → cost_table[Op] (运行时查表), < 0 表示未定义

    // 步骤 2: 栈溢出检查 (对于产生值的指令)
    //   if (stack_top == stack_bottom + 1024) → STACK_OVERFLOW

    // 步骤 3: 栈下溢检查 (对于消费值的指令)
    //   if (stack_top <= stack_bottom + required - 1) → STACK_UNDERFLOW

    // 步骤 4: 扣除 gas
    //   gas_left -= gas_cost
    //   if (gas_left < 0) → OUT_OF_GAS

    return EVMC_SUCCESS;
}
```

---

## 11. Gas 计费层次

```
┌─ 基础 gas ──────────────────────────────────────┐
│  gas_costs[revision][opcode] 编译期查找表         │
│  Baseline: check_requirements<> 每条指令扣除      │
│  Advanced: opx_beginblock 每块扣除                │
├─ 动态 gas ──────────────────────────────────────┤
│  内存扩展: grow_memory()                          │
│  拷贝操作: 3 gas/32字节                           │
│  KECCAK256: 6 gas/字                             │
│  EXP: 10/50 gas/有效字节                          │
│  冷访问 (EIP-2929): 额外 2500/2000               │
│  SSTORE: 按 storage_status 的 9 种状态查表        │
│  CALL 带 value: 9000, CREATE: 25000              │
├─ Gas 退款 ──────────────────────────────────────┤
│  SSTORE: 最高 4800/15000                         │
│  SELFDESTRUCT: 24000 (London 前)                 │
│  仅 EVMC_SUCCESS 时生效                          │
├─ 1/64 规则 (EIP-150) ──────────────────────────┤
│  CALL/CREATE 子调用可用 gas = gas_left - gas/64   │
└─────────────────────────────────────────────────┘
```

---

## 12. 示例一：简单存储合约 store(42)

### 合约代码

```solidity
contract SimpleStorage {
    uint256 private storedValue;  // slot 0

    function store(uint256 value) public {
        storedValue = value;
    }
}
```

### 字节码 (简化)

```
PC   Opcode          说明
───  ──────────────  ─────────────────────────────
0x00 PUSH1 0x80      准备 MSTORE 的 value
0x02 PUSH1 0x40      memory offset (free memory pointer)
0x04 MSTORE          MSTORE(0x40, 0x80)
0x05 CALLVALUE       检查 msg.value
0x06 DUP1
0x07 ISZERO          value == 0?
0x08 PUSH1 0x0e      jumpdest 目标
0x0a JUMPI           if(value==0) goto 0x0e
0x0b PUSH1 0x00
0x0d REVERT          有 value 则 revert
0x0e JUMPDEST        ★ 有效跳转目标
0x0f POP
0x10 PUSH4 0x6057..  函数选择器
0x15 PUSH1 0xe0
0x17 SHR             calldata[0:4] >> 224
0x18 DUP1
0x19 PUSH4 0x6057..  store 的 selector
0x1e EQ              匹配?
0x1f PUSH1 0x27
0x21 JUMPI           if match goto 0x27
...
0x27 JUMPDEST        ★ store() 入口
0x28 PUSH1 0x2a
0x2a PUSH1 0x04      calldata offset
0x2c CALLDATALOAD    加载 calldata[4:36] → value (42)
0x2d PUSH1 0x00      storage slot 0
0x2f SSTORE          SSTORE(slot=0, value=42) ★
0x30 STOP            终止
```

### 执行追踪

```
PC   指令          base_gas  动态gas    gas_left   数据来源
───  ────────────  ────────  ────────  ─────────  ─────────────
0x00 PUSH1 0x80      3                  49997
0x02 PUSH1 0x40      3                  49994
0x04 MSTORE          3        +0        49991      memory
0x05 CALLVALUE       2                  49989      msg.value
0x06 DUP1            3                  49986
0x07 ISZERO          3                  49983
0x08 PUSH1 0x0e      3                  49980
0x0a JUMPI          10                  49970
0x0e JUMPDEST        1                  49969
0x0f POP             2                  49967
0x10 PUSH4          ...                 ...        calldata[0:4]
...  (函数选择器)    ...                 ...
0x27 JUMPDEST        1                  ~49940
0x2c CALLDATALOAD    3                  ~49931      msg.input_data
0x2f SSTORE          0      +2100*      ~47831      host.get_storage
0x30 STOP            0                  ~49900      host.set_storage

* SSTORE 动态 gas 取决于存储状态
```

### JUMPI 跳转验证流程

```
JUMPI 执行:
    stack = [..., condition, target]
          │
          ▼
    condition == 0?
     │         │
    YES        NO
     │         │
     ▼         ▼
    不跳转     check_jumpdest(target):
                │
                ▼
              target >= code.size()?
               │          │
              YES         NO
               │          │
               ▼          ▼
             return    jumpdest_bitset.test(target)?
             false      │          │
                       YES         NO
                        │          │
                        ▼          ▼
                      return     return
                      true       false
```

---

## 13. 示例二：合约间调用 Caller → Callee

### 13.1 合约代码

```solidity
// 合约 A: 调用者
contract Caller {
    uint256 public result;

    function doCall(address callee, uint256 x) public {
        (bool success, bytes memory data) = callee.call(
            abi.encodeWithSignature("compute(uint256)", x)
        );
        require(success, "call failed");
        result = abi.decode(data, (uint256));
    }
}

// 合约 B: 被调用者
contract Callee {
    uint256 public lastResult;

    function compute(uint256 x) public returns (uint256) {
        lastResult = x * 2 + 1;
        return lastResult;
    }
}
```

调用: `Caller.doCall(Callee地址, 21)`

### 13.2 Caller 执行流程 (depth=0)

```
初始: gas = 200000, depth = 0

┌─ Caller (depth=0) ─────────────────────────────────────────────┐
│                                                                 │
│  1. 函数选择器匹配 doCall(address, uint256)                     │
│  2. CALLDATALOAD ×2 加载 callee 地址 和 x=21                   │
│  3. MSTORE 构造 calldata: selector("compute(uint256)") + 21     │
│     memory[0x00..0x23] = compute 的 selector                    │
│     memory[0x24..0x43] = 21 (0x15)                              │
│  4. PUSH ×7 准备 CALL 参数:                                     │
│     栈: [gas=50000, callee_addr, 0, 0x00, 0x44, 0x44, 0x20]    │
│  5. 执行 CALL 指令 → 递归进入 Callee                            │
│  6. 返回后:                                                      │
│     · stack.top() = 1 (成功)                                    │
│     · memory[0x44..0x63] = 43 (返回值)                          │
│     · require(success) 通过                                     │
│     · abi.decode → 43                                           │
│     · SSTORE(result_slot, 43)                                   │
│  7. STOP                                                        │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 13.3 CALL 指令内部流程

文件：`instructions_calls.cpp:64-185`

```
call_impl<OP_CALL>(stack, gas_left, state):

  ① 弹出 7 个参数
     gas_limit=50000, dst=0xCallee, value=0,
     in_off=0, in_size=0x44, out_off=0x44, out_size=0x20
     stack.push(0)  // 假设失败
     state.return_data.clear()

  ② EIP-2929 冷访问检查
     host.access_account(0xCallee) → COLD
     gas_left -= 2500

  ③ EIP-7702 委托解析 (Prague+)
     get_target_address(0xCallee) → 无委托, 返回自身

  ④ 内存检查
     check_memory(input: 0x00..0x43) ✓
     check_memory(output: 0x44..0x63) ✓

  ⑤ 构造 evmc_message
     msg_1 = {
       .kind       = EVMC_CALL,
       .depth      = 0 + 1 = 1,          ★ 深度 +1
       .recipient  = 0xCallee,
       .sender     = 0xCaller,            ★ Caller 是 sender
       .value      = 0,
       .input_data = &memory[0x00],       ★ 指向 Caller 的 memory
       .input_size = 0x44,
       .flags      = 继承父 flags
     }

  ⑥ 63/64 规则 (EIP-150)
     msg.gas = min(50000, gas_left - gas_left/64) = 50000

  ⑦ depth 检查
     if (depth >= 1024) → 轻失败
     // depth=0, 通过

  ⑧ ★ 宿主调用 ★
     result = state.host.call(msg_1)
       → host_interface->call(ctx, &msg_1)
       → 客户端递归调用 vm->execute(vm, host, ctx, CANCUN, &msg_1, code_callee, code_size)

  ⑨ 处理返回结果
     state.return_data = result.output_data  // [43]
     stack.top() = 1                         // 成功
     memcpy(&memory[0x44], output, 32)       // 拷贝到父 memory
     gas_used = 50000 - result.gas_left
     gas_left -= gas_used
     state.gas_refund += result.gas_refund
```

**各 CALL 变体差异**：

| | CALL | DELEGATECALL | STATICCALL |
|---|---|---|---|
| recipient | dst | parent.recipient | dst |
| sender | parent.recipient | parent.sender | parent.recipient |
| value | stack value | parent.value | 0 |
| flags | parent | parent | + EVMC_STATIC |

### 13.4 Callee 执行流程 (depth=1)

```
┌─ Callee (depth=1) ─────────────────────────────────────────────┐
│                                                                 │
│  VM::get_execution_state(1) → m_execution_states[1]             │
│  state_1.reset(msg_1, CANCUN, host, ctx, code_callee)           │
│                                                                 │
│  ExecutionState[1] (完全独立于 depth=0):                         │
│  · msg_1: sender=0xCaller, recipient=0xCallee, depth=1          │
│  · memory_1: 全新的 4KB                                         │
│  · stack_1: 全新的 1024 项                                      │
│  · analysis_1: Callee 的字节码分析                               │
│  · host: 同一个 host (共享链上状态访问)                           │
│                                                                 │
│  执行过程:                                                       │
│  1. 函数选择器匹配 compute(uint256)                              │
│  2. CALLDATALOAD(4) → 21                                        │
│     数据来源: state.msg->input_data → Caller memory[4:36]        │
│  3. PUSH1 2, MUL, PUSH1 1, ADD → 21*2+1 = 43                   │
│  4. SSTORE(slot=0, 43)                                          │
│     → host.set_storage(Callee, 0, 43)  // 写入链上状态           │
│  5. MSTORE(0x00, 43), RETURN                                    │
│     → 返回 {status=SUCCESS, gas_left=27374, output=[43]}        │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 13.5 返回结果处理

```
Callee 返回 result_1 给 Caller 的 call_impl:

  result_1 = {
    status_code = EVMC_SUCCESS,
    gas_left    = 27374,
    gas_refund  = 0,
    output_data = [0x00...2B],  // 43
    output_size = 32
  }

  Caller 的 call_impl 处理:
  · state.return_data = [43]
  · stack.top() = 1  (成功)
  · memcpy(&memory[0x44], output, 32)  → memory[0x44..0x63] = 43
  · gas_used = 50000 - 27374 = 22626
  · gas_left -= 22626

  Caller 继续执行:
  · require(success) → ISZERO + JUMPI (成功跳过 REVERT)
  · abi.decode → MLOAD(0x44) → 43
  · SSTORE(result_slot, 43)
  · STOP
```

### 13.6 ExecutionState 隔离

```
VM::m_execution_states (vector, capacity=1025)

  [0] ExecutionState (Caller)        [1] ExecutionState (Callee)
  ┌─────────────────────────┐        ┌─────────────────────────┐
  │ msg ──→ msg_0           │        │ msg ──→ msg_1           │
  │   sender=0xUser         │        │   sender=0xCaller       │
  │   depth=0               │        │   depth=1               │
  │                         │        │                         │
  │ memory ──→ [4KB 区块 A] │        │ memory ──→ [4KB 区块 B] │
  │   独立的内存空间         │        │   独立的内存空间         │
  │                         │        │                         │
  │ stack ──→ [1024 项]     │        │ stack ──→ [1024 项]     │
  │   独立的栈空间           │        │   独立的栈空间           │
  │                         │        │                         │
  │ analysis → Caller 分析  │        │ analysis → Callee 分析  │
  │                         │        │                         │
  │ return_data [43]        │        │ return_data []          │
  └─────────────────────────┘        └─────────────────────────┘

  ★ 两个 ExecutionState 通过同一个 host 访问同一个链上状态
  ★ 但 memory、stack、msg、analysis 完全独立
  ★ 子调用对 storage 的修改 (host.set_storage) 对父调用立即可见
  ★ 子调用的 memory 不会影响父调用的 memory
```

### 13.7 Gas 流转明细

```
┌─ Caller (depth=0) ────────────────────────────────────────┐
│                                                            │
│  操作                                    gas 消耗  gas_left│
│  ────────────────────────────────────   ────────  ────────│
│  初始                                             200000  │
│  函数选择器匹配 + 参数加载                   ~5200  194800  │
│  MSTORE 构造 calldata                        ~100  194700  │
│  PUSH ×7 (CALL 参数)                         ~210  194490  │
│  CALL: EIP-2929 冷访问                      +2500  191990  │
│  CALL: 子调用 gas 结算 (50000-27374)       +22626  169364  │
│  require(success)                             ~50  169314  │
│  abi.decode → MLOAD                           ~10  169304  │
│  SSTORE(result, 43)                        ~22100  147204  │
│  STOP                                           0  147204  │
└────────────────────────────────────────────────────────────┘

┌─ Callee (depth=1) ────────────────────────────────────────┐
│                                                            │
│  操作                                    gas 消耗  gas_left│
│  ────────────────────────────────────   ────────  ────────│
│  初始                                              50000  │
│  函数选择器匹配 + CALLDATALOAD(4)            ~500   49500  │
│  PUSH1 2, MUL, PUSH1 1, ADD                    14   49486  │
│  SSTORE(slot=0, 43) 冷写入 0→非0            22100   27386  │
│  PUSH1, MSTORE, PUSH1, PUSH1, RETURN            12   27374  │
│                                                            │
│  实际消耗 = 50000 - 27374 = 22626                          │
└────────────────────────────────────────────────────────────┘
```

---

## 14. 关键设计模式

| 模式 | 应用 |
|------|------|
| **X-Macro** | `MAP_OPCODES` — 256 操作码的单一定义，多处展开 |
| **模板特化** | `instr::core::impl<Op>` — 编译期操作码→实现映射 |
| **策略模式** | 运行时切换 Baseline/Advanced 解释器 |
| **责任链** | Tracer 链式通知 |
| **对象池** | `ExecutionState` 按调用深度预分配复用 |
| **编译期表** | gas_costs、traits、OpTable 全部 constexpr |
| **两阶段执行** | 检查与实现解耦，同一套指令服务两种解释器 |
| **懒加载** | 交易上下文首次访问时才从 host 获取 |

---

## 15. LRU Cache 使用情况

`LRUCache` 定义在 `lru_cache.hpp` 中，是 O(1) 操作的固定容量 LRU 缓存。

**当前状态**：仅在测试和基准中使用，**未在生产代码中使用**。

| 类别 | 文件 |
|------|------|
| 单元测试 | `test/unittests/lru_cache_test.cpp` |
| 微基准 | `test/internal_benchmarks/lru_cache_bench.cpp` |

**基准测试中的类型实例化**：

```cpp
LRUCache<hash256, std::shared_ptr<char>>  // 暗示未来用途: EVM 状态缓存
```

**实现特点**：

- `unordered_map` + `std::list` 实现 O(1) get/put/evict
- 构造时 `reserve` 避免 rehash
- 满容量时用 `extract()` 节点 API 实现零分配驱逐
- `get()` 返回 `std::optional<Value>`（值拷贝，非引用）
