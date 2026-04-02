#include "triton/context.hpp"
#include "VmHandlerTrace.hpp"
#include "VmHandlerEmulator.hpp"
#include "VmHandlerMatcher.hpp"
#include "HandlerCachedStorage.hpp"
#include "LLVM_Lifting/LLVMTraceLifter.hpp"
#include <set>
#include <fstream>
#include <execution>
#include <vector>
#include <algorithm>
void PrintHelp() {
	std::cout << "-input <path>  - path to the trace file\n";
	std::cout << "-use_cache     - use cache for fast matching. Not all handlers will have full data available.\n";
	std::cout << "-print_blocks  - displays all found virtual blocks\n";
	std::cout << "-output <path> - path to the output IR file\n";
}
std::string ReadPath(char** args, int argsCount, int* curArgIndex, const char* pathName) {
	const char* curArg = args[*curArgIndex];
	if (strcmp(curArg, pathName) == 0) {
		if (*curArgIndex + 1 < argsCount) {
			char* path = args[++*curArgIndex];
			if (path[0] == '-') throw std::runtime_error("Invalid path argument");
			if (path[0] == ' ') throw std::runtime_error("Invalid path argument");
			++*curArgIndex;
			return path;
		}
	}

	return "";
}

bool ReadFlag(char** args, int argsCount, int* curArgIndex, const char* flagName) {
	const char* curArg = args[*curArgIndex];
	if (strcmp(curArg, flagName) == 0) {
		*curArgIndex += 1;
		return true;
	}

	return false;
}

int main(int argc, char* argv[]) {
	std::vector<std::string> inputFiles;
	std::string outputFile = "vm_lifted_module.ll";
	bool useCaching = false;
	bool printBasicBlocks = false;

	for (int i = 1; i < argc; ) {
		auto inputPath = ReadPath(argv, argc, &i, "-input");
		if (!inputPath.empty()) {
			inputFiles.push_back(inputPath);
			continue;
		}
		auto outputPath = ReadPath(argv, argc, &i, "-output");
		if (!outputPath.empty()){
			outputFile = outputPath;
			continue;
		}

		if (ReadFlag(argv, argc, &i, "-use_cache")) {
			useCaching = true;
			continue;
		}

		if (ReadFlag(argv, argc, &i, "-print_blocks")) {
			printBasicBlocks = true;
			continue;
		}

		PrintHelp();
		return -1;
	}

	HandlerCachedStorage cacheStorage(useCaching);
	std::list<VirtualBasicBlock> uniqueBasicBlocks;
	std::map<VirtualBasicBlock*, std::vector<VirtualBasicBlock*>> blockTransitions;
	std::mutex mutex;
	std::mutex printErrorMutex;

	std::for_each(std::execution::par, inputFiles.begin(), inputFiles.end(), [&](const std::string& inputFile) {
		std::fstream file(inputFile, std::ios::in);
		auto handlerTraces = ParseVmHandlerTraces(file);

		triton::arch::register_e vipReg = triton::arch::ID_REG_INVALID;
		triton::arch::register_e vspReg = triton::arch::ID_REG_INVALID;

		std::vector<HandlerMatch> vmHandlers;
		for (auto& trace : handlerTraces) {
			HandlerMatch currentHandler;
			auto addr = trace.instructions[0].GetAddress();
			auto cachedHandler = cacheStorage.TryGetCachedInstruction(addr, trace, vipReg, vspReg);
			if (!cachedHandler) {
				auto data = EmulateVmHandler(trace, vipReg, vspReg);

				currentHandler = MatchVmHandler(data, trace);
				if (currentHandler.type == Handler_Unknown) {
					std::lock_guard<std::mutex> lock(printErrorMutex);
					PrintBasicBlock(trace);
					std::cout << '\n';
					PrintEmulationData(data);
					continue;
				}

				//if (currentHandler.type == Handler_VmJmpIndirect) {
				//	std::lock_guard<std::mutex> lock(printErrorMutex);
				//	PrintBasicBlock(trace);
				//	std::cout << '\n';
				//	PrintEmulationData(data);
				//}

				if (currentHandler.type != Handler_VmEntry &&
					currentHandler.type != Handler_VmExit &&
					currentHandler.type != Handler_VmPopVsp &&
					currentHandler.type != Handler_Unknown) {

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

			if (currentHandler.type == Handler_VmExit) {
				break;
			}
		}

		auto virtualBlocks = SplitToVirtualBlocks(vmHandlers);

		std::lock_guard<std::mutex> lock(mutex);
		
		VirtualBasicBlock* prevGlobalBlock = nullptr;
		for (auto& localBlock : virtualBlocks) {
			VirtualBasicBlock* currentGlobalBlock = nullptr;

			// 1. Ищем, существует ли уже этот блок в глобальном графе
			// (Если вы перейдете на std::map<uint64_t, VirtualBasicBlock>, этот поиск станет мгновенным O(1))
			auto it = std::find_if(uniqueBasicBlocks.begin(), uniqueBasicBlocks.end(),
				[&](const VirtualBasicBlock& globalBlock) {
					return globalBlock.startAddr == localBlock.startAddr;
				});

			if (it != uniqueBasicBlocks.end()) {
				// Блок уже существует (например, общая часть трассы до развилки)
				currentGlobalBlock = &(*it);
			}
			else {
				// Блок встретился впервые! Добавляем его в глобальный пул
				uniqueBasicBlocks.push_back(std::move(localBlock));
				currentGlobalBlock = &uniqueBasicBlocks.back();
			}

			// 2. Строим карту переходов (Связываем prev и current)
			if (prevGlobalBlock != nullptr) {
				auto& successors = blockTransitions[prevGlobalBlock];

				// Проверяем, нет ли уже такого ребра в графе, чтобы избежать дубликатов 
				// (ведь мы могли уже проходить этот путь в другой трассе)
				if (std::find(successors.begin(), successors.end(), currentGlobalBlock) == successors.end()) {
					successors.push_back(currentGlobalBlock);
				}
			}

			// Сдвигаем указатель для следующей итерации
			prevGlobalBlock = currentGlobalBlock;
		}

	});
	

	if (printBasicBlocks) {
		for (auto& block : uniqueBasicBlocks) {
			std::cout << block << "\n\n";
		}
		for (auto& [source, dsts] : blockTransitions) {
			std::cout << std::hex << "0x" << source->startAddr << "->[";
			for (auto& dst : dsts) {
				std::cout << "0x" << dst->startAddr << ", ";
			}
			std::cout << "]\n";
		}
	}
	


	
	LLVMTraceLifter lifter(blockTransitions, &uniqueBasicBlocks.front());
	lifter.OptimizeModule(true);
	lifter.PrintModule();
	
	lifter.DumpModuleToFile(outputFile);
}