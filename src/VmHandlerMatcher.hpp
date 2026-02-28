#pragma once
#include <triton/architecture.hpp>
#include "VmHandlerEmulator.hpp"
#include <variant>

enum VmHandlerType {
	Handler_Unknown,
	Handler_VmEntry,
	Handler_VmPop,
	Handler_VmPushConst,
	Handler_VmPushReg,
	Handler_VmPushVsp,
	Handler_VmPopVsp,

	Handler_VmReadMem,
	Handler_VmWriteMem,

	Handler_VmAdd,    // (+)
	Handler_VmNor,    // ~(A | B)
	Handler_VmNand,   // ~(A & B)
	Handler_VmShr,    // (>>)
	Handler_VmShl,    // (<<)
	Handler_VmRol,    // Rotate Left
	Handler_VmRor,    // Rotate Right

	Handler_VmShld,
	Handler_VmShrd,

	Handler_VmJmpIndirect,
	Handler_VmDispatch,

	Handler_VmExit,
};
enum HandlerBitDepth {
	BitDepth_8 = 8,
	BitDepth_16 = 16,
	BitDepth_32 = 32,
	BitDepth_64 = 64
};

struct TraceInstructionLocation {
	uint64_t instAddr;
	triton::arch::register_e regId;
};

struct NativeRegData {
	triton::arch::register_e id{ triton::arch::register_e::ID_REG_INVALID };
	std::string name{};
	uint64_t value{ 0 };
};

struct VmEntryHandlerData {
	uint64_t imageBaseDifference{ 0 };
	std::vector<NativeRegData> pushRegsOrder{};
	triton::arch::register_e vspReg{ triton::arch::register_e::ID_REG_INVALID };
	triton::arch::register_e vipReg{ triton::arch::register_e::ID_REG_INVALID };
	std::optional<uint64_t> returnAddr;
	std::optional<uint64_t> pushedConst;
};

struct VmContextAccessData {
	uint32_t regIndex{ 0 };
	uint32_t offset{ 0 };
	uint64_t value{ 0 };

	TraceInstructionLocation contextAccess;
};

struct VmPushConstData {
	uint64_t value{ 0 };

	TraceInstructionLocation constData;
};


struct VmMemAccessData {
	uint64_t address{ 0 };
	uint64_t value{ 0 };
};

struct VmPushVspData {
	uint64_t value{ 0 };
};

struct VmPopVspData {
	int64_t offset{ 0 };
};

struct VmAluData {
	uint64_t arg1{ 0 };
	uint64_t arg2{ 0 };
	uint64_t result{ 0 };
	uint64_t flags{ 0 };
};

struct VmShldData {
	uint64_t dst{ 0 };
	uint64_t src{ 0 };
	uint64_t shift{ 0 };
	uint64_t result{ 0 };
	uint64_t flags{ 0 };
};

struct VmJmpData {
	uint64_t newVip{ 0 };
	int64_t newVipShift{ 0 };
	triton::arch::register_e newVspReg{ triton::arch::register_e::ID_REG_INVALID };
	triton::arch::register_e newVipReg{ triton::arch::register_e::ID_REG_INVALID };

	TraceInstructionLocation jmpDestData;
};

struct VmExitData {
	std::vector<NativeRegData> popedRegsOrder{};
};

struct HandlerMatch {
	HandlerMatch() = default;

	VmHandlerType type{ Handler_Unknown };
	HandlerBitDepth bitDepth{ BitDepth_64 };
	uint64_t addr{ 0 };
	uint64_t vipBefore{ 0 };
	uint64_t vspBefore{ 0 };
	uint64_t vspAfter{ 0 };
	std::variant<std::monostate, 
		VmEntryHandlerData, VmContextAccessData, VmPushConstData,
		VmPushVspData, VmMemAccessData,
		VmAluData, VmShldData, VmJmpData,
		VmExitData, VmPopVspData> matchData;
};


HandlerMatch MatchVmHandler(const HandlerEmulationData& data);

struct VirtualBasicBlock {
	uint64_t startAddr{ 0 };
	int64_t nextVipShift{ 0 };
	int64_t virtualStackOffset{ 0 };
	std::vector<HandlerMatch> instructions;
};

std::vector<VirtualBasicBlock> SplitToVirtualBlocks(const std::vector<HandlerMatch>& trace);

std::ostream& operator<<(std::ostream& os, const HandlerMatch& p);
std::ostream& operator<<(std::ostream& os, const VirtualBasicBlock& p);