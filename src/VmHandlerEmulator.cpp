#pragma once
#include "VmHandlerEmulator.hpp"
#include <triton/context.hpp>
#include <triton/x8664Cpu.hpp>

static HandlerEmulationData* currentDataPtr{ nullptr };

void memWriteCallback(triton::Context& ctx, const triton::arch::MemoryAccess& mem) {
	if (ctx.isMemorySymbolized(mem)) return;
	
	uint64_t addr = mem.getAddress();
	size_t size = mem.getSize();

	uint64_t rspBase = currentDataPtr->rspChange.startValue;
	uint64_t vspBase = currentDataPtr->vspChange.startValue;

	if (vspBase && addr < vspBase && addr >= vspBase - 8) {
		ctx.symbolizeMemory(mem, "NewStackTop");
		return;
	}

	if (addr >= defaultImageBase) {
		ctx.concretizeMemory(mem);
		return;
	}
}
void memReadCallback(triton::Context& ctx, const triton::arch::MemoryAccess& mem) {
	if (ctx.isMemorySymbolized(mem)) return;

	uint64_t addr = mem.getAddress();
	size_t size = mem.getSize();
	if (addr >= defaultImageBase) {
		ctx.concretizeMemory(mem);
		return;
	}

	uint64_t rspBase = currentDataPtr->rspChange.startValue;
	uint64_t vspBase = currentDataPtr->vspChange.startValue;
	if (vspBase && addr >= rspBase && addr < vspBase - 8) {
		uint64_t offset = addr - rspBase;
		ctx.symbolizeMemory(mem, "VmReg_" + std::to_string(offset));
		return;
	}

	if (vspBase && addr >= vspBase && addr < vspBase + 0x100) {
		uint64_t offset = addr - vspBase;
		ctx.symbolizeMemory(mem, "StackArg_" + std::to_string(offset));
		return;
	}
}


constexpr size_t kAstDeepLevelLimit = 16;
HandlerEmulationData EmulateVmHandler(
	const VmBasicBlock& block,
	triton::arch::register_e vipRegId,
	triton::arch::register_e vspRegId
)
{
	HandlerEmulationData data;
	data.baseAddress = block.instructions[0].instruction->getAddress();
	currentDataPtr = &data;
	try {
		auto context = std::make_unique<triton::Context>(triton::arch::ARCH_X86_64);
		context->setMode(triton::modes::MEMORY_ARRAY, false);
		context->setMode(triton::modes::ALIGNED_MEMORY, true);
		context->setMode(triton::modes::CONSTANT_FOLDING, true);
		context->setMode(triton::modes::AST_OPTIMIZATIONS, true);
		context->setMode(triton::modes::PC_TRACKING_SYMBOLIC, false);
		context->setMode(triton::modes::TAINT_THROUGH_POINTERS, false);
		context->setMode(triton::modes::SYMBOLIZE_INDEX_ROTATION, false);

		block.instructions[0].ApplyRegisters(*context);
		for (auto& instr : block.instructions) {
			instr.ApplyMemoryReads(*context);
		}

		context->addCallback(triton::callbacks::GET_CONCRETE_MEMORY_VALUE, memReadCallback);

		if (vipRegId != triton::arch::ID_REG_INVALID) {
			data.vipRegId = vipRegId;
			auto vipReg = context->getRegister(vipRegId);
			data.vipChange.startValue = static_cast<uint64_t>(context->getConcreteRegisterValue(vipReg));
			context->symbolizeRegister(vipReg, "VIP");
		}

		if (vspRegId != triton::arch::ID_REG_INVALID) {
			data.vspRegId = vspRegId;
			auto vspReg = context->getRegister(vspRegId);
			data.vspChange.startValue = static_cast<uint64_t>(context->getConcreteRegisterValue(vspReg));
			context->symbolizeRegister(context->getRegister(vspRegId), "VSP");
		}

		data.rspChange.startValue = static_cast<uint64_t>(context->getConcreteRegisterValue(context->registers.x86_rsp));
		context->symbolizeRegister(context->registers.x86_rsp, "VM_CONTEXT");

		for (auto& inst : block.instructions) {
			context->processing(*inst.instruction);

			for (auto& memAccess : inst.instruction->getLoadAccess()) {
				MemoryAccessInfo readInfo;
				readInfo.address = memAccess.first.getAddress();
				readInfo.size = memAccess.first.getSize();
				readInfo.concreteValue = static_cast<uint64_t>(context->getConcreteMemoryValue(memAccess.first, false));
				auto& instOperands = inst.instruction->operands;
				bool isPopFq = false;
				auto instructionType = inst.instruction->getType();
				if (instOperands.size() > 0 && instOperands[0].getType() == triton::arch::OP_REG) {
					readInfo.reg = &inst.instruction->operands[0].getRegister();
				}
				else if (instructionType == triton::arch::x86::ID_INS_POPFQ) {
					readInfo.reg = &context->registers.x86_eflags;
				}
				readInfo.instruction = &inst;
				if (memAccess.second->getLevel() < kAstDeepLevelLimit) {
					readInfo.ast = context->simplify(memAccess.second, false, true);
				}
				else {
					readInfo.ast = memAccess.second;
				}

				int64_t dist = static_cast<int64_t>(readInfo.address) - static_cast<int64_t>(data.vspChange.startValue);
				bool isMovSb = instructionType == triton::arch::x86::ID_INS_MOVSB;
				bool isSystemRead = isMovSb;
				if (isSystemRead) {
					data.systemReads[readInfo.address] = std::move(readInfo);
				}
				else {
					data.logicReads[readInfo.address] = std::move(readInfo);
				}
				
			}

			for (auto& memAccess : inst.instruction->getStoreAccess()) {
				MemoryAccessInfo writeInfo;
				writeInfo.address = memAccess.first.getAddress();
				writeInfo.size = memAccess.first.getSize();
				writeInfo.concreteValue = static_cast<uint64_t>(context->getConcreteMemoryValue(memAccess.first, false));

				auto& instOperands = inst.instruction->operands;
				bool isPushFq = false;
				auto instructionType = inst.instruction->getType();
				if (instOperands.size() == 1 && instOperands[0].getType() == triton::arch::OP_REG) {
					writeInfo.reg = &inst.instruction->operands[0].getRegister();
				}
				else if (instOperands.size() == 2 && instOperands[1].getType() == triton::arch::OP_REG) {
					writeInfo.reg = &inst.instruction->operands[1].getRegister();
				}
				else if (instructionType == triton::arch::x86::ID_INS_PUSHFQ) {
					writeInfo.reg = &context->registers.x86_eflags;
					isPushFq = true;
					context->symbolizeMemory(memAccess.first, "RFLAGS_CLEAN");
				}
				
				if (memAccess.second->getLevel() < kAstDeepLevelLimit && !isPushFq) {
					writeInfo.ast = context->simplify(memAccess.second, false, true);
				}
				else {
					writeInfo.ast = memAccess.second;
				}
				writeInfo.instruction = &inst;
				int64_t dist = static_cast<int64_t>(writeInfo.address) - static_cast<int64_t>(data.vspChange.startValue);
				bool isMovSb = instructionType == triton::arch::x86::ID_INS_MOVSB;
				bool isSystemWrite = isMovSb;
				if (isSystemWrite) {
					data.systemWrites[writeInfo.address] = std::move(writeInfo);
				}
				else {
					data.logicWrites[writeInfo.address] = std::move(writeInfo);
				}
			}
		}
		for (auto& reg : context->getParentRegisters()) {
			if (reg->getSize() != 8) continue;
			if (reg->getId() == triton::arch::ID_REG_X86_RIP) continue;
			if (!reg->getName().starts_with("r")) continue;
			auto ast = context->getRegisterAst(*reg);

			data.registerAstMap[reg->getId()] = context->simplify(ast, false, true);
		}

		data.registerAstMap[context->registers.x86_eflags.getId()] = context->simplify(
			context->getRegisterAst(context->registers.x86_eflags), false, true);


		if (vipRegId != triton::arch::ID_REG_INVALID) {
			auto vipReg = context->getRegister(vipRegId);
			data.vipChange.endValue = static_cast<uint64_t>(context->getConcreteRegisterValue(vipReg));
			auto symbolicReg = context->getSymbolicRegister(vipReg);
			if (symbolicReg) {
				data.vipAst = context->simplify(symbolicReg->getAst(), false, true);
			}
		}

		if (vspRegId != triton::arch::ID_REG_INVALID) {
			auto vspReg = context->getRegister(vspRegId);
			data.vspChange.endValue = static_cast<uint64_t>(context->getConcreteRegisterValue(vspReg));
			auto symbolicReg = context->getSymbolicRegister(vspReg);
			if (symbolicReg) {
				data.vspAst = context->simplify(symbolicReg->getAst(), false, true);
			}
		}

		data.rspChange.endValue = static_cast<uint64_t>(context->getConcreteRegisterValue(context->registers.x86_rsp));
		data.context = std::move(context);
	}
	catch (std::exception& ex) {
		std::cout << ex.what() << '\n';
		PrintBasicBlock(block);
	}
	currentDataPtr = nullptr;
	return data;
}

void PrintEmulationData(const HandlerEmulationData& data) {
	if (data.vipAst) {
		std::cout << "VIP AST: " << triton::ast::unroll(data.vipAst.value()) << "\n";
	}
	if (data.vspAst) {
		std::cout << "VSP AST: " << triton::ast::unroll(data.vspAst.value()) << "\n";
	}

	for (auto& [regId, ast] : data.registerAstMap) {
		std::cout << data.context->getRegister(regId) << ": " << triton::ast::unroll(ast) << '\n';
	}

	std::cout << "VSP CHANGES: " << std::hex << data.vspChange.startValue << " - " << data.vspChange.endValue << '\n';
	std::cout << "VIP CHANGES: " << std::hex << data.vipChange.startValue << " - " << data.vipChange.endValue << '\n';
	std::cout << "RSP CHANGES: " << std::hex << data.rspChange.startValue << " - " << data.rspChange.endValue << '\n';
	std::cout << "Logic Writes:\n";
	for (auto& write : data.logicWrites) {
		std::cout << "  [STORE] Addr: 0x" << std::hex << write.second.address << std::dec
			<< " Size: " << write.second.size
			<< " Value: " << write.second.concreteValue << " (";
		
			std::cout << write.second.ast << ")\n";
	}
	std::cout << "Logic Reads:\n";
	for (auto& read : data.logicReads) {
		std::cout << "  [LOAD] Addr: 0x" << std::hex << read.second.address << std::dec
			<< " Size: " << read.second.size
			<< " Value: " << read.second.concreteValue << " (";

			std::cout << read.second.ast << ")\n";
	}

	std::cout << "System Writes:\n";
	for (auto& write : data.systemWrites) {
		std::cout << "  [STORE] Addr: 0x" << std::hex << write.second.address << std::dec
			<< " Size: " << write.second.size
			<< " Value: " << write.second.concreteValue << " (";

		std::cout << write.second.ast << ")\n";
	}
	std::cout << "System Reads:\n";
	for (auto& read : data.systemReads) {
		std::cout << "  [LOAD] Addr: 0x" << std::hex << read.second.address << std::dec
			<< " Size: " << read.second.size
			<< " Value: " << read.second.concreteValue << " (";

		std::cout << read.second.ast << ")\n";
	}

	std::cout << "--------------------------------\n\n";
}