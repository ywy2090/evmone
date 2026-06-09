# evmone 栈结构设计笔记

> 基于 evmone v0.21.0 源码分析，C++20，Apache 2.0 许可证。

---

## 目录

- [1. 三层架构总览](#1-三层架构总览)
- [2. StackSpace — 底层存储](#2-stackspace--底层存储)
- [3. StackTop — 轻量操作接口](#3-stacksop--轻量操作接口)
- [4. 指令操作栈的模式分类](#4-指令操作栈的模式分类)
- [5. Baseline 解释器中的栈管理](#5-baseline-解释器中的栈管理)
- [6. 指令族实现详解](#6-指令族实现详解)
  - [6.1 算术指令 (弹N压1)](#61-算术指令-弹n压1)
  - [6.2 POP — 为什么是 noop](#62-pop--为什么是-noop)
  - [6.3 PUSH — 模板化字节码读取](#63-push--模板化字节码读取)
  - [6.4 DUP — 模板特化](#64-dup--模板特化)
  - [6.5 SWAP — fast_swap 优化](#65-swap--fast_swap-优化)
  - [6.6 DUPN/SWAPN/EXCHANGE — 变深度操作](#66-dupnswapnexchange--变深度操作)
- [7. Advanced 解释器中的栈管理](#7-advanced-解释器中的栈管理)
- [8. 跨调用栈隔离 (depth)](#8-跨调用栈隔离-depth)
- [9. 设计亮点总结](#9-设计亮点总结)

---

## 1. 三层架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│  层 1: StackSpace (底层存储)                                     │
│  · 固定容量 1024 项, 32 字节对齐, 堆分配                         │
│  · 生命周期 = ExecutionState 生命周期                            │
│  · 仅提供 bottom() 指针                                         │
├─────────────────────────────────────────────────────────────────┤
│  层 2: StackTop (操作接口)                                       │
│  · 轻量包装 uint256* m_end                                      │
│  · 提供 push/pop/top/operator[] 操作                            │
│  · 编译期 assume_aligned<32>                                    │
├─────────────────────────────────────────────────────────────────┤
│  层 3: 指令实现 (instr::core)                                    │
│  · 通过 StackTop 参数操作栈                                     │
│  · 不关心底层存储布局                                           │
│  · 假设栈检查已由解释器完成                                     │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. StackSpace — 底层存储

文件：`execution_state.hpp:30-51`

```cpp
class StackSpace {
    struct Storage {
        static constexpr auto limit = 1024;                    // EVM 栈上限
        static constexpr auto alignment = sizeof(uint256);     // 32 字节对齐
        alignas(alignment) uint256 items[limit];               // 连续数组
    };

    std::unique_ptr<Storage> m_stack_space = std::make_unique<Storage>();

public:
    static constexpr auto limit = Storage::limit;
    uint256* bottom() noexcept { return &m_stack_space->items[0]; }
};
```

### 内存布局

```
堆上分配 (std::make_unique<Storage>()):

  items[0]    items[1]    items[2]    ...    items[1023]
  ┌──────────┬──────────┬──────────┬─────┬──────────┐
  │ uint256  │ uint256  │ uint256  │     │ uint256  │
  │ 32 bytes │ 32 bytes │ 32 bytes │     │ 32 bytes │
  └──────────┴──────────┴──────────┴─────┴──────────┘
  ↑bottom()

  · alignas(32) 保证每个 uint256 不跨缓存行
  · 32 字节 = 256 位 = EVM 字长
  · 总大小: 1024 × 32 = 32,768 字节 (32 KB)
```

### 为什么堆分配

32KB 如果放在宿主栈上，加上递归调用 (depth 最大 1024)，可能导致宿主栈溢出。堆分配 + `unique_ptr` 自动管理生命周期。

### 为什么用 unique_ptr 而非直接成员

`Storage` 体积大 (32KB)，作为类直接成员会导致 `ExecutionState` 体积膨胀，影响对象池中多个实例的缓存行为。`unique_ptr` 只占 8 字节。

---

## 3. StackTop — 轻量操作接口

文件：`instructions.hpp:18-44`

```cpp
class StackTop {
    uint256* m_end;  // 指向栈顶的下一个位置 (即栈顶+1)

public:
    explicit(false) StackTop(uint256* end) noexcept
      : m_end{std::assume_aligned<sizeof(uint256)>(end)}  // 编译期对齐提示
    {}

    uint256* end() noexcept { return m_end; }

    // 索引访问: 0=栈顶, 1=次顶, 2=第三项...
    uint256& operator[](int index) noexcept { return m_end[-1 - index]; }

    // 栈顶引用
    uint256& top() noexcept { return m_end[-1]; }

    // 弹出: m_end 前移, 返回原栈顶引用
    uint256& pop() noexcept { return *--m_end; }

    // 压入: 写入 m_end 位置, m_end 后移
    void push(const uint256& value) noexcept { *m_end++ = value; }
};
```

### 指针语义

```
EVM 栈 (增长方向 →):

  items[0]  items[1]  items[2]  items[3]  items[4]  items[5] ...
  ┌────────┬────────┬────────┬────────┬────────┬────────┬────
  │  val0  │  val1  │  val2  │  val3  │ (空)   │ (空)   │
  └────────┴────────┴────────┴────────┴────────┴────────┴────
                             ↑                  ↑
                           top()              m_end
                           m_end[-1]          (栈顶+1)
                           stack[0]

  operator[] 索引:
    stack[0] = m_end[-1] = items[3] = val3  (栈顶)
    stack[1] = m_end[-2] = items[2] = val2  (次顶)
    stack[2] = m_end[-3] = items[1] = val1  (第三项)
    stack[3] = m_end[-4] = items[0] = val0  (第四项)
```

### 各操作详解

**push(value) — 压入**

```
实现: *m_end++ = value  (先写后移)

执行前:                          执行后:
┌────┬────┬────┬────┬────┐      ┌────┬────┬────┬────┬────┬────┐
│ v0 │ v1 │ v2 │ v3 │    │      │ v0 │ v1 │ v2 │ v3 │ 42 │    │
└────┴────┴────┴────┴────┘      └────┴────┴────┴────┴────┴────┘
                      ↑m_end                     ↑m_end (新位置)

栈高度: +1
```

**pop() — 弹出**

```
实现: return *--m_end  (先移后读, 返回引用而非值)

执行前:                          执行后:
┌────┬────┬────┬────┬────┐      ┌────┬────┬────┬────┐
│ v0 │ v1 │ v2 │ v3 │    │      │ v0 │ v1 │ v2 │ v3 │
└────┴────┴────┴────┴────┘      └────┴────┴────┴────┘
                      ↑m_end               ↑m_end (新位置)

★ 返回引用避免 32 字节 uint256 拷贝
★ v3 的内存位置仍然有效, 调用方可按需读取
栈高度: -1
```

**top() — 栈顶引用**

```
实现: return m_end[-1]  (不移动指针)

┌────┬────┬────┬────┬────┐
│ v0 │ v1 │ v2 │ v3 │    │
└────┴────┴────┴────┴────┘
                ↑    ↑
              top() m_end
              m_end[-1]
```

**operator[](int index) — 索引访问**

```
实现: return m_end[-1 - index]

stack[0] = m_end[-1] = 栈顶
stack[1] = m_end[-2] = 次顶
stack[2] = m_end[-3] = 第三项
...
```

**end() — 栈顶+1 指针**

```
实现: return m_end

用途:
  栈高度 = stack.end() - state.stack_space.bottom()
  溢出检查 = stack.end() == bottom + 1024
  下溢检查 = stack.end() <= bottom + required - 1
```

### 设计决策总结

| 决策 | 原因 |
|------|------|
| `m_end` 指向栈顶+1 而非栈顶 | `push` 可以先写后移 (`*m_end++ = val`)，单条指令完成 |
| `pop()` 返回引用而非值 | 避免 32 字节拷贝，调用方按需读取 |
| `operator[](int)` 而非 `size_t` | 需要负数索引，且 `int` 足够 (栈深 ≤ 1024) |
| `assume_aligned<32>` | 编译期对齐提示，启用对齐加载优化 |
| `explicit(false)` | 允许隐式从 `uint256*` 构造，减少调用处噪音 |

---

## 4. 指令操作栈的模式分类

### 模式 A: 弹 N 压 1 (净变化 1-N)

典型指令：ADD, MUL, SUB, DIV, LT, GT, EQ, AND, OR, XOR, BYTE, SHL, SHR

```
ADD 示例:
  void add(StackTop stack) noexcept {
      stack.top() += stack.pop();
  }

  执行过程:
  ┌────┬────┬────┬────┐
  │ a  │ b  │ c  │ d  │
  └────┴────┴────┴────┘
              ↑top ↑m_end

  stack.top() → 引用 d (m_end[-1])
  stack.pop() → --m_end, 返回引用 c (*m_end)
  d += c      → 原 d 位置写入 c+d

  ┌────┬────┬────┐
  │ a  │ b  │c+d │
  └────┴────┴────┘
            ↑m_end

  净变化: -1 (弹2压1)
  m_end 只前移 1 次 (pop), top() 不移
```

### 模式 B: 弹 N 压 0 (纯消费, 净变化 -N)

典型指令：POP (-1), JUMP (-1), JUMPI (-2), MSTORE (-2), SSTORE (-2)

### 模式 C: 弹 0 压 1 (纯产出, 净变化 +1)

典型指令：PUSH0, PUSH1-PUSH32, CALLER, CALLVALUE, ADDRESS, DUP1-DUP16

### 模式 D: 弹 1 压 1 (净变化 0, 内容改变)

典型指令：ISZERO, NOT, CALLDATALOAD, MLOAD, SLOAD, SWAP1-SWAP16

---

## 5. Baseline 解释器中的栈管理

### Position 结构

```cpp
struct Position {
    code_iterator code_it;  // 字节码位置
    uint256* stack_end;     // 栈顶指针 (等价于 StackTop.m_end)
};
```

Baseline 不使用 `StackTop` 类，而是直接操作裸指针 `uint256* stack_end`，减少一层间接。

### 初始化

```cpp
const auto stack_bottom = state.stack_space.bottom();  // items[0] 地址
Position position{code, stack_bottom};                  // 初始: 空栈 (stack_end == bottom)
```

### 指令执行后栈调整

```cpp
template <Opcode Op>
Position invoke(const CostTable& cost_table, const uint256* stack_bottom,
    Position pos, int64_t& gas, ExecutionState& state) noexcept
{
    // ① 检查 (编译期模板)
    check_requirements<Op>(cost_table, gas, pos.stack_end, stack_bottom);

    // ② 执行指令
    const auto new_pos = invoke(instr::core::impl<Op>, pos, gas, state);

    // ③ 调整栈指针 (编译期常量, 零运行时开销)
    const auto new_stack_top = pos.stack_end + instr::traits[Op].stack_height_change;

    return {new_pos, new_stack_top};
}
```

### stack_height_change 编译期查表

| 指令 | stack_height_change | 效果 |
|------|---------------------|------|
| PUSH1 | +1 | stack_end++ |
| DUP1 | +1 | stack_end++ |
| ADD | -1 | stack_end-- (弹2压1) |
| MSTORE | -2 | stack_end -= 2 |
| SWAP1 | 0 | 不变 |
| CALLDATALOAD | 0 | 弹1压1 (净0) |
| POP | -1 | stack_end-- |
| STOP | 0 | 终止, 不调整 |

★ 零运行时查表: `stack_height_change` 是 `constexpr`, 编译器直接内联常量。

### 栈溢出/下溢检查

```cpp
template <Opcode Op>
evmc_status_code check_requirements(..., const uint256* stack_top,
    const uint256* stack_bottom) noexcept
{
    // 溢出检查 (对于产生值的指令)
    if constexpr (instr::traits[Op].stack_height_change > 0) {
        if (stack_top == stack_bottom + 1024)
            return EVMC_STACK_OVERFLOW;
    }

    // 下溢检查 (对于消费值的指令)
    if constexpr (instr::traits[Op].stack_height_required > 0) {
        constexpr auto min_offset = instr::traits[Op].stack_height_required - 1;
        if (stack_top <= stack_bottom + min_offset)
            return EVMC_STACK_UNDERFLOW;
    }
}
```

检查图示：

```
stack_bottom                                    stack_bottom + 1024
│                                                │
▼                                                ▼
┌────┬────┬────┬────┬────┬──────────────────────────────────┐
│ v0 │ v1 │ v2 │ v3 │    │          (空闲)                  │
└────┴────┴────┴────┴────┴──────────────────────────────────┘
                    ↑
                 stack_top (stack_end)

溢出: stack_top == bottom + 1024  (栈满, 无法 push)
下溢: stack_top <= bottom + N - 1 (不足 N 个值)
      例如 ADD 需要 2 个值: stack_top <= bottom + 1

★ 用 <= 而非 <: stack_top 指向栈顶+1, 所以 stack_top - bottom = 元素数量
```

---

## 6. 指令族实现详解

### 6.1 算术指令 (弹N压1)

```cpp
// ADD: 弹2压1, 净变化 -1
inline void add(StackTop stack) noexcept {
    stack.top() += stack.pop();
}

// MUL: 弹2压1
inline void mul(StackTop stack) noexcept {
    stack.top() *= stack.pop();
}

// SUB: 弹2压1 (注意操作数顺序)
inline void sub(StackTop stack) noexcept {
    stack[1] = stack[0] - stack[1];
    // stack[0] = 栈顶, stack[1] = 次顶
    // 结果写入 stack[1] (次顶位置), m_end 不变
}

// DIV: 弹2压1, 除零返回 0
inline void div(StackTop stack) noexcept {
    auto& v = stack[1];
    v = v != 0 ? stack[0] / v : 0;
}
```

### 6.2 POP — 为什么是 noop

```cpp
inline void noop(StackTop /*stack*/) noexcept {}
inline constexpr auto pop = noop;
```

POP 不需要任何操作，因为栈高度调整由解释器循环通过 `stack_height_change` 完成：

```cpp
// baseline_execution.cpp — 解释器循环
new_stack_top = pos.stack_end + instr::traits[OP_POP].stack_height_change;
//                                  OP_POP: stack_height_change = -1
// 所以 new_stack_top = stack_end - 1
// 效果等同于 pop, 但零指令开销
```

### 6.3 PUSH — 模板化字节码读取

```cpp
template <size_t Len>
inline code_iterator push(StackTop stack, ExecutionState&, code_iterator pos) noexcept {
    using word_type = uint256::word_type;
    constexpr auto NUM_FULL_WORDS = Len / sizeof(word_type);       // 完整 64-bit word 数
    constexpr auto NUM_PARTIAL_BYTES = Len % sizeof(word_type);    // 剩余字节数

    stack.push({});           // 先压入零值, m_end++
    auto& r = stack.top();    // 引用刚压入的槽位
    pos += 1;                 // 跳过 opcode 字节

    // 加载部分 word (1-7 字节)
    if constexpr (NUM_PARTIAL_BYTES != 0) {
        r[NUM_FULL_WORDS] = load_partial_push_data<NUM_PARTIAL_BYTES>(pos);
        pos += NUM_PARTIAL_BYTES;
    }

    // 加载完整 word (每 8 字节)
    if constexpr (NUM_FULL_WORDS != 0) {
        for (size_t i = 0; i < NUM_FULL_WORDS; ++i) {
            r[NUM_FULL_WORDS - 1 - i] = intx::be::unsafe::load<uint64_t>(pos);
            pos += 8;
        }
    }

    return pos;
}
```

PUSH4 (0x6057a371) 执行过程：

```
字节码:  [63] [60 57 a3 71] [下一个 opcode]
          ↑    ↑
        PUSH4  立即数

Len = 4, NUM_FULL_WORDS = 0, NUM_PARTIAL_BYTES = 4

  stack.push({})  →  压入 uint256{0}, m_end++
  load_partial_push_data<4>(pos) → 0x6057a371
  存入 r[0] (最低的 64-bit word)

结果: r = { 0x6057a371, 0, 0, 0 }
返回 pos + 5 (opcode + 4 字节立即数)
```

PUSH32 的 word 加载顺序：

```
PUSH32 立即数: [b0 b1 ... b7] [b8 ... b15] [b16 ... b23] [b24 ... b31]
                  word 3          word 2        word 1         word 0

r[3] = load(b0..b7)    // 最高 word
r[2] = load(b8..b15)
r[1] = load(b16..b23)
r[0] = load(b24..b31)   // 最低 word

uint256 内存布局 (little-endian words, big-endian bytes within word):
┌──────────┬──────────┬──────────┬──────────┐
│  r[0]    │  r[1]    │  r[2]    │  r[3]    │
│ 最低 word │          │          │ 最高 word │
└──────────┴──────────┴──────────┴──────────┘
```

### 6.4 DUP — 模板特化

```cpp
template <int N>
inline void dup(StackTop stack) noexcept {
    static_assert(N >= 1 && N <= 16);
    stack.push(stack[N - 1]);  // 复制第 N 项到栈顶
}
```

DUP3 执行过程：

```
执行前:  stack = [..., v0, v1, v2]
                           [2] [1] [0]

stack[2] = v0  (第 3 项, 索引从 0 开始)
stack.push(v0) → *m_end = v0; m_end++

执行后:  stack = [..., v0, v1, v2, v0]
```

### 6.5 SWAP — fast_swap 优化

```cpp
template <int N>
inline void swap(StackTop stack) noexcept {
    static_assert(N >= 1 && N <= 16);
    fast_swap(stack.top(), stack[N]);
}

constexpr void fast_swap(uint256& x, uint256& y) noexcept {
    // 绕过 clang 优化 bug #59116
    auto t0 = x[0]; auto t1 = x[1]; auto t2 = x[2]; auto t3 = x[3];
    x = y;
    y[0] = t0; y[1] = t1; y[2] = t2; y[3] = t3;
}
```

为什么不用 `std::swap`：

```
std::swap(x, y):                    fast_swap(x, y):
  auto temp = x;  // 拷贝 32B         t0=x[0]; t1=x[1]; // 读 2×8B
  x = y;          // 拷贝 32B         t2=x[2]; t3=x[3]; // 读 2×8B
  y = temp;       // 拷贝 32B         x = y;             // 写 32B
  总计: 96B 移动                     y[0]=t0; y[1]=t1;  // 写 2×8B
                                     y[2]=t2; y[3]=t3;  // 写 2×8B

★ 关键: 避免 clang 优化 bug #59116
★ word 级操作对编译器更友好, 生成更优代码
```

SWAP1 执行过程：

```
执行前:                          执行后:
┌────┬────┬────┐                ┌────┬────┬────┐
│ a  │ b  │    │                │ b  │ a  │    │
└────┴────┴────┘                └────┴────┴────┘
   [1]  [0]                        [1]  [0]
   ↑top ↑m_end                     ↑top ↑m_end

m_end 不变, 栈高度: 0
```

### 6.6 DUPN/SWAPN/EXCHANGE — 变深度操作

EVM Osaka/Amsterdam 引入的新指令，突破 DUP1-16/SWAP1-16 的深度限制。

**与 DUP1-16 / SWAP1-16 的对比**：

| | DUP3 / SWAP1 | DUPN / SWAPN / EXCHANGE |
|---|---|---|
| 深度范围 | 1-16 (编译期) | 17-235 (运行时) |
| 立即数 | 无 | 1 字节 |
| 深度来源 | 模板参数 N | 字节码立即数解码 |
| 栈检查 | check_requirements | 指令内部运行时检查 |
| 代码位置增量 | pos + 1 | pos + 2 |

**立即数解码**：

```cpp
// DUPN/SWAPN: 立即数 → 栈深度 n ∈ [17, 235]
constexpr std::optional<int> decode_dupn_swapn_imm(uint8_t imm) noexcept {
    if (imm >= 0x5b && imm <= 0x7f)  // 禁止范围 (与 JUMPDEST 冲突)
        return std::nullopt;
    return static_cast<uint8_t>(imm + 0x91);
}

// EXCHANGE: 立即数 → (n, m) 对, 1 ≤ n < m, n+m ≤ 30
constexpr std::optional<std::pair<int, int>> decode_exchange_imm(uint8_t imm) noexcept {
    if (imm >= 0x52 && imm <= 0x7f)
        return std::nullopt;
    const auto k = imm ^ 0x8f;
    const auto q = k / 16, r = k % 16;
    return (q < r) ? std::pair{q + 1, r + 1} : std::pair{r + 1, 29 - q};
}
```

**DUPN 实现**：

```cpp
inline code_iterator dupn(StackTop stack, ExecutionState& state, code_iterator pos) noexcept {
    const auto n = decode_dupn_swapn_imm(pos[1]);
    if (!n) { state.status = EVMC_UNDEFINED_INSTRUCTION; return nullptr; }

    // 运行时栈下溢检查 (静态检查无法覆盖变深度)
    const auto stack_size = stack.end() - state.stack_space.bottom();
    if (*n > stack_size) { state.status = EVMC_STACK_UNDERFLOW; return nullptr; }

    stack.push(stack[*n - 1]);
    return pos + 2;
}
```

**EXCHANGE 语义**：

```
执行前:  [..., S[m], ..., S[n], ..., top]
执行后:  [..., S[n], ..., S[m], ..., top]

用途: 编译器生成代码中重排栈上变量
```

---

## 7. Advanced 解释器中的栈管理

Advanced 解释器不使用 `Position` 结构，而是通过 `ExecutionState` 直接管理栈。

**基本块级栈检查**：

```cpp
// opx_beginblock: 在基本块入口一次性检查整个块的栈需求
void opx_beginblock(Instruction* instr, ExecutionState& state) {
    const auto& block = instr->arg.block;

    // 块级栈检查 (整块只需要一次)
    if (state.stack_size() < block.stack_req)
        return state.exit(EVMC_STACK_UNDERFLOW);
    if (state.stack_size() + block.stack_max_growth > StackSpace::limit)
        return state.exit(EVMC_STACK_OVERFLOW);

    // 块级 gas 扣除 (整块只需要一次)
    state.gas_left -= block.gas_cost;
    if (state.gas_left < 0)
        return state.exit(EVMC_OUT_OF_GAS);
}
```

与 Baseline 的对比：

| | Baseline | Advanced |
|---|---|---|
| 栈检查频率 | 每条指令 | 每个基本块 |
| 栈调整 | `pos.stack_end + stack_height_change` | `state.adjust_stack_size(change)` |
| 检查位置 | `check_requirements<Op>()` | `opx_beginblock()` |

---

## 8. 跨调用栈隔离 (depth)

### depth 是什么

`depth` 是 `evmc_message` 中的字段，表示当前合约调用的嵌套层数。

```
外部交易 (depth=0)
  └→ 合约 A 调用 合约 B (depth=1)
       └→ 合约 B 调用 合约 C (depth=2)
            └→ ...最大 depth=1024
```

### depth 的来源

- 外部交易：以太坊客户端设置 `msg.depth = 0`
- CALL/CREATE 指令：evmone 设置 `msg.depth = parent.depth + 1`

```cpp
// instructions_calls.cpp:113
msg.depth = state.msg->depth + 1;
```

### depth 上限检查 (1024)

```cpp
// instructions_calls.cpp:171-172
if (state.msg->depth >= 1024)
    return {EVMC_SUCCESS, gas_left};  // "轻失败" — 不 revert, 但返回 0
```

1024 限制的原因：每个 depth 消耗约 36KB (StackSpace 32KB + Memory 4KB)，1024 × 36KB ≈ 36MB，防止恶意递归 DoS。

### ExecutionState 按 depth 隔离

```cpp
// vm.cpp:88-97
ExecutionState& VM::get_execution_state(size_t depth) noexcept {
    if (m_execution_states.size() <= depth)
        m_execution_states.resize(depth + 1);
    return m_execution_states[depth];
}
```

```
VM::m_execution_states (vector, capacity=1025)

  [0] ExecutionState (depth=0)       [1] ExecutionState (depth=1)
  ┌──────────────────────────┐       ┌──────────────────────────┐
  │ stack_space:             │       │ stack_space:             │
  │ ┌──────────────────────┐│       │ ┌──────────────────────┐│
  │ │ [0] [1] ... [1023]   ││       │ │ [0] [1] ... [1023]   ││
  │ │  ↑bottom  ↑stack_end ││       │ │  ↑bottom  ↑stack_end ││
  │ │  独立的 32KB 栈       ││       │ │  独立的 32KB 栈       ││
  │ └──────────────────────┘│       │ └──────────────────────┘│
  │ memory: [4KB 独立内存]  │       │ memory: [4KB 独立内存]  │
  │ msg: depth=0            │       │ msg: depth=1            │
  └──────────────────────────┘       └──────────────────────────┘

  ★ 每个 depth 拥有独立的栈和内存
  ★ 子调用修改自己的栈不影响父调用
  ★ 子调用通过 host.set_storage() 写入的链上状态对父调用立即可见
```

### "轻失败" vs REVERT

| | 轻失败 (depth≥1024) | REVERT |
|---|---|---|
| status | EVMC_SUCCESS | EVMC_REVERT |
| stack.top() | 0 (失败) | — |
| gas | 不额外消耗 | 剩余 gas 返还 |
| 状态变更 | 保留 | 回滚 |

---

## 9. 设计亮点总结

| # | 亮点 | 说明 |
|---|------|------|
| 1 | **堆分配 + unique_ptr** | 32KB 栈不在宿主栈上，避免 1024 层递归栈溢出；unique_ptr 仅 8 字节，对象池缓存友好 |
| 2 | **m_end 指向栈顶+1** | push: `*m_end++ = val`；pop: `return *--m_end`；都是单条指令 |
| 3 | **pop() 返回引用** | 避免 32 字节 uint256 拷贝；`add: stack.top() += stack.pop()` 零拷贝 |
| 4 | **assume_aligned<32>** | 编译期对齐提示，启用 SIMD 或对齐加载优化 |
| 5 | **POP 是 noop** | 栈高度调整由解释器通过 `stack_height_change` 完成，零指令开销 |
| 6 | **编译期栈检查** | `check_requirements<>` 是模板函数，`stack_height_change/required` 是 constexpr，编译器生成最优代码 |
| 7 | **fast_swap 绕过 clang bug** | word 级操作而非 `std::swap` 的字节级拷贝，避免 clang #59116 |
| 8 | **模板化指令族** | `dup<N>`, `swap<N>`, `push<Len>` 各一个模板，static_assert 约束范围，编译期特化 |
| 9 | **跨调用栈隔离** | 每个 depth 独立的 StackSpace + Memory；链上状态通过 host 共享，内存/栈完全隔离 |
| 10 | **基本块级栈检查 (Advanced)** | 将逐指令检查合并为逐块检查，减少热路径分支 |
