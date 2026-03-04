#pragma once
#include "triton/architecture.hpp"

struct NativeContext {
	uint64_t raxReg{ 0 };
	uint64_t rbxReg{ 0 };
	uint64_t rcxReg{ 0 };
	uint64_t rdxReg{ 0 };
	uint64_t rdiReg{ 0 };
	uint64_t rsiReg{ 0 };
	uint64_t rbpReg{ 0 };
	uint64_t r8Reg{ 0 };
	uint64_t r9Reg{ 0 };
	uint64_t r10Reg{ 0 };
	uint64_t r11Reg{ 0 };
	uint64_t r12Reg{ 0 };
	uint64_t r13Reg{ 0 };
	uint64_t r14Reg{ 0 };
	uint64_t r15Reg{ 0 };
	uint64_t rflagsReg{ 0 };
	uint64_t rspReg{ 0 };
	uint64_t retAddr{ 0 };
	uint64_t vmExitAddr{ 0 };
};
constexpr int64_t rspOffset = 16;
constexpr int64_t retAddrOffset = 17;
constexpr int64_t vmExitAddr = 18;

static int64_t GetNativeOffset(triton::arch::register_e reg) {
	switch (reg)
	{
	case triton::arch::ID_REG_X86_RAX: return 0;
	case triton::arch::ID_REG_X86_RBX: return 1;
	case triton::arch::ID_REG_X86_RCX: return 2;
	case triton::arch::ID_REG_X86_RDX: return 3;
	case triton::arch::ID_REG_X86_RDI: return 4;
	case triton::arch::ID_REG_X86_RSI: return 5;
	case triton::arch::ID_REG_X86_RBP: return 6;
	case triton::arch::ID_REG_X86_R8:  return 7;
	case triton::arch::ID_REG_X86_R9:  return 8;
	case triton::arch::ID_REG_X86_R10: return 9;
	case triton::arch::ID_REG_X86_R11: return 10;
	case triton::arch::ID_REG_X86_R12: return 11;
	case triton::arch::ID_REG_X86_R13: return 12;
	case triton::arch::ID_REG_X86_R14: return 13;
	case triton::arch::ID_REG_X86_R15: return 14;
	case triton::arch::ID_REG_X86_EFLAGS: return 15;
	default: return -1;
	}
}