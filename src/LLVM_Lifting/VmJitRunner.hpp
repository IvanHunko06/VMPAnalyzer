#pragma once
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/TargetSelect.h>

extern "C" {
	uint64_t LLVM_JIT_ReadMem(uint64_t address, uint32_t size);
	void LLVM_JIT_WriteMem(uint64_t address, uint64_t value, uint32_t size);


	void LLVM_JIT_LogPop(uint32_t bitDepth, uint64_t offset, uint64_t value);
	void LLVM_JIT_LogPushReg(uint32_t bitDepth, uint64_t offset, uint64_t value);
	void LLVM_JIT_LogPushConst(uint32_t bitDepth, uint64_t value);

	void LLVM_JIT_LogPushVsp(uint32_t bitDepth, uint64_t value);
	void LLVM_JIT_LogPopVsp(uint32_t bitDepth, uint64_t value);

	void LLVM_JIT_LogReadMem(uint32_t bitDepth, uint64_t addr, uint64_t value);
	void LLVM_JIT_LogWriteMem(uint32_t bitDepth, uint64_t addr, uint64_t value);

	void LLVM_JIT_LogAdd(uint32_t bitDepth, uint64_t arg1, uint64_t arg2, uint64_t result, uint64_t flags);
	void LLVM_JIT_LogNor(uint32_t bitDepth, uint64_t arg1, uint64_t arg2, uint64_t result, uint64_t flags);
	void LLVM_JIT_LogNand(uint32_t bitDepth, uint64_t arg1, uint64_t arg2, uint64_t result, uint64_t flags);
	void LLVM_JIT_LogShl(uint32_t bitDepth, uint64_t arg1, uint64_t arg2, uint64_t result, uint64_t flags);
	void LLVM_JIT_LogShr(uint32_t bitDepth, uint64_t arg1, uint64_t arg2, uint64_t result, uint64_t flags);

	void LLVM_JIT_LogShld(uint32_t bitDepth, uint64_t dst, uint64_t src, uint8_t shift, uint64_t result, uint64_t flags);
	void LLVM_JIT_LogShrd(uint32_t bitDepth, uint64_t dst, uint64_t src, uint8_t shift, uint64_t result, uint64_t flags);

	void LLVM_JIT_LogJmpIndirect(uint64_t dst);
}

using LLVM_FunctionSignature = uint64_t(*)(uint64_t*, uint64_t**, int64_t);

class VmJitRunner {
	std::unique_ptr<llvm::orc::LLJIT> jit;
public:
	VmJitRunner() {
		llvm::InitializeNativeTarget();
		llvm::InitializeNativeTargetAsmPrinter();
		llvm::InitializeNativeTargetAsmParser();

		auto builder = llvm::orc::LLJITBuilder();

		auto jitExpected = builder.create();
		if (!jitExpected) {
			llvm::errs() << "Failed to create JIT: " << toString(jitExpected.takeError()) << "\n";
			exit(1);
		}

		jit = std::move(*jitExpected);

		// 4. Теперь jit валиден, можно регистрировать хуки
		RegisterHooks();
	}
	void AddModule(std::unique_ptr<llvm::Module> M, std::unique_ptr<llvm::LLVMContext> C) {
		cantFail(jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(M), std::move(C))));
	}
	LLVM_FunctionSignature GetFuncAddress(const std::string& name) {
		auto sym = jit->lookup(name);
		if (!sym) return 0;
		return reinterpret_cast<LLVM_FunctionSignature>(sym->getAddress());
	}
private:
	void RegisterHooks();

};
