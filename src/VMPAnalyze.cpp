#include "triton/context.hpp"
#include "VmHandlerTrace.hpp"
#include "VmHandlerEmulator.hpp"
#include "VmHandlerMatcher.hpp"
#include "HandlerCachedStorage.hpp"
#include "LLVM_Lifting/LLVMTraceLifter.hpp"
#include "LLVM_Lifting/CFGRepatcher.hpp"
#include <set>
#include <fstream>
#include <execution>
#include <vector>
#include <algorithm>

int main() {

 	std::fstream file("E:/Programs/pin-external-3.31-98869-gfa6f126a8-msvc-windows/trace5.log", std::ios::in);
	
	auto handlerTraces = ParseVmHandlerTraces(file);

	triton::arch::register_e vipReg = triton::arch::ID_REG_INVALID;
	triton::arch::register_e vspReg = triton::arch::ID_REG_INVALID;

	std::set<uint64_t> breakpoints{};
	HandlerCachedStorage cacheStorage(false);

	std::vector<HandlerMatch> vmHandlers;
	for (auto& trace : handlerTraces) {
		HandlerMatch currentHandler;
		auto addr = trace.instructions[0].GetAddress();
		auto cachedHandler = cacheStorage.TryGetCachedInstruction(addr, trace, vipReg, vspReg);
		if (!cachedHandler) {
			auto data = EmulateVmHandler(trace, vipReg, vspReg);
			if (breakpoints.contains(data.baseAddress)) {
				PrintBasicBlock(trace);
				std::cout << '\n';
				PrintEmulationData(data);
				__debugbreak();
			}

			currentHandler = MatchVmHandler(data);
			if (currentHandler.type != Handler_VmEntry &&
				currentHandler.type != Handler_VmExit &&
				currentHandler.type != Handler_VmPopVsp) {

				cacheStorage.CacheInstruction(currentHandler);
			}
			
		}
		else {
			currentHandler = *cachedHandler;
		}

		
		vmHandlers.push_back(currentHandler);
		if (currentHandler.type == VmHandlerType::Handler_VmEntry) {
			auto matchData = std::get<VmEntryHandlerData>(currentHandler.matchData);
			vipReg = matchData.vipReg;
			vspReg = matchData.vspReg;
		}

		if (currentHandler.type == VmHandlerType::Handler_VmJmpIndirect) {
			auto matchData = std::get<VmJmpData>(currentHandler.matchData);
			vipReg = matchData.newVipReg;
			vspReg = matchData.newVspReg;
		}
	}

	auto virtualBlocks = SplitToVirtualBlocks(vmHandlers);

	for (auto& block : virtualBlocks) {
		std::cout << block << "\n\n";
	}


	
	LLVMTraceLifter lifter(virtualBlocks);
	//lifter.OptimizeModule(true);
	lifter.PrintModule();
	//CFGRepatcher::Patch(lifter);
	lifter.OptimizeModule(true);
	lifter.PrintModule();
	
	//lifter.OptimizeModule(true);
	//lifter.PrintModule();
	lifter.DumpModuleToFile("vm_lifted_module.ll");
	//EmulateVmCode(vmHandlers);
}