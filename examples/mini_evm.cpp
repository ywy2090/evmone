// mini_evm.cpp — 参考 evmone Baseline 的精简 EVM 实现
//
// 设计目标: 用最少代码展示 EVM 核心原理，便于学习 evmone 架构
// 简化: uint256 → uint64_t, 无 EVMC, 无 EIP, 无预编译
//
// 编译: g++ -std=c++20 -O2 -o mini_evm mini_evm.cpp
// 运行: ./mini_evm

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
//  类型与常量
// ═══════════════════════════════════════════════════════════════════════════

using u64 = uint64_t;
using u8 = uint8_t;
using bytes = std::vector<u8>;

static constexpr size_t STACK_LIMIT = 1024;
static constexpr size_t MAX_CODE_SIZE = 0x6000;
static constexpr size_t CALL_DEPTH_LIMIT = 1024;

// ═══════════════════════════════════════════════════════════════════════════
//  操作码枚举 — 参考 instructions_opcodes.hpp
// ═══════════════════════════════════════════════════════════════════════════

enum Opcode : u8 {
    OP_STOP = 0x00,
    OP_ADD = 0x01,
    OP_MUL = 0x02,
    OP_SUB = 0x03,
    OP_DIV = 0x04,
    OP_MOD = 0x06,
    OP_ADDMOD = 0x08,
    OP_LT = 0x10,
    OP_GT = 0x11,
    OP_EQ = 0x14,
    OP_ISZERO = 0x15,
    OP_AND = 0x16,
    OP_OR = 0x17,
    OP_XOR = 0x18,
    OP_NOT = 0x19,
    OP_KECCAK256 = 0x20,
    OP_ADDRESS = 0x30,
    OP_CALLER = 0x33,
    OP_CALLVALUE = 0x34,
    OP_CALLDATALOAD = 0x35,
    OP_CALLDATASIZE = 0x36,
    OP_CALLDATACOPY = 0x37,
    OP_COINBASE = 0x41,
    OP_TIMESTAMP = 0x42,
    OP_NUMBER = 0x43,
    OP_POP = 0x50,
    OP_MLOAD = 0x51,
    OP_MSTORE = 0x52,
    OP_MSTORE8 = 0x53,
    OP_SLOAD = 0x54,
    OP_SSTORE = 0x55,
    OP_JUMP = 0x56,
    OP_JUMPI = 0x57,
    OP_PC = 0x58,
    OP_GAS = 0x5a,
    OP_JUMPDEST = 0x5b,
    OP_PUSH1 = 0x60,
    OP_PUSH2 = 0x61,
    OP_PUSH3 = 0x62,
    OP_PUSH4 = 0x63,
    OP_PUSH5 = 0x64,
    OP_PUSH6 = 0x65,
    OP_PUSH7 = 0x66,
    OP_PUSH8 = 0x67,
    OP_DUP1 = 0x80,
    OP_DUP2 = 0x81,
    OP_DUP3 = 0x82,
    OP_DUP4 = 0x83,
    OP_SWAP1 = 0x90,
    OP_SWAP2 = 0x91,
    OP_SWAP3 = 0x92,
    OP_LOG0 = 0xa0,
    OP_CREATE = 0xf0,
    OP_CALL = 0xf1,
    OP_RETURN = 0xf3,
    OP_REVERT = 0xfd,
};

// ═══════════════════════════════════════════════════════════════════════════
//  Gas 成本表 — 参考 instructions_traits.hpp
// ═══════════════════════════════════════════════════════════════════════════

// Gas 成本 — 参考 Frontier 修订版 (evmone gas_costs[EVMC_FRONTIER])
static constexpr int GAS_ZERO = 0;      // STOP, RETURN, REVERT, PC, GAS, ADDRESS, CALLER...
static constexpr int GAS_BASE = 2;      // ADD, SUB, LT, GT, EQ, ISZERO, AND, OR, XOR, NOT, POP
static constexpr int GAS_VERYLOW = 3;   // MUL, DIV, MOD, ADDMOD, PUSH, DUP, SWAP
static constexpr int GAS_LOW = 5;       // (未使用)
static constexpr int GAS_MID = 8;       // JUMP
static constexpr int GAS_HIGH = 10;     // JUMPI
static constexpr int GAS_SHA3 = 30;     // KECCAK256 base
static constexpr int GAS_SHA3_WORD = 6; // KECCAK256 per word
static constexpr int GAS_MEMORY = 3;    // MLOAD, MSTORE, MSTORE8
static constexpr int GAS_SLOAD = 200;   // SLOAD (Tangerine Whistle)
static constexpr int GAS_SSTORE = 5000; // SSTORE (Simplified)
static constexpr int GAS_JUMPDEST = 1;
static constexpr int GAS_CALL = 700;
static constexpr int GAS_CALL_VALUE = 9000;
static constexpr int GAS_CREATE = 32000;
static constexpr int GAS_LOG0 = 375;
static constexpr int GAS_LOG_DATA = 8;
static constexpr int GAS_CALL_STIPEND = 2300;
static constexpr int GAS_MEMORY_EXPANSION = 3;  // 每个新 word 的线性成本

// ═══════════════════════════════════════════════════════════════════════════
//  指令特征表 — 参考 instructions_traits.hpp
// ═══════════════════════════════════════════════════════════════════════════

struct InstrTraits {
    int gas_cost;             // 基础 gas 成本 (-1 = 未定义)
    int stack_required;       // 栈中需要的最小元素数
    int stack_change;         // 执行后栈高度变化 (+1, -1, -2 等)
};

static consteval std::array<InstrTraits, 256> make_traits()
{
    std::array<InstrTraits, 256> t{};
    t.fill({-1, 0, 0});  // 默认: 未定义

    // 算术
    t[OP_STOP]    = {GAS_ZERO, 0, 0};
    t[OP_ADD]     = {GAS_BASE, 2, -1};
    t[OP_MUL]     = {GAS_VERYLOW, 2, -1};
    t[OP_SUB]     = {GAS_BASE, 2, -1};
    t[OP_DIV]     = {GAS_VERYLOW, 2, -1};
    t[OP_MOD]     = {GAS_VERYLOW, 2, -1};
    t[OP_ADDMOD]  = {GAS_VERYLOW, 3, -2};

    // 比较
    t[OP_LT]      = {GAS_BASE, 2, -1};
    t[OP_GT]      = {GAS_BASE, 2, -1};
    t[OP_EQ]      = {GAS_BASE, 2, -1};
    t[OP_ISZERO]  = {GAS_BASE, 1, 0};

    // 位运算
    t[OP_AND]     = {GAS_BASE, 2, -1};
    t[OP_OR]      = {GAS_BASE, 2, -1};
    t[OP_XOR]     = {GAS_BASE, 2, -1};
    t[OP_NOT]     = {GAS_BASE, 1, 0};

    // KECCAK256
    t[OP_KECCAK256] = {GAS_SHA3, 2, -1};

    // 环境
    t[OP_ADDRESS]      = {GAS_ZERO, 0, 1};
    t[OP_CALLER]       = {GAS_ZERO, 0, 1};
    t[OP_CALLVALUE]    = {GAS_ZERO, 0, 1};
    t[OP_CALLDATALOAD] = {GAS_BASE, 1, 0};
    t[OP_CALLDATASIZE] = {GAS_ZERO, 0, 1};
    t[OP_CALLDATACOPY] = {GAS_BASE, 3, -3};

    // 区块
    t[OP_COINBASE]   = {GAS_ZERO, 0, 1};
    t[OP_TIMESTAMP]  = {GAS_ZERO, 0, 1};
    t[OP_NUMBER]     = {GAS_ZERO, 0, 1};

    // 栈/内存/存储
    t[OP_POP]      = {GAS_BASE, 1, -1};
    t[OP_MLOAD]    = {GAS_MEMORY, 1, 0};
    t[OP_MSTORE]   = {GAS_MEMORY, 2, -2};
    t[OP_MSTORE8]  = {GAS_MEMORY, 2, -2};
    t[OP_SLOAD]    = {GAS_SLOAD, 1, 0};
    t[OP_SSTORE]   = {GAS_SSTORE, 2, -2};

    // 控制流
    t[OP_JUMP]     = {GAS_MID, 1, -1};
    t[OP_JUMPI]    = {GAS_HIGH, 2, -2};
    t[OP_PC]       = {GAS_ZERO, 0, 1};
    t[OP_GAS]      = {GAS_ZERO, 0, 1};
    t[OP_JUMPDEST] = {GAS_JUMPDEST, 0, 0};

    // PUSH (1-8 字节立即数)
    for (int op = OP_PUSH1; op <= OP_PUSH8; ++op)
        t[op] = {GAS_VERYLOW, 0, 1};

    // DUP1-4
    for (int op = OP_DUP1; op <= OP_DUP4; ++op)
        t[op] = {GAS_VERYLOW, op - OP_DUP1, 1};

    // SWAP1-3
    for (int op = OP_SWAP1; op <= OP_SWAP3; ++op)
        t[op] = {GAS_VERYLOW, op - OP_SWAP1 + 1, 0};

    // LOG0
    t[OP_LOG0]    = {GAS_LOG0, 2, -2};

    // 系统
    t[OP_CREATE]  = {GAS_CREATE, 3, -2};
    t[OP_CALL]    = {GAS_CALL, 7, -6};
    t[OP_RETURN]  = {GAS_ZERO, 2, -2};
    t[OP_REVERT]  = {GAS_ZERO, 2, -2};

    return t;
}

static constexpr auto TRAITS = make_traits();

// 便捷访问
static constexpr auto GAS_TABLE = []() noexcept {
    std::array<int, 256> t{};
    for (int i = 0; i < 256; ++i)
        t[i] = TRAITS[i].gas_cost;
    return t;
}();

// ═══════════════════════════════════════════════════════════════════════════
//  Stack — 参考 StackSpace + StackTop
// ═══════════════════════════════════════════════════════════════════════════

struct Stack {
    u64 data[STACK_LIMIT];
    size_t size = 0;

    void push(u64 v) { data[size++] = v; }
    u64 pop() { return data[--size]; }
    u64& top() { return data[size - 1]; }
    u64& operator[](int i) { return data[size - 1 - i]; }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Memory — 参考 execution_state.hpp Memory 类
// ═══════════════════════════════════════════════════════════════════════════

struct Memory {
    bytes data;
    size_t active_size = 0;  // EVM 认为的大小

    Memory() { data.resize(4096, 0); }  // 初始 4KB

    // 扩展内存并扣除 gas (参考 Memory::grow + grow_memory)
    // 返回 false 表示 gas 不足
    bool expand(size_t new_size, u64& gas)
    {
        if (new_size <= active_size)
            return true;
        // 对齐到 32 字节
        new_size = (new_size + 31) & ~size_t{31};

        // 计算内存扩展 gas: 3*words + words²/512 (增量)
        size_t old_words = active_size / 32;
        size_t new_words = new_size / 32;
        u64 old_cost = 3 * old_words + old_words * old_words / 512;
        u64 new_cost = 3 * new_words + new_words * new_words / 512;
        u64 cost = new_cost - old_cost;

        if (gas < cost)
            return false;
        gas -= cost;

        if (new_size > data.size())
            data.resize(new_size * 2, 0);  // 倍增
        active_size = new_size;
        return true;
    }

    size_t words() const { return (active_size + 31) / 32; }

    u64 load_u64(size_t offset)
    {
        if (offset + 32 > data.size())
            return 0;
        // 读取最后 8 字节 (big-endian 256-bit: 前 24 字节为高位)
        u64 v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | data[offset + 24 + i];
        return v;
    }

    bool store_u64(size_t offset, u64 v, u64& gas)
    {
        if (!expand(offset + 32, gas))
            return false;
        // 清零整个 32 字节
        std::fill_n(data.data() + offset, 32, 0);
        // 将值存储到最后 8 字节 (big-endian 256-bit)
        for (int i = 7; i >= 0; --i)
        {
            data[offset + 24 + i] = static_cast<u8>(v);
            v >>= 8;
        }
        return true;
    }

    bool store8(size_t offset, u8 v, u64& gas)
    {
        if (!expand(offset + 1, gas))
            return false;
        data[offset] = v;
        return true;
    }

    const u8* ptr(size_t offset) const { return data.data() + offset; }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Host — 简化的宿主接口
// ═══════════════════════════════════════════════════════════════════════════

struct Host {
    // 每个地址的账户: code + storage + balance
    struct Account {
        bytes code;
        std::unordered_map<u64, u64> storage;
        u64 balance = 0;
    };

    std::unordered_map<u64, Account> accounts;
    u64 default_balance = 1000000;

    // 区块信息 (简化)
    u64 coinbase = 0x1234;
    u64 timestamp = 1700000000;
    u64 block_number = 19000000;

    Account& get_account(u64 addr)
    {
        auto it = accounts.find(addr);
        if (it == accounts.end())
        {
            accounts[addr] = Account{{}, {}, default_balance};
            return accounts[addr];
        }
        return it->second;
    }

    u64 get_storage(u64 addr, u64 key) const
    {
        auto it = accounts.find(addr);
        if (it == accounts.end())
            return 0;
        auto sit = it->second.storage.find(key);
        return sit != it->second.storage.end() ? sit->second : 0;
    }

    void set_storage(u64 addr, u64 key, u64 value)
    {
        get_account(addr).storage[key] = value;
    }

    const bytes* get_code(u64 addr) const
    {
        auto it = accounts.find(addr);
        if (it == accounts.end() || it->second.code.empty())
            return nullptr;
        return &it->second.code;
    }

    // 创建检查点用于回滚 (简化: 深拷贝)
    using Checkpoint = std::unordered_map<u64, Account>;
    Checkpoint checkpoint() const { return accounts; }
    void rollback(const Checkpoint& cp) { accounts = cp; }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Message — 参考 evmc_message
// ═══════════════════════════════════════════════════════════════════════════

struct Message {
    u64 sender = 0;
    u64 value = 0;
    bytes input;
    int depth = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
//  执行状态 — 参考 ExecutionState
// ═══════════════════════════════════════════════════════════════════════════

enum Status : int { SUCCESS = 0, REVERT = 1, FAILURE = 2 };

struct ExecutionState {
    Stack stack;
    Memory memory;
    Host* host;
    Message msg;
    u64 gas;
    size_t pc = 0;
    Status status = SUCCESS;
    bytes output;
    bytes return_data;  // 上一次调用的返回数据
    u64 gas_refund = 0;

    // 合约地址 (简化)
    u64 address = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
//  CodeAnalysis — 参见 baseline_analysis.cpp
// ═══════════════════════════════════════════════════════════════════════════

struct CodeAnalysis {
    bytes code;                // 填充后的代码
    std::vector<bool> jumpdest;  // JUMPDEST 标记 (替代 evmone 的 BitsetSpan)
};

// 参考 analyze_jumpdests() + analyze_legacy()
CodeAnalysis analyze(const u8* raw_code, size_t raw_size)
{
    if (raw_size > MAX_CODE_SIZE)
        return {{}, {}};

    CodeAnalysis result;

    // 填充: +33 字节 (32 for PUSH32 + 1 STOP) — 参考 baseline_analysis.cpp
    result.code.resize(raw_size + 33, 0);
    std::memcpy(result.code.data(), raw_code, raw_size);
    result.code[raw_size] = 0x00;  // STOP 终止保证

    // JUMPDEST 扫描 — 参考 analyze_jumpdests()
    // 利用 OP_PUSH32 == 0x7f == INT8_MAX 的特性
    result.jumpdest.resize(raw_size, false);
    for (size_t i = 0; i < raw_size; ++i)
    {
        const auto op = result.code[i];
        if (static_cast<int8_t>(op) >= static_cast<int8_t>(OP_PUSH1))
            i += op - OP_PUSH1 + 1;  // 跳过 PUSH1-PUSH32 的立即数
        else if (op == OP_JUMPDEST)
            result.jumpdest[i] = true;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  简化的 KECCAK256 — 教学用，非真实实现
// ═══════════════════════════════════════════════════════════════════════════

u64 simple_keccak256(const u8* data, size_t size)
{
    // 简化的哈希 (不是真正的 Keccak256, 仅用于教学)
    u64 hash = 0xcbf29ce484222325;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 0x100000001b3;
    }
    return hash;
}

// ═══════════════════════════════════════════════════════════════════════════
//  execute() — 参考 baseline_execution.cpp dispatch<false>
// ═══════════════════════════════════════════════════════════════════════════

Status execute(ExecutionState& state, const CodeAnalysis& analysis)
{
    const auto& code = analysis.code;
    const auto& jumpdest = analysis.jumpdest;
    auto& stack = state.stack;
    auto& memory = state.memory;
    auto& gas = state.gas;
    auto& pc = state.pc;

    // 主循环 — 参考 dispatch() 的 while(true)
    // 由 padded_code 末尾的 STOP 保证终止
    while (true)
    {
        const auto op = code[pc];

        // ═══════════════════════════════════════════════════════════════
        //  check_requirements — 参考 baseline_execution.cpp
        //  ① 未定义指令检查
        //  ② 栈溢出/下溢检查
        //  ③ Gas 检查
        // ═══════════════════════════════════════════════════════════════
        const auto& traits = TRAITS[op];
        if (traits.gas_cost < 0)
        {
            state.status = FAILURE;
            gas = 0;  // EVM 规范: FAILURE 时 gas 归零
            return FAILURE;
        }

        // 栈溢出检查 (对于产生值的指令)
        if (traits.stack_change > 0)
        {
            if (stack.size >= STACK_LIMIT)
            {
                state.status = FAILURE;
                gas = 0;
                return FAILURE;
            }
        }

        // 栈下溢检查 (对于消费值的指令)
        if (traits.stack_required > 0)
        {
            if (stack.size < static_cast<size_t>(traits.stack_required))
            {
                state.status = FAILURE;
                gas = 0;
                return FAILURE;
            }
        }

        // Gas 检查
        if (gas < static_cast<u64>(traits.gas_cost))
        {
            state.status = FAILURE;
            gas = 0;
            return FAILURE;
        }
        gas -= traits.gas_cost;

        // ═══════════════════════════════════════════════════════════════
        //  指令分发 — 参考 MAP_OPCODES switch 展开
        // ═══════════════════════════════════════════════════════════════
        switch (op)
        {
        // ── 算术 ─────────────────────────────────────────────────────
        case OP_ADD:
            stack.top() += stack.pop();
            ++pc;
            break;

        case OP_MUL:
            stack.top() *= stack.pop();
            ++pc;
            break;

        case OP_SUB:
            stack[1] = stack[1] - stack[0];
            stack.pop();
            ++pc;
            break;

        case OP_DIV:
        {
            auto& v = stack[1];
            v = (v != 0) ? stack[0] / v : 0;
            stack.pop();
            ++pc;
            break;
        }

        case OP_MOD:
        {
            auto& v = stack[1];
            v = (v != 0) ? stack[0] % v : 0;
            stack.pop();
            ++pc;
            break;
        }

        case OP_ADDMOD:
        {
            auto a = stack.pop();
            auto b = stack.pop();
            auto& m = stack.top();
            m = (m != 0) ? (a + b) % m : 0;
            ++pc;
            break;
        }

        // ── 比较 ─────────────────────────────────────────────────────
        case OP_LT:
            stack[1] = (stack[0] < stack[1]) ? 1 : 0;
            stack.pop();
            ++pc;
            break;

        case OP_GT:
            stack[1] = (stack[1] < stack[0]) ? 1 : 0;
            stack.pop();
            ++pc;
            break;

        case OP_EQ:
            stack[1] = (stack[0] == stack[1]) ? 1 : 0;
            stack.pop();
            ++pc;
            break;

        case OP_ISZERO:
            stack.top() = (stack.top() == 0) ? 1 : 0;
            ++pc;
            break;

        // ── 位运算 ───────────────────────────────────────────────────
        case OP_AND:
            stack.top() &= stack.pop();
            ++pc;
            break;

        case OP_OR:
            stack.top() |= stack.pop();
            ++pc;
            break;

        case OP_XOR:
            stack.top() ^= stack.pop();
            ++pc;
            break;

        case OP_NOT:
            stack.top() = ~stack.top();
            ++pc;
            break;

        // ── KECCAK256 ────────────────────────────────────────────────
        case OP_KECCAK256:
        {
            auto offset = static_cast<size_t>(stack.pop());
            auto size = static_cast<size_t>(stack.pop());
            // 动态 gas: 6 per word
            gas += traits.gas_cost;  // 退基础 gas
            u64 dyn_gas = GAS_SHA3 + GAS_SHA3_WORD * ((size + 31) / 32);
            if (gas < dyn_gas) { state.status = FAILURE; gas = 0; return FAILURE; }
            gas -= dyn_gas;

            if (size > 0)
            {
                if (!memory.expand(offset + size, gas))
                    { state.status = FAILURE; gas = 0; return FAILURE; }
                auto hash = simple_keccak256(memory.ptr(offset), size);
                stack.push(hash);
            }
            else
            {
                stack.push(0);
            }
            ++pc;
            break;
        }

        // ── 环境指令 ─────────────────────────────────────────────────
        case OP_ADDRESS:
            stack.push(state.address);
            ++pc;
            break;

        case OP_CALLER:
            stack.push(state.msg.sender);
            ++pc;
            break;

        case OP_CALLVALUE:
            stack.push(state.msg.value);
            ++pc;
            break;

        case OP_CALLDATALOAD:
        {
            auto offset = static_cast<size_t>(stack.top());
            u64 v = 0;
            for (size_t i = 0; i < 8 && offset + i < state.msg.input.size(); ++i)
                v = (v << 8) | state.msg.input[offset + i];
            stack.top() = v;
            ++pc;
            break;
        }

        case OP_CALLDATASIZE:
            stack.push(state.msg.input.size());
            ++pc;
            break;

        case OP_CALLDATACOPY:
        {
            auto mem_offset = static_cast<size_t>(stack.pop());
            auto data_offset = static_cast<size_t>(stack.pop());
            auto size = static_cast<size_t>(stack.pop());
            if (size > 0)
            {
                if (!memory.expand(mem_offset + size, gas))
                    { state.status = FAILURE; gas = 0; return FAILURE; }
                for (size_t i = 0; i < size; ++i)
                {
                    u8 byte = (data_offset + i < state.msg.input.size())
                        ? state.msg.input[data_offset + i]
                        : 0;
                    memory.data[mem_offset + i] = byte;
                }
            }
            ++pc;
            break;
        }

        // ── 区块指令 ─────────────────────────────────────────────────
        case OP_COINBASE:
            stack.push(state.host->coinbase);
            ++pc;
            break;

        case OP_TIMESTAMP:
            stack.push(state.host->timestamp);
            ++pc;
            break;

        case OP_NUMBER:
            stack.push(state.host->block_number);
            ++pc;
            break;

        // ── 栈操作 ───────────────────────────────────────────────────
        case OP_POP:
            stack.pop();
            ++pc;
            break;

        // ── 内存操作 ─────────────────────────────────────────────────
        case OP_MLOAD:
        {
            auto offset = static_cast<size_t>(stack.top());
            if (!memory.expand(offset + 32, gas))
                { state.status = FAILURE; gas = 0; return FAILURE; }
            stack.top() = memory.load_u64(offset);
            ++pc;
            break;
        }

        case OP_MSTORE:
        {
            auto offset = static_cast<size_t>(stack.pop());
            auto value = stack.pop();
            if (!memory.store_u64(offset, value, gas))
                { state.status = FAILURE; gas = 0; return FAILURE; }
            ++pc;
            break;
        }

        case OP_MSTORE8:
        {
            auto offset = static_cast<size_t>(stack.pop());
            auto value = stack.pop();
            if (!memory.store8(offset, static_cast<u8>(value), gas))
                { state.status = FAILURE; gas = 0; return FAILURE; }
            ++pc;
            break;
        }

        // ── 存储操作 ─────────────────────────────────────────────────
        case OP_SLOAD:
        {
            auto key = stack.top();
            stack.top() = state.host->get_storage(state.address, key);
            ++pc;
            break;
        }

        case OP_SSTORE:
        {
            auto key = stack.pop();
            auto value = stack.pop();
            state.host->set_storage(state.address, key, value);
            ++pc;
            break;
        }

        // ── 控制流 ───────────────────────────────────────────────────
        case OP_JUMP:
        {
            auto dst = static_cast<size_t>(stack.pop());
            if (dst >= jumpdest.size() || !jumpdest[dst])
            {
                state.status = FAILURE;
                gas = 0;
                return FAILURE;
            }
            pc = dst;
            break;
        }

        case OP_JUMPI:
        {
            auto dst = static_cast<size_t>(stack.pop());
            auto cond = stack.pop();
            if (cond != 0)
            {
                if (dst >= jumpdest.size() || !jumpdest[dst])
                {
                    state.status = FAILURE;
                    gas = 0;
                    return FAILURE;
                }
                pc = dst;
            }
            else
            {
                ++pc;
            }
            break;
        }

        case OP_PC:
            stack.push(pc);
            ++pc;
            break;

        case OP_GAS:
            stack.push(gas);
            ++pc;
            break;

        case OP_JUMPDEST:
            ++pc;
            break;

        // ── PUSH 指令 ────────────────────────────────────────────────
        case OP_PUSH1:
        case OP_PUSH2:
        case OP_PUSH3:
        case OP_PUSH4:
        case OP_PUSH5:
        case OP_PUSH6:
        case OP_PUSH7:
        case OP_PUSH8:
        {
            const int len = op - OP_PUSH1 + 1;
            u64 v = 0;
            for (int i = 0; i < len; ++i)
                v = (v << 8) | code[pc + 1 + i];
            stack.push(v);
            pc += 1 + len;
            break;
        }

        // ── DUP 指令 ─────────────────────────────────────────────────
        case OP_DUP1:
            stack.push(stack[0]);
            ++pc;
            break;
        case OP_DUP2:
            stack.push(stack[1]);
            ++pc;
            break;
        case OP_DUP3:
            stack.push(stack[2]);
            ++pc;
            break;
        case OP_DUP4:
            stack.push(stack[3]);
            ++pc;
            break;

        // ── SWAP 指令 ────────────────────────────────────────────────
        case OP_SWAP1:
            std::swap(stack[0], stack[1]);
            ++pc;
            break;
        case OP_SWAP2:
            std::swap(stack[0], stack[2]);
            ++pc;
            break;
        case OP_SWAP3:
            std::swap(stack[0], stack[3]);
            ++pc;
            break;

        // ── LOG ──────────────────────────────────────────────────────
        case OP_LOG0:
        {
            stack.pop();  // offset
            auto size = static_cast<size_t>(stack.pop());
            // 动态 gas: 8 per byte
            gas += traits.gas_cost;  // 退基础 gas
            u64 dyn_gas = GAS_LOG0 + GAS_LOG_DATA * size;
            if (gas < dyn_gas) { state.status = FAILURE; gas = 0; return FAILURE; }
            gas -= dyn_gas;
            // 简化: 只计算 gas, 不实际存储 log
            ++pc;
            break;
        }

        // ── CALL — 参考 instructions_calls.cpp call_impl ─────────────
        case OP_CALL:
        {
            auto call_gas = stack.pop();
            auto dst = stack.pop();
            auto value = stack.pop();
            auto in_off = static_cast<size_t>(stack.pop());
            auto in_size = static_cast<size_t>(stack.pop());
            auto out_off = static_cast<size_t>(stack.pop());
            auto out_size = static_cast<size_t>(stack.pop());

            stack.push(0);  // 假设失败
            state.return_data.clear();

            // depth 检查 — "轻失败"
            if (static_cast<size_t>(state.msg.depth) >= CALL_DEPTH_LIMIT)
            {
                ++pc;
                break;
            }

            // value > 0 时额外 gas
            if (value > 0)
            {
                if (gas < GAS_CALL_VALUE)
                    { state.status = FAILURE; gas = 0; return FAILURE; }
                gas -= GAS_CALL_VALUE;
            }

            // 63/64 规则 — 参考 EIP-150
            u64 max_child_gas = gas - gas / 64;
            u64 child_gas = std::min(call_gas, max_child_gas);
            if (value > 0)
                child_gas += GAS_CALL_STIPEND;

            // 内存检查
            if (in_size > 0 && !memory.expand(in_off + in_size, gas))
                { state.status = FAILURE; gas = 0; return FAILURE; }
            if (out_size > 0 && !memory.expand(out_off + out_size, gas))
                { state.status = FAILURE; gas = 0; return FAILURE; }

            // 准备子调用
            Message child_msg;
            child_msg.sender = state.address;
            child_msg.value = value;
            child_msg.depth = state.msg.depth + 1;
            if (in_size > 0)
                child_msg.input.assign(memory.ptr(in_off), memory.ptr(in_off) + in_size);

            // 状态快照
            auto checkpoint = state.host->checkpoint();

            // 执行子调用
            const auto* child_code = state.host->get_code(dst);
            u64 result_gas_left = child_gas;
            Status result_status = SUCCESS;
            u64 child_refund = 0;

            if (child_code == nullptr)
            {
                // 空代码: 直接成功
                result_gas_left = child_gas;
                result_status = SUCCESS;
            }
            else
            {
                // 加载子合约代码并执行
                auto child_analysis = analyze(child_code->data(), child_code->size());
                ExecutionState child_state;
                child_state.host = state.host;
                child_state.msg = child_msg;
                child_state.gas = child_gas;
                child_state.address = dst;

                result_status = execute(child_state, child_analysis);
                result_gas_left = child_state.gas;
                child_refund = child_state.gas_refund;
                state.return_data = child_state.output;
            }

            // 失败时回滚
            if (result_status != SUCCESS)
                state.host->rollback(checkpoint);

            // 更新栈顶
            stack.top() = (result_status == SUCCESS) ? 1 : 0;

            // 拷贝输出到父 memory
            u64 copy_size = std::min(out_size, state.return_data.size());
            if (copy_size > 0)
                std::memcpy(memory.data.data() + out_off, state.return_data.data(), copy_size);

            // 结算 gas
            u64 gas_used = child_gas - result_gas_left;
            if (gas_used > gas)
                gas = 0;
            else
                gas -= gas_used;

            // 退款传播
            state.gas_refund += child_refund;

            ++pc;
            break;
        }

        // ── CREATE ───────────────────────────────────────────────────
        case OP_CREATE:
        {
            stack.pop();  // value (简化: 不使用)
            stack.pop();  // offset (简化: 不使用)
            stack.pop();  // size (简化: 不使用)

            stack.push(0);  // 简化: 总是失败
            ++pc;
            break;
        }

        // ── 终止指令 ─────────────────────────────────────────────────
        case OP_RETURN:
        {
            auto offset = static_cast<size_t>(stack.pop());
            auto size = static_cast<size_t>(stack.pop());
            if (size > 0)
            {
                if (!memory.expand(offset + size, gas))
                    { state.status = FAILURE; gas = 0; return FAILURE; }
                state.output.assign(memory.ptr(offset), memory.ptr(offset) + size);
            }
            state.status = SUCCESS;
            return SUCCESS;
        }

        case OP_REVERT:
        {
            auto offset = static_cast<size_t>(stack.pop());
            auto size = static_cast<size_t>(stack.pop());
            if (size > 0)
            {
                if (!memory.expand(offset + size, gas))
                    { state.status = FAILURE; gas = 0; return FAILURE; }
                state.output.assign(memory.ptr(offset), memory.ptr(offset) + size);
            }
            state.status = REVERT;
            return REVERT;
        }

        case OP_STOP:
            state.status = SUCCESS;
            return SUCCESS;

        default:
            state.status = FAILURE;
            return FAILURE;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  辅助函数
// ═══════════════════════════════════════════════════════════════════════════

// 打印字节码
void print_code(const u8* code, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        printf("%02x", code[i]);
    printf("\n");
}

// 运行一个测试用例
void run_test(const char* name, const u8* code, size_t code_size,
    u64 gas_limit = 100000, u64 sender = 0xAAAA, u64 value = 0,
    const bytes& input = {})
{
    printf("═══ %s ═══\n", name);
    printf("Code: ");
    print_code(code, code_size);

    Host host;
    auto analysis = analyze(code, code_size);

    // 注册合约代码到 Host
    auto& acc = host.get_account(0xCCCC);
    acc.code.assign(code, code + code_size);

    ExecutionState state;
    state.host = &host;
    state.gas = gas_limit;
    state.address = 0xCCCC;
    state.msg.sender = sender;
    state.msg.value = value;
    state.msg.input = input;

    auto status = execute(state, analysis);

    const char* status_str = (status == SUCCESS) ? "SUCCESS" :
                             (status == REVERT) ? "REVERT" : "FAILURE";
    printf("Status: %s\n", status_str);
    printf("Gas used: %lu\n", gas_limit - state.gas);
    printf("Gas left: %lu\n", state.gas);
    printf("Stack size: %zu\n", state.stack.size);
    if (state.stack.size > 0)
        printf("Stack top: 0x%lx\n", state.stack.top());
    if (!state.output.empty())
    {
        printf("Output: ");
        for (auto b : state.output)
            printf("%02x", b);
        printf("\n");
    }
    printf("\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  测试用例
// ═══════════════════════════════════════════════════════════════════════════

int main()
{
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║              mini_evm — 精简 EVM 实现                       ║\n");
    printf("║              参考 evmone Baseline 解释器                    ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    // ── 测试 1: 简单算术 ─────────────────────────────────────────────
    // PUSH1 21 PUSH1 21 ADD STOP → 栈顶 = 42
    {
        u8 code[] = {0x60, 0x15, 0x60, 0x15, 0x01, 0x00};
        run_test("Test 1: 21 + 21 = 42", code, sizeof(code));
    }

    // ── 测试 2: MSTORE + MLOAD ───────────────────────────────────────
    // PUSH1 42 PUSH1 0 MSTORE PUSH1 0 MLOAD STOP → 栈顶 = 42
    {
        u8 code[] = {0x60, 0x2a, 0x60, 0x00, 0x52, 0x60, 0x00, 0x51, 0x00};
        run_test("Test 2: MSTORE/MLOAD roundtrip", code, sizeof(code));
    }

    // ── 测试 3: JUMP + JUMPI ─────────────────────────────────────────
    // PUSH1 1 PUSH1 0x0a JUMPI PUSH1 99 STOP JUMPDEST PUSH1 42 STOP
    // → 栈顶 = 42 (跳过 99)
    {
        u8 code[] = {
            0x60, 0x01,   // PUSH1 1 (condition)
            0x60, 0x0a,   // PUSH1 0x0a (target)
            0x57,         // JUMPI
            0x60, 0x63,   // PUSH1 99 (dead code)
            0x00,         // STOP
            0x5b,         // JUMPDEST (PC=0x08) — 修正: 实际是 0x08
            0x60, 0x2a,   // PUSH1 42
            0x00          // STOP
        };
        // 修正 target
        code[3] = 0x08;  // JUMPDEST 在 PC=0x08
        run_test("Test 3: JUMPI conditional jump", code, sizeof(code));
    }

    // ── 测试 4: SSTORE + SLOAD ───────────────────────────────────────
    // PUSH1 42 PUSH1 0 SSTORE PUSH1 0 SLOAD STOP → 栈顶 = 42
    {
        u8 code[] = {
            0x60, 0x2a,   // PUSH1 42
            0x60, 0x00,   // PUSH1 0 (slot)
            0x55,         // SSTORE
            0x60, 0x00,   // PUSH1 0 (slot)
            0x54,         // SLOAD
            0x00          // STOP
        };
        run_test("Test 4: SSTORE/SLOAD storage", code, sizeof(code));
    }

    // ── 测试 5: SUB ─────────────────────────────────────────────────
    // PUSH1 7 PUSH1 3 SUB STOP → 栈顶 = 4 (7-3)
    {
        u8 code[] = {
            0x60, 0x07,   // PUSH1 7
            0x60, 0x03,   // PUSH1 3
            0x03,         // SUB (7 - 3)
            0x00          // STOP
        };
        run_test("Test 5: SUB (7 - 3 = 4)", code, sizeof(code));
    }

    // ── 测试 6: CALLER + CALLVALUE ───────────────────────────────────
    // CALLER CALLVALUE ADD STOP → 栈顶 = sender + value
    {
        u8 code[] = {
            0x33,         // CALLER
            0x34,         // CALLVALUE
            0x01,         // ADD
            0x00          // STOP
        };
        run_test("Test 6: CALLER + CALLVALUE", code, sizeof(code),
            100000, 0xAAAA, 100);
    }

    // ── 测试 7: CALLDATALOAD ─────────────────────────────────────────
    // PUSH1 4 CALLDATALOAD STOP → 读取 calldata[4:12]
    {
        u8 code[] = {
            0x60, 0x04,   // PUSH1 4
            0x35,         // CALLDATALOAD
            0x00          // STOP
        };
        bytes input = {0x00, 0x00, 0x00, 0x00,  // padding
                       0x00, 0x00, 0x00, 0x2a};  // 42
        run_test("Test 7: CALLDATALOAD reads calldata[4]", code, sizeof(code),
            100000, 0xAAAA, 0, input);
    }

    // ── 测试 8: RETURN ───────────────────────────────────────────────
    // PUSH1 42 PUSH1 0 MSTORE PUSH1 32 PUSH1 0 RETURN
    {
        u8 code[] = {
            0x60, 0x2a,   // PUSH1 42
            0x60, 0x00,   // PUSH1 0
            0x52,         // MSTORE
            0x60, 0x20,   // PUSH1 32
            0x60, 0x00,   // PUSH1 0
            0xf3          // RETURN
        };
        run_test("Test 8: RETURN with value 42", code, sizeof(code));
    }

    // ── 测试 9: 循环 (累加 1+2+...+10) ───────────────────────────────
    // 计算 sum = 1+2+...+10 = 55
    //
    // 栈布局: [sum, i] (sum 在栈顶)
    //
    // 0x00: PUSH1 1       ; i = 1
    // 0x02: PUSH1 0       ; sum = 0
    // 0x04: JUMPDEST      ; loop_start (PC=0x04)
    // 0x05: DUP2          ; [i, sum, i]
    // 0x06: PUSH1 11      ; [11, i, sum, i]
    // 0x08: SWAP1         ; [i, 11, sum, i] — 交换使 i 在栈顶
    // 0x09: LT            ; [i<11, sum, i]
    // 0x0a: ISZERO        ; [i>=11, sum, i] — 1=exit, 0=continue
    // 0x0b: PUSH1 0x18    ; [exit, i>=11, sum, i]
    // 0x0d: JUMPI         ; if i>=11, jump to exit
    // 0x0e: DUP2          ; [i, sum, i]
    // 0x0f: ADD           ; [sum+i, i] — sum += i
    // 0x10: SWAP1         ; [i, sum+i]
    // 0x11: PUSH1 1       ; [1, i, sum+i]
    // 0x13: ADD           ; [i+1, sum+i] — i++
    // 0x14: SWAP1         ; [sum+i, i+1]
    // 0x15: PUSH1 0x04    ; [loop_start, sum+i, i+1]
    // 0x17: JUMP          ; goto loop
    // 0x18: JUMPDEST      ; exit (PC=0x18)
    // 0x19: SWAP1         ; [i, sum]
    // 0x1a: POP           ; [sum]
    // 0x1b: STOP
    {
        u8 code[] = {
            0x60, 0x01,   // 0x00: PUSH1 1 (i)
            0x60, 0x00,   // 0x02: PUSH1 0 (sum)
            0x5b,         // 0x04: JUMPDEST (loop start)
            0x81,         // 0x05: DUP2 (dup i)
            0x60, 0x0b,   // 0x06: PUSH1 11
            0x90,         // 0x08: SWAP1 (i ↔ 11)
            0x10,         // 0x09: LT (i < 11)
            0x15,         // 0x0a: ISZERO (i >= 11)
            0x60, 0x18,   // 0x0b: PUSH1 0x18 (exit)
            0x57,         // 0x0d: JUMPI
            0x81,         // 0x0e: DUP2 (dup i)
            0x01,         // 0x0f: ADD (sum += i)
            0x90,         // 0x10: SWAP1 (i ↔ sum)
            0x60, 0x01,   // 0x11: PUSH1 1
            0x01,         // 0x13: ADD (i++)
            0x90,         // 0x14: SWAP1 (sum ↔ i)
            0x60, 0x04,   // 0x15: PUSH1 0x04 (loop start)
            0x56,         // 0x17: JUMP
            0x5b,         // 0x18: JUMPDEST (exit)
            0x90,         // 0x19: SWAP1 (sum ↔ i)
            0x50,         // 0x1a: POP (pop i)
            0x00          // 0x1b: STOP
        };
        run_test("Test 9: Loop sum(1..10) = 55", code, sizeof(code));
    }

    printf("═══ 所有测试完成 ═══\n");
    return 0;
}
