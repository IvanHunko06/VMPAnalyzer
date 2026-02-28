#include "VmEmulator.hpp"
#include "LLVMFunctionLifter.hpp"
#include "VmJitRunner.hpp"
#include <map>

std::vector<LLVMFunctionLifter> LiftAllFunctions(const std::vector<VirtualBasicBlock>& blocks,
	llvm::LLVMContext* llvmContext,
	llvm::Module* llvmModule,
	llvm::IRBuilder<>* llvmBuilder,
	bool isDebugBuild) {

	std::vector<LLVMFunctionLifter> functions;
	int index = 0;

	for (auto& block : blocks) {
		LLVMFunctionLifter function(llvmContext, llvmModule, llvmBuilder, isDebugBuild, isDebugBuild);
		function.LiftVirtualCode("ProtectedFunction_" + std::to_string(index++), block.instructions);
		functions.push_back(std::move(function));
	}
	OptimizeModule(*llvmModule, true);

	return functions;
}


std::map<uint64_t, uint8_t> virtualMemory;
std::vector<uint64_t> stack(250);

void SetVirtualMemoryValue(uint64_t addr, uint64_t value, uint32_t size) {
	// 1. Вычисляем границы реального стека в памяти
	uint64_t stackBase = reinterpret_cast<uint64_t>(stack.data());
	uint64_t stackSizeBytes = stack.size() * sizeof(uint64_t);
	uint64_t stackEnd = stackBase + stackSizeBytes;

	// 2. Проверяем, попадает ли диапазон [addr, addr + size) целиком в стек
	if (addr >= stackBase && (addr + size) <= stackEnd) {
		// Прямая запись в память вектора (быстро)
		std::memcpy(reinterpret_cast<void*>(addr), &value, size);
	}
	else {
		// Запись в эмулируемую память (медленно, побайтово)
		// Реализуем Little-Endian (младший байт по меньшему адресу)
		for (uint32_t i = 0; i < size; ++i) {
			uint8_t byte = (value >> (i * 8)) & 0xFF;
			virtualMemory[addr + i] = byte;
		}
	}
}
uint64_t GetVirtualMemoryValue(uint64_t addr, uint32_t size) {
	uint64_t result = 0;

	// 1. Границы стека
	uint64_t stackBase = reinterpret_cast<uint64_t>(stack.data());
	uint64_t stackSizeBytes = stack.size() * sizeof(uint64_t);
	uint64_t stackEnd = stackBase + stackSizeBytes;

	// 2. Проверка на стек
	if (addr >= stackBase && (addr + size) <= stackEnd) {
		// Прямое чтение из памяти вектора
		std::memcpy(&result, reinterpret_cast<void*>(addr), size);
	}
	else {
		// Чтение из эмулируемой памяти
		// Собираем число из байтов (Little-Endian)
		for (uint32_t i = 0; i < size; ++i) {
			// Если страницы нет в map, map вернет 0 (default construct для uint8_t), 
			// что соответствует поведению чтения неинициализированной памяти как 0
			if (virtualMemory.count(addr + i)) {
				uint8_t byte = virtualMemory[addr + i];
				result |= (static_cast<uint64_t>(byte) << (i * 8));
			}
		}
	}

	return result;
}

void DumpStack(uint64_t* begin, uint64_t* end) {
	if (begin > end) std::swap(begin, end);

	for (uint64_t* i = begin; i < end; ++i) {
		std::cout << std::hex << "0x" << *i << '\n';
	}
	std::cout << std::dec << "\n\n";
}

void PrintNativeContext(const NativeContext& context) {
	std::cout << std::hex
		<< "RAX: 0x" << context.raxReg << '\n'
		<< "RBX: 0x" << context.rbxReg << '\n'
		<< "RCX: 0x" << context.rcxReg << '\n'
		<< "RDX: 0x" << context.rdxReg << '\n'
		<< "RDI: 0x" << context.rdiReg << '\n'
		<< "RSI: 0x" << context.rsiReg << '\n'
		<< "RBP: 0x" << context.rbpReg << '\n'
		<< "R8: 0x" << context.r8Reg << '\n'
		<< "R9: 0x" << context.r9Reg << '\n'
		<< "R10: 0x" << context.r10Reg << '\n'
		<< "R11: 0x" << context.r11Reg << '\n'
		<< "R12: 0x" << context.r12Reg << '\n'
		<< "R13: 0x" << context.r13Reg << '\n'
		<< "R14: 0x" << context.r14Reg << '\n'
		<< "R15: 0x" << context.r15Reg << '\n'
		<< "RFlags: 0x" << context.rflagsReg << '\n'
		<< "RetAddr: 0x" << context.retAddr << '\n'
		<< "VmExitAddr: 0x" << context.vmExitAddr << "\n\n";
}

void RunEmulation(VmJitRunner& runner, const VmEntryHandlerData& vmEntryData, const std::map<uint64_t, std::string> virtualAddrToFunctionMap) {
	NativeContext context;
	for (auto& reg : vmEntryData.pushRegsOrder) {
		auto offset = GetNativeOffset(reg.id);
		if (offset == -1) continue;
		uint64_t* contexti64Ptr = reinterpret_cast<uint64_t*>(&context);
		contexti64Ptr[offset] = reg.value;
	}
	NativeContext startNativeContext = context;
	uint64_t* vsp = &stack[249];

	PrintNativeContext(startNativeContext);

	std::string currentFunctionName = "ProtectedFunction_0";
	uint64_t nextVip = 0;
	uint64_t* startVsp = vsp;
	do {
		auto func = runner.GetFuncAddress(currentFunctionName);
		nextVip = func(reinterpret_cast<uint64_t*>(&context), &vsp, vmEntryData.imageBaseDifference);
		if (nextVip) {
			auto it = virtualAddrToFunctionMap.find(nextVip);
			if (it == virtualAddrToFunctionMap.end()) nextVip = 0;
			else currentFunctionName = it->second;
		}
		//DumpStack(startVsp, vsp);
	} while (nextVip);
	
	PrintNativeContext(context);
}

VmJitRunner CreateDebugRunner(const std::vector<VirtualBasicBlock> blocks) {
	using namespace llvm;

	auto llvmCtx = std::make_unique<LLVMContext>();
	auto llvmModule = std::make_unique<Module>("vm_lifted", *llvmCtx);
	auto llvmBuilder = std::make_unique<IRBuilder<>>(*llvmCtx);

	std::vector<LLVMFunctionLifter> functions = LiftAllFunctions(
		blocks, llvmCtx.get(),
		llvmModule.get(), llvmBuilder.get(),
		true
	);

	PrintLlvmModuleToConsole(*llvmModule);

	VmJitRunner runner;
	runner.AddModule(std::move(llvmModule), std::move(llvmCtx));

	return runner;
}

void DumpReleaseBuild(const std::vector<VirtualBasicBlock> blocks, const char* path) {
	using namespace llvm;

	auto llvmCtx = std::make_unique<LLVMContext>();
	auto llvmModule = std::make_unique<Module>("vm_lifted", *llvmCtx);
	auto llvmBuilder = std::make_unique<IRBuilder<>>(*llvmCtx);

	std::vector<LLVMFunctionLifter> functions = LiftAllFunctions(
		blocks, llvmCtx.get(),
		llvmModule.get(), llvmBuilder.get(),
		false
	);

	DumpLlvmModuleToFile(*llvmModule, "output.ll");
}

void EmulateVmCode(const std::vector<HandlerMatch>& handlers) {
	auto& vmEntry = handlers[0];
	if (vmEntry.type != Handler_VmEntry) std::exit(-1);

	auto& vmEntryData = std::get<VmEntryHandlerData>(vmEntry.matchData);
	auto virtualBlocks = SplitToVirtualBlocks(handlers);

	using namespace llvm;

	VmJitRunner runner = CreateDebugRunner(virtualBlocks);

	std::map<uint64_t, std::string> handlerFunctionNames;
	std::set<uint64_t> definedAddresses;
	for (int i = 0; i < virtualBlocks.size(); ++i) {
		auto& block = virtualBlocks[i];
		auto& lastInstr = block.instructions.back();
		if (lastInstr.type == Handler_VmJmpIndirect) {
			auto jmpData = std::get<VmJmpData>(lastInstr.matchData);
			handlerFunctionNames[jmpData.newVip] = "ProtectedFunction_" + std::to_string(i + 1);
		}
		for (auto& instr : block.instructions) {
			if (instr.type != Handler_VmReadMem) continue;

			auto& readMemData = std::get<VmMemAccessData>(instr.matchData);
			//if (readMemData.address < defaultImageBase + vmEntryData.imageBaseDifference) continue;
			if (definedAddresses.contains(readMemData.address)) continue;
			SetVirtualMemoryValue(readMemData.address, readMemData.value, instr.bitDepth / 8);
			definedAddresses.insert(readMemData.address);
		}
	}

	RunEmulation(runner, vmEntryData, handlerFunctionNames);

	DumpReleaseBuild(virtualBlocks, "output.ll");
}