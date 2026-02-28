#include "VmHandlerTrace.hpp"
#include "triton/context.hpp"
#include "triton/x8664Cpu.hpp"

bool IsNewHandlerTransfer(const triton::arch::Instruction& inst) {
	auto type = inst.getType();
	if (type == triton::arch::x86::ID_INS_CALL &&
		inst.operands[0].getType() != triton::arch::OP_IMM) {
		return true;
	}

	if (type == triton::arch::x86::ID_INS_JMP && inst.operands[0].getType() == triton::arch::OP_REG) {
		return true;
	}

	if (type == triton::arch::x86::ID_INS_RET) {
		return true;
	}

	return false;
}

std::vector<VmHandlerTrace> ParseVmHandlerTraces(std::fstream& traceFile) {
	triton::Context tempContext(triton::arch::architecture_e::ARCH_X86_64);
	std::vector<VmHandlerTrace> basicBlocks;
	VmHandlerTrace currentBlock;
	
	
	std::string memoryRead;
	std::string registersState;
	std::string line;
	while (std::getline(traceFile, line)){
		if (line._Starts_with("mr:")) {
			memoryRead = line.substr(3);
			continue;
		}
		else if (line._Starts_with("r:")) {
			registersState = line.substr(2);
			continue;
		}
		else if (!line._Starts_with("i:")) {
			memoryRead.clear();
			registersState.clear();
			continue;
		}
		NativeInstructionContext instr(tempContext, line.substr(2), std::move(registersState), std::move(memoryRead));
		bool transferToNewHandler = IsNewHandlerTransfer(*instr.instruction);
		currentBlock.instructions.push_back(std::move(instr));
		if (transferToNewHandler) {
			basicBlocks.push_back(std::move(currentBlock));
			currentBlock = VmHandlerTrace();
		}
	}

	return basicBlocks;
}

void PrintBasicBlock(const VmHandlerTrace& block) {
	for (auto& inst : block.instructions) {
		std::cout << inst.instruction << '\n';
	}
}