# evmone 合约间调用设计笔记

> 基于 evmone v0.21.0 源码分析，C++20，Apache 2.0 许可证。

---

## 目录

- [1. 调用链全景](#1-调用链全景)
- [2. 指令层 — call_impl 详解](#2-指令层--call_impl-详解)
  - [2.1 CALL 变体对比](#21-call-变体对比)
  - [2.2 执行步骤](#22-执行步骤)
- [3. EVMC 接口层 — C vtable 桥接](#3-evmc-接口层--c-vtable-桥接)
- [4. 宿主层 — Host::call() 详解](#4-宿主层--hostcall-详解)
  - [4.1 prepare_message()](#41-prepare_message)
  - [4.2 execute_message() — 核心分发](#42-execute_message--核心分发)
- [5. EIP-7702 委托机制](#5-eip-7702-委托机制)
- [6. CREATE/CREATE2 差异](#6-createcreate2-差异)
- [7. 预编译合约分发](#7-预编译合约分发)
- [8. Advanced 解释器的 gas 校正](#8-advanced-解释器的-gas-校正)
- [9. 状态回滚机制](#9-状态回滚机制)
- [10. 递归调用中的 ExecutionState 隔离](#10-递归调用中的-executionstate-隔离)
- [11. 关键设计亮点](#11-关键设计亮点)

---

## 1. 调用链全景

```
EVM 字节码执行 CALL 指令
  │
  ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  指令层: call_impl<OP_CALL>()                                           │
│  文件: instructions_calls.cpp                                           │
│  · 弹出 7 个栈参数                                                      │
│  · EIP-2929 冷访问检查                                                  │
│  · EIP-7702 委托解析                                                    │
│  · 内存检查 + 扩展                                                      │
│  · 构造 evmc_message                                                    │
│  · 63/64 gas 规则                                                       │
│  · depth 上限检查                                                       │
│  · 调用 state.host.call(msg)                                            │
└─────────────────────────────────────────────────────────────────────────┘
  │
  │ state.host.call(msg)
  ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  EVMC 接口层: evmc_host_interface.call                                  │
│  文件: evmc/include/evmc/evmc.hpp                                       │
│  · C vtable 函数指针                                                    │
│  · C++ → C 桥接                                                         │
└─────────────────────────────────────────────────────────────────────────┘
  │
  │ host_interface->call(ctx, &msg)
  ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  宿主层: Host::call()                                                   │
│  文件: test/state/host.cpp                                              │
│  · prepare_message() — nonce 检查, CREATE 地址计算                      │
│  · state.checkpoint() — 状态快照 (用于回滚)                             │
│  · execute_message() — 核心分发                                         │
│  · 失败时 state.rollback() — 回滚所有变更                               │
└─────────────────────────────────────────────────────────────────────────┘
  │
  │ execute_message()
  ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  分发层: Host::execute_message()                                        │
│  · CREATE/CREATE2 → create()                                            │
│  · CALL → 转账 + touch                                                  │
│  · 预编译 → call_precompile()                                           │
│  · 空代码 → 直接返回 SUCCESS                                            │
│  · 普通合约 → m_vm.execute(*this, rev, msg, code, len)                  │
│                    ↓                                                    │
│              递归进入 evmone 解释器                                      │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 指令层 — call_impl 详解

文件：`instructions_calls.cpp:64-185`

### 2.1 CALL 变体对比

```cpp
template <Opcode Op>  // OP_CALL | OP_CALLCODE | OP_DELEGATECALL | OP_STATICCALL
Result call_impl(StackTop stack, int64_t gas_left, ExecutionState& state) noexcept;
```

| | CALL | CALLCODE | DELEGATECALL | STATICCALL |
|---|---|---|---|---|
| **kind** | EVMC_CALL | EVMC_CALLCODE | EVMC_DELEGATECALL | EVMC_CALL |
| **recipient** | dst | parent.recipient | parent.recipient | dst |
| **sender** | parent.recipient | parent.recipient | parent.sender | parent.recipient |
| **value** | stack value | stack value | parent.value | 0 |
| **flags** | parent | parent | parent | + EVMC_STATIC |
| **has_value_arg** | 是 | 是 | 否 | 否 |

### 2.2 执行步骤

#### ① 弹出 7 个参数

```
gas_limit  = stack.pop()     // 用户指定的子调用 gas 上限
dst        = stack.pop()     // 目标合约地址
value      = stack.pop()     // 转账金额 (CALL/CALLCODE)
in_off     = stack.pop()     // input 在 memory 中的偏移
in_size    = stack.pop()     // input 大小
out_off    = stack.pop()     // output 写入 memory 的偏移
out_size   = stack.pop()     // 期望 output 大小

stack.push(0)                // 先假设失败
state.return_data.clear()
```

#### ② EIP-2929 冷访问检查 (Berlin+)

```cpp
if (rev >= EVMC_BERLIN) {
    access_status = state.host.access_account(dst);
    if (access_status == EVMC_ACCESS_COLD)
        gas_left -= 2500;  // additional_cold_account_access_cost
}
```

数据来源：`host.access_account()` → 宿主维护的 `accessed_addresses` 集合。

#### ③ EIP-7702 委托解析 (Prague+)

```cpp
const auto target_addr_or_result = get_target_address(dst, gas_left, state);
// 如果有委托: 返回委托地址, 并扣除冷访问 gas
// 如果无委托: 返回原始 dst
```

数据来源：`host.copy_code()` → 读取目标合约代码前 3 字节。

#### ④ 内存检查

```cpp
check_memory(gas_left, memory, input_offset, input_size);   // 可能触发 grow()
check_memory(gas_left, memory, output_offset, output_size);  // 同上
```

#### ⑤ 构造 evmc_message

```cpp
msg.kind       = EVMC_CALL;
msg.depth      = state.msg->depth + 1;     // ★ 深度 +1
msg.recipient  = dst;                       // 目标地址
msg.code_address = code_addr;               // 可能是委托地址
msg.sender     = state.msg->recipient;      // ★ Caller 的地址
msg.value      = be::store(value);
msg.input_data = &memory[input_offset];     // ★ 指向父的 memory
msg.input_size = input_size;
msg.flags      = state.msg->flags;          // 继承父 flags
```

#### ⑥ 价值转账 + 账户创建 gas (仅 CALL/CALLCODE)

```cpp
if (value != 0) {
    if (state.in_static_mode())
        return EVMC_STATIC_MODE_VIOLATION;
    gas_left -= 9000;   // CALL_VALUE_COST
    if (!state.host.account_exists(dst))
        gas_left -= 25000;  // ACCOUNT_CREATION_COST
}
```

#### ⑦ 63/64 规则 (EIP-150)

```cpp
msg.gas = min(gas_limit, gas_left - gas_left / 64);
```

确保父调用在子调用返回后仍有 gas 继续执行：

```
gas_left = 145000
子调用最多 = 145000 × 63/64 = 142735
父保留     = 145000 × 1/64  = 2265
用户指定 gas_limit = 50000, 实际得 50000
```

#### ⑧ Gas 补贴 (Stipend) — 仅 value-bearing CALL

```cpp
if (value != 0) {
    msg.gas += 2300;       // 给子调用的免费 gas
    gas_left += 2300;      // 父也补回
    if (balance < value)
        return EVMC_SUCCESS;  // "轻失败" (不 revert, stack=0)
}
```

#### ⑨ depth 检查

```cpp
if (state.msg->depth >= 1024)
    return EVMC_SUCCESS;  // "轻失败", 防止栈溢出
```

#### ⑩ 执行子调用

```cpp
const auto result = state.host.call(msg);
// → host_interface->call(ctx, &msg)  // EVMC C vtable
// → Host::call(msg)                  // 宿主实现
// → 递归进入新的 vm->execute()       // 新的解释器实例
```

#### ⑪ 处理返回结果

```cpp
state.return_data.assign(result.output_data, result.output_size);
stack.top() = (result.status_code == EVMC_SUCCESS) ? 1 : 0;

copy_size = min(output_size, result.output_size);
memcpy(&memory[output_offset], result.output_data, copy_size);

gas_used = msg.gas - result.gas_left;
gas_left -= gas_used;
state.gas_refund += result.gas_refund;

return {EVMC_SUCCESS, gas_left};
```

---

## 3. EVMC 接口层 — C vtable 桥接

文件：`evmc/include/evmc/evmc.hpp`

```cpp
// C++ → C 桥接函数
inline evmc_result call(evmc_host_context* h, const evmc_message* msg) noexcept
{
    return Host::from_context(h)->call(*msg).release_raw();
}

// 注册到 C vtable
inline const evmc_host_interface& Host::get_interface() noexcept
{
    static constexpr evmc_host_interface interface = {
        ...
        ::evmc::internal::call,    // call 函数指针
        ...
    };
    return interface;
}
```

数据流：

```
EVM 指令 (C++)
  → state.host.call(msg)              // C++ HostContext 方法
    → host_interface->call(ctx, msg)  // C 函数指针调用
      → evmc::internal::call(h, msg) // C→C++ 桥接
        → Host::call(*msg)           // 宿主实现
```

---

## 4. 宿主层 — Host::call() 详解

文件：`test/state/host.cpp:375-401`

```cpp
evmc::Result Host::call(const evmc_message& orig_msg) noexcept
{
    // ① 准备消息 (nonce 检查, CREATE 地址计算)
    const auto msg = prepare_message(orig_msg);
    if (!msg.has_value())
        return evmc::Result{EVMC_FAILURE, orig_msg.gas};

    // ② 状态快照 (用于失败时回滚)
    const auto logs_checkpoint = m_logs.size();
    const auto state_checkpoint = m_state.checkpoint();

    // ③ 执行
    auto result = execute_message(*msg);

    // ④ 失败时回滚
    if (result.status_code != EVMC_SUCCESS)
    {
        m_state.rollback(state_checkpoint);
        m_logs.resize(logs_checkpoint);

        // 0x03 quirk: ecrecover precompile 的 touch 永不回滚
        if (is_03_touched && m_rev >= EVMC_SPURIOUS_DRAGON)
            m_state.touch(addr_03);
    }
    return result;
}
```

### 4.1 prepare_message()

```cpp
std::optional<evmc_message> Host::prepare_message(evmc_message msg) noexcept
{
    if (msg.depth == 0 || msg.kind == EVMC_CREATE || msg.kind == EVMC_CREATE2)
    {
        auto& sender_acc = m_state.get(msg.sender);
        if (sender_acc.nonce == Account::NonceMax)
            return {};  // EIP-2681: nonce 溢出 → 轻失败

        if (msg.depth != 0)
        {
            m_state.journal_bump_nonce(msg.sender);
            ++sender_acc.nonce;  // CREATE 时 sender nonce +1
        }

        if (msg.kind == EVMC_CREATE || msg.kind == EVMC_CREATE2)
        {
            // 计算创建地址
            if (msg.kind == EVMC_CREATE)
                msg.recipient = compute_create_address(msg.sender, nonce);
            else
                msg.recipient = compute_create2_address(msg.sender, salt, initcode);

            access_account(msg.recipient);  // EIP-2929: 永不回滚的热访问
        }
    }
    return msg;
}
```

对于普通 CALL (depth > 0)：基本是 noop，直接返回原始消息。

### 4.2 execute_message() — 核心分发

```cpp
evmc::Result Host::execute_message(const evmc_message& msg) noexcept
{
    // ① CREATE/CREATE2 → create()
    if (msg.kind == EVMC_CREATE || msg.kind == EVMC_CREATE2)
        return create(msg);

    // ② CALL: 账户创建日志
    if (msg.kind == EVMC_CALL)
    {
        const auto exists = m_state.find(msg.recipient) != nullptr;
        if (!exists)
            m_state.journal_create(msg.recipient, exists);
    }

    // ③ CALL: 价值转账
    if (msg.kind == EVMC_CALL)
    {
        if (evmc::is_zero(msg.value))
            m_state.touch(msg.recipient);  // EIP-161: dust 清理
        else
        {
            m_state.journal_balance_change(msg.sender, ...);
            m_state.journal_balance_change(msg.recipient, ...);
            m_state.get(msg.sender).balance -= value;
            dst_acc.balance += value;
        }
    }

    // ④ 预编译检查
    if ((msg.flags & EVMC_DELEGATED) == 0 && is_precompile(m_rev, msg.code_address))
        return call_precompile(m_rev, msg);

    // ⑤ 空代码检查
    const auto code = m_state.get_code(msg.code_address);
    if (code.empty())
        return evmc::Result{EVMC_SUCCESS, msg.gas};

    // ⑥ 普通合约: 递归调用 evmone
    return m_vm.execute(*this, m_rev, msg, code.data(), code.size());
}
```

分发流程：

```
execute_message(msg):
    │
    ├─ kind == CREATE/CREATE2?
    │   └─ YES → create(msg)
    │
    ├─ kind == CALL?
    │   ├─ recipient 不存在? → journal_create()
    │   └─ value == 0? → touch(recipient)
    │      value > 0?  → 转账 (sender -= value, recipient += value)
    │
    ├─ 预编译地址? (且非 EIP-7702 委托)
    │   └─ YES → call_precompile(rev, msg)
    │
    ├─ 代码为空?
    │   └─ YES → return {SUCCESS, gas}
    │
    └─ 普通合约
        └─ m_vm.execute(*this, rev, msg, code, len)
             ↓
           递归进入 baseline::execute() 或 advanced::execute()
```

---

## 5. EIP-7702 委托机制

文件：`delegation.hpp`, `delegation.cpp`

EIP-7702 允许 EOA 账户委托执行到合约代码。EOA 账户代码前缀：`0xef0100` + 20 字节委托地址。

### 5.1 检测委托

```cpp
constexpr uint8_t DELEGATION_MAGIC_BYTES[] = {0xef, 0x01, 0x00};

constexpr bool is_code_delegated(bytes_view code) noexcept {
    return code.starts_with(DELEGATION_MAGIC);
}
```

### 5.2 获取委托地址

```cpp
std::optional<evmc::address> get_delegate_address(
    const evmc::HostInterface& host, const evmc::address& addr) noexcept
{
    // 读取目标代码前 23 字节 (3 magic + 20 address)
    uint8_t buffer[23];
    const auto size = host.copy_code(addr, 0, buffer, 23);
    const bytes_view designation{buffer, size};

    if (!is_code_delegated(designation))
        return {};  // 无委托

    // 提取 20 字节委托地址
    evmc::address delegate_address;
    std::ranges::copy(designation.substr(3), delegate_address.bytes);
    return delegate_address;
}
```

### 5.3 在 call_impl 中的应用

```cpp
inline std::variant<evmc::address, Result> get_target_address(
    const evmc::address& addr, int64_t& gas_left, ExecutionState& state) noexcept
{
    if (state.rev < EVMC_PRAGUE)
        return addr;  // 非 Prague 跳过

    const auto delegate_addr = get_delegate_address(state.host, addr);
    if (!delegate_addr)
        return addr;  // 无委托

    // 有委托: 扣除冷访问 gas
    const auto cost = (state.host.access_account(*delegate_addr) == EVMC_ACCESS_COLD)
        ? cold_account_access_cost : warm_storage_read_cost;

    if ((gas_left -= cost) < 0)
        return Result{EVMC_OUT_OF_GAS, gas_left};

    return *delegate_addr;  // 返回委托地址
}
```

### 5.4 委托调用流程

```
CALL 0xEOA (有 EIP-7702 委托)
    │
    ▼
get_target_address(0xEOA)
    │
    ├─ host.copy_code(0xEOA, 0, buf, 23)
    │   → 返回 [0xef, 0x01, 0x00, 0xContract, ...]
    │
    ├─ is_code_delegated? → YES
    │
    ├─ 提取委托地址: 0xContract
    │
    ├─ gas_left -= access_cost(0xContract)
    │
    └─ return 0xContract
         │
         ▼
    msg.code_address = 0xContract  // 执行委托合约的代码
    msg.recipient    = 0xEOA       // 但 recipient 仍是 EOA
    msg.flags       |= EVMC_DELEGATED  // 标记为委托调用
```

---

## 6. CREATE/CREATE2 差异

文件：`instructions_calls.cpp:196-258`

```cpp
template <Opcode Op>  // OP_CREATE | OP_CREATE2
Result create_impl(StackTop stack, int64_t gas_left, ExecutionState& state) noexcept;
```

| | CALL | CREATE/CREATE2 |
|---|---|---|
| 栈参数 | 7 个 (gas, dst, value, in_off, in_size, out_off, out_size) | 3+1 个 (value, offset, size, [salt]) |
| msg.kind | EVMC_CALL | EVMC_CREATE / EVMC_CREATE2 |
| msg.sender | parent.recipient | parent.recipient |
| msg.depth | parent.depth + 1 | parent.depth + 1 |
| msg.recipient | dst (调用者指定) | Host 计算 (CREATE 地址) |
| msg.input | memory[offset:size] | init code |
| 返回值 | success (0/1) | 新合约地址 (失败则 0) |
| msg.gas | min(limit, 63/64 规则) | gas_left - gas_left/64 |
| 静态模式 | 允许 (无 value 时) | 禁止 (STATIC_MODE_VIOLATION) |
| 额外 gas | CALL_VALUE: 9000, ACCOUNT_CREATION: 25000 | init_code_word_cost: 2 (Shanghai+), +6 (CREATE2 hash), initcode_size_limit: 49152 |

---

## 7. 预编译合约分发

文件：`test/state/host.cpp` 中 `execute_message()` 的预编译检查

```cpp
if ((msg.flags & EVMC_DELEGATED) == 0 && is_precompile(m_rev, msg.code_address))
    return call_precompile(m_rev, msg);
```

预编译地址表：

| 地址 | 功能 | 引入修订版 |
|------|------|-----------|
| 0x01 | ecrecover | Frontier |
| 0x02 | SHA-256 | Frontier |
| 0x03 | RIPEMD-160 | Frontier |
| 0x04 | identity (数据拷贝) | Frontier |
| 0x05 | modexp | Byzantium |
| 0x06 | ecAdd (BN254) | Byzantium |
| 0x07 | ecMul (BN254) | Byzantium |
| 0x08 | ecPairing (BN254) | Byzantium |
| 0x09 | Blake2b | Istanbul |
| 0x0a | KZG point evaluation | Cancun |
| 0x0b | BLS12-381 G1ADD | Prague |
| 0x0c | BLS12-381 G1MUL | Prague |
| 0x0d | BLS12-381 G1MSM | Prague |
| 0x1000 | secp256r1 (P-256) | Osaka |

关键规则：

- EIP-7702 委托调用跳过预编译：`(msg.flags & EVMC_DELEGATED) == 0`
- 空代码直接返回 SUCCESS，不进入解释器

---

## 8. Advanced 解释器的 gas 校正

文件：`advanced_instructions.cpp:207-221`

Advanced 解释器在基本块入口一次性扣除 gas，但 CALL 需要知道精确的逐指令 `gas_left`。通过校正机制解决：

```cpp
template <Opcode Op>
const Instruction* op_call(const Instruction* instr, AdvancedExecutionState& state) noexcept
{
    // 校正: 还原到逐指令粒度的 gas_left
    const auto gas_left_correction = state.current_block_cost - instr->arg.number;
    state.gas_left += gas_left_correction;

    // 调用通用 call_impl (与 Baseline 共享)
    const auto status = instr::impl<Op>(state);
    if (status != EVMC_SUCCESS)
        return state.exit(status);

    // 反向校正: 恢复块级 gas 计量
    if ((state.gas_left -= gas_left_correction) < 0)
        return state.exit(EVMC_OUT_OF_GAS);

    return ++instr;
}
```

校正图示：

```
基本块: [PUSH, PUSH, PUSH, PUSH, PUSH, PUSH, PUSH, CALL]
         ──────────────── 块级 gas 已扣除 ────────────────
         current_block_cost = 7×3 + 0 = 21 (CALL base=0)

执行到 CALL 时:
  state.gas_left = 原始 gas_left - 21 (块级已扣)

  校正:
    instr->arg.number = CALL 在块内的 gas 累计 = 21
    correction = 21 - 21 = 0
    state.gas_left += 0  // 恰好等于逐指令 gas_left

  如果 CALL 在块中间 (假设 arg.number = 15):
    correction = 21 - 15 = 6
    state.gas_left += 6  // 还原到 CALL 执行前的精确 gas
```

---

## 9. 状态回滚机制

文件：`test/state/host.cpp` 中 `Host::call()` 的 checkpoint/rollback

```
Host::call():
    │
    ├─ state_checkpoint = m_state.checkpoint()
    │   logs_checkpoint  = m_logs.size()
    │
    ├─ execute_message(msg)
    │   │
    │   │  产生的状态变更:
    │   │  · 转账: sender.balance -= value, recipient.balance += value
    │   │  · SSTORE: storage[key] = new_value
    │   │  · LOG: m_logs.push_back(...)
    │   │  · CREATE: 新账户加入 state
    │   │
    │   │  所有操作通过 journal 记录:
    │   │  · journal_balance_change(addr, old_balance)
    │   │  · journal_storage_change(addr, key, old_value)
    │   │  · journal_create(addr, existed)
    │   │  · journal_bump_nonce(addr)
    │   │
    │   └─ result
    │
    ├─ if (result.status_code != EVMC_SUCCESS):
    │       m_state.rollback(state_checkpoint)  // 按 journal 反向恢复
    │       m_logs.resize(logs_checkpoint)      // 截断日志
    │
    │   例外: 0x03 (ecrecover) 的 touch 永不回滚 (Spurious Dragon+)
    │
    └─ return result
```

---

## 10. 递归调用中的 ExecutionState 隔离

```
VM::m_execution_states (vector, capacity=1025)

  depth=0 (Caller)                    depth=1 (Callee)
  ┌──────────────────────────┐        ┌──────────────────────────┐
  │ ExecutionState           │        │ ExecutionState           │
  │                          │        │                          │
  │ msg:                     │        │ msg:                     │
  │   sender=0xUser          │        │   sender=0xCaller        │
  │   depth=0                │        │   depth=1                │
  │   recipient=0xCaller     │        │   recipient=0xCallee     │
  │                          │        │                          │
  │ stack_space: [32KB]      │        │ stack_space: [32KB]      │
  │   独立的栈                │        │   独立的栈                │
  │                          │        │                          │
  │ memory: [4KB]            │        │ memory: [4KB]            │
  │   独立的内存              │        │   独立的内存              │
  │                          │        │                          │
  │ host: ───────────────────│───┬────│── host: 同一个            │
  │                          │   │    │                          │
  │ return_data: [...]       │   │    │ return_data: [...]       │
  │ gas_refund: N            │   │    │ gas_refund: M            │
  └──────────────────────────┘   │    └──────────────────────────┘
                                 │
                          共享同一个 host
                          (访问同一个链上状态)

  隔离:
    ✓ 栈完全独立
    ✓ 内存完全独立
    ✓ msg 完全独立
    ✓ return_data 完全独立

  共享:
    ✓ host → 同一个链上状态 (storage, balance, code)
    ✓ 子调用的 host.set_storage() 对父调用立即可见
    ✓ 子调用的 host.set_storage() 在失败时通过 journal 回滚
```

---

## 11. 关键设计亮点

| # | 亮点 | 说明 |
|---|------|------|
| 1 | **EVMC 接口解耦** | evmone 不知道宿主是谁，通过 C vtable 回调，Geth/Nethermind 各自实现 |
| 2 | **递归调用复用同一 VM 实例** | `m_vm.execute(*this, rev, msg, code, len)` 递归调用，`*this` 作为 host 传入 |
| 3 | **ExecutionState 按深度隔离** | 栈/内存/msg 完全独立，通过 host 共享链上状态 |
| 4 | **Checkpoint/Rollback** | journal 机制记录所有状态变更，失败时精确回滚 |
| 5 | **63/64 规则** | 确保父调用在子调用返回后仍有 gas 继续执行 |
| 6 | **Gas Stipend** | value-bearing CALL 自动补贴 2300 gas，避免简单转账因 gas 不足失败 |
| 7 | **EIP-7702 委托透明化** | `get_target_address()` 统一处理，对指令实现透明 |
| 8 | **Advanced gas 校正** | 块级 gas 计量与逐指令精确 gas 需求的桥接 |
| 9 | **预编译短路** | 空代码/预编译地址直接返回，不进入解释器 |
| 10 | **"轻失败"设计** | depth≥1024 / 余额不足不 revert，返回 0，保留已执行状态 |
| 11 | **shared_ptr 语义** | 0x03 (ecrecover) 的 touch 永不回滚，处理 Spurious Dragon 特殊情况 |
