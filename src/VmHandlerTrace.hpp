#pragma once
#include <vector>
#include <fstream>
#include "NativeInstructionContext.hpp"

struct VmHandlerTrace {
	std::vector<NativeInstructionContext> instructions;
};

std::vector<VmHandlerTrace> ParseVmHandlerTraces(std::fstream& traceFile);
void PrintBasicBlock(const VmHandlerTrace& block);