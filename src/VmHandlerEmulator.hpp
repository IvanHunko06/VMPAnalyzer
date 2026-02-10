#pragma once
#include <triton/context.hpp>
#include "VmBasicBlock.hpp"
#include <map>
#include <optional>

constexpr uint64_t defaultImageBase = 0x140000000;

struct MemoryAccessInfo {
	uint64_t address{};
	uint32_t size{};
	uint64_t concreteValue{};
	triton::ast::SharedAbstractNode ast{};
	triton::arch::Register* reg{ nullptr };
	const NativeInstructionContext* instruction{ nullptr };
};

struct RegisterChange {
	uint64_t startValue{ 0 };
	uint64_t endValue{ 0 };
};

struct HandlerEmulationData {
	std::unique_ptr<triton::Context> context;
	uint64_t baseAddress;

	RegisterChange vipChange;
	RegisterChange vspChange;
	RegisterChange rspChange;

	std::optional<triton::ast::SharedAbstractNode> vipAst;
	std::optional<triton::ast::SharedAbstractNode> vspAst;

	std::map<triton::arch::register_e, triton::ast::SharedAbstractNode> registerAstMap;

	triton::arch::register_e vipRegId;
	triton::arch::register_e vspRegId;

	std::map<uint64_t, MemoryAccessInfo> logicReads;
	std::map<uint64_t, MemoryAccessInfo> logicWrites;

	std::map<uint64_t, MemoryAccessInfo> systemReads;
	std::map<uint64_t, MemoryAccessInfo> systemWrites;
};


HandlerEmulationData EmulateVmHandler(
	const VmBasicBlock& block, 
	triton::arch::register_e vipRegId, 
	triton::arch::register_e vspRegId
);

void PrintEmulationData(const HandlerEmulationData& data);