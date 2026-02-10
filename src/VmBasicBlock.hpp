#pragma once
#include <vector>
#include <fstream>
#include "NativeInstructionContext.hpp"

struct VmBasicBlock {
	std::vector<NativeInstructionContext> instructions;
};

std::vector<VmBasicBlock> ParseVmBasicBlocks(std::fstream& traceFile);
void PrintBasicBlock(const VmBasicBlock& block);