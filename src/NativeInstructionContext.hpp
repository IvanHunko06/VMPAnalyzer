#pragma once
#include <triton/context.hpp>
#include <map>
#include <string>
#include <memory>

struct NativeInstructionContext {
	std::unique_ptr<triton::arch::Instruction> instruction{};
    std::string registerValues{};
    std::string memoryReads{};
    uint64_t address;

	NativeInstructionContext(triton::Context& ctx, std::string&& instr, std::string&& registersState, std::string&& memoryReads) {
        parseInstruction(ctx, instr);
        this->registerValues = std::move(registersState);
        this->memoryReads = std::move(memoryReads);
    }
    void ApplyMemoryReads(triton::Context& ctx) const {
        if (memoryReads.empty()) {
            return;
        }

        auto parts = split(memoryReads, ':');
        uint64_t address = std::stoull(parts[0], nullptr, 16);
        uint64_t size = std::stoul(parts[1], nullptr, 10);
        std::vector<uint8_t> valueBytes = hexStringToBytes(parts[2]);
        if (ctx.isConcreteMemoryValueDefined(address, size))
            return;

        if (ctx.isMemorySymbolized(address, size))
            return;

        ctx.setConcreteMemoryAreaValue(address, valueBytes);
    }
    void ApplyRegisters(triton::Context& ctx) const {
        if (registerValues.empty()) {
            return;
        }

        auto parts = split(registerValues, ':');
        triton::arch::Register* regs[16] = {
            &ctx.registers.x86_rax,
            &ctx.registers.x86_rbx,
            &ctx.registers.x86_rcx,
            &ctx.registers.x86_rdx,
            &ctx.registers.x86_rdi,
            &ctx.registers.x86_rsi,
            &ctx.registers.x86_rbp,
            &ctx.registers.x86_rsp,
            &ctx.registers.x86_r8,
            &ctx.registers.x86_r9,
            &ctx.registers.x86_r10,
            &ctx.registers.x86_r11,
            &ctx.registers.x86_r12,
            &ctx.registers.x86_r13,
            &ctx.registers.x86_r14,
            &ctx.registers.x86_r15
        };
        if (parts.empty()) {
            assert(false && "No register parts found");
            return;
        }
        for (int i = 0; i < 16; ++i) {
            uint64_t value = std::stoull(parts[i], nullptr, 16);
            uint64_t currentValue = static_cast<uint64_t>(ctx.getConcreteRegisterValue(*regs[i]));
            if (currentValue != value) {
                //if (ctx.isRegisterSymbolized(*regs[i])) continue;
                ctx.setConcreteRegisterValue(*regs[i], value);
            }

        }
    }
    uint64_t GetRegisterValue(triton::arch::register_e reg) const{
        auto parts = split(registerValues, ':');
        static std::map< triton::arch::register_e, uint64_t> regToIndexMap{
            {triton::arch::register_e::ID_REG_X86_RAX, 0},
            {triton::arch::register_e::ID_REG_X86_RBX, 1},
            {triton::arch::register_e::ID_REG_X86_RCX, 2},
            {triton::arch::register_e::ID_REG_X86_RDX, 3},
            {triton::arch::register_e::ID_REG_X86_RDI, 4},
            {triton::arch::register_e::ID_REG_X86_RSI, 5},
            {triton::arch::register_e::ID_REG_X86_RBP, 6},
            {triton::arch::register_e::ID_REG_X86_RSP, 7},
            {triton::arch::register_e::ID_REG_X86_R8, 8},
            {triton::arch::register_e::ID_REG_X86_R9, 9},
            {triton::arch::register_e::ID_REG_X86_R10, 10},
            {triton::arch::register_e::ID_REG_X86_R11, 11},
            {triton::arch::register_e::ID_REG_X86_R12, 12},
            {triton::arch::register_e::ID_REG_X86_R13, 13},
            {triton::arch::register_e::ID_REG_X86_R14, 14},
            {triton::arch::register_e::ID_REG_X86_R15, 15},
        };
        auto index = regToIndexMap[reg];
        uint64_t value = std::stoull(parts[index], nullptr, 16);
        return value;
    }
    inline uint64_t GetAddress() const noexcept{
        return address;
    }

private:
    std::vector<std::string> split(const std::string& s, char delimiter) const {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);

        // getline читает поток до разделителя
        while (std::getline(tokenStream, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }
    std::vector<uint8_t> hexStringToBytes(const std::string& hex) const {
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i < hex.length(); i += 2) {
            std::string byteString = hex.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoul(byteString, nullptr, 16));
            bytes.push_back(byte);
        }
        return bytes;
    }
    void parseInstruction(triton::Context& ctx, const std::string& line) {
        auto parts = split(line, ':');
        address = std::stoull(parts[0], nullptr, 16);
        std::vector<uint8_t> opcodeBytes = hexStringToBytes(parts[2]);
        instruction = std::make_unique<triton::arch::Instruction>();
        instruction->setAddress(address);
        instruction->setOpcode(opcodeBytes.data(), opcodeBytes.size());
        ctx.disassembly(*instruction);
    }
};