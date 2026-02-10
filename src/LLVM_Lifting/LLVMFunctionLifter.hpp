#pragma once

#include "../VmHandlerMatcher.hpp"
#include "NativeContext.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <deque>

class LLVMFunctionLifter {
private:
	llvm::LLVMContext* ctx;
	llvm::Module* llvmModule;
	llvm::IRBuilder<>* builder;
	llvm::Type* i64;
	llvm::Type* i32;
	llvm::Type* i16;
	llvm::Type* i8;
	llvm::Type* voidTy;
	llvm::PointerType* ptrTy;

	llvm::Value* virtualContextPtr; // Аргумент 0: массив регистров
	llvm::Value* vspPtrAddr;   // Аргумент 1: адрес переменной стека
	llvm::Value* vspLocalVar;
	llvm::Value* nativeContextPtr;
	llvm::Value* imageBaseDif;
	llvm::Function* function;

	llvm::FunctionCallee jitReadFunc;
	llvm::FunctionCallee jitWriteFunc;

	struct ShadowStackSlot {
		llvm::Value* value;
		uint64_t bitDepth;
	};
	std::deque<ShadowStackSlot> shadowStack;
	uint64_t shadowStackOffset;

private:
	llvm::FunctionCallee jitLogPop;
	llvm::FunctionCallee jitLogPushReg;
	llvm::FunctionCallee jitLogPushConst;

	llvm::FunctionCallee jitLogPushVsp;
	llvm::FunctionCallee jitLogPopVsp;

	llvm::FunctionCallee jitLogReadMem;
	llvm::FunctionCallee jitLogWriteMem;

	llvm::FunctionCallee jitLogAdd;
	llvm::FunctionCallee jitLogNand;
	llvm::FunctionCallee jitLogNor;

	llvm::FunctionCallee jitLogShl;
	llvm::FunctionCallee jitLogShr;

	llvm::FunctionCallee jitLogShld;
	llvm::FunctionCallee jitLogShrd;

	llvm::FunctionCallee jitLogJmpIndirect;

private:
	bool jitHookMemoryAccess;
	bool jitDebugLogs;
public:
	LLVMFunctionLifter(
		llvm::LLVMContext* ctx,
		llvm::Module* llvmModule,
		llvm::IRBuilder<>* builder,
		bool jitHookMemoryAccess = true,
		bool jitDebugLogs = true
	);
	void LiftVirtualCode(const std::string& functionName, const std::vector<HandlerMatch> handlers);
	llvm::Function* GetFunction() {
		return function;
	}
private:
	void CreateFunction(const std::string& name);
	llvm::Value* Pop(HandlerBitDepth bitDepth);
	llvm::Value* LegacyPop(HandlerBitDepth bitDepth);
	llvm::Value* ShadowPop(HandlerBitDepth bitDepth);
	void Push(llvm::Value* value, HandlerBitDepth bitDepth);
	void LegacyPush(llvm::Value* value, HandlerBitDepth bitDepth);
	void ShadowPush(llvm::Value* value, HandlerBitDepth bitDepth);
	llvm::Type* GetTypeByDepth(HandlerBitDepth depth) {
		switch (depth) {
		case BitDepth_8: return i8;
		case BitDepth_16: return i16;
		case BitDepth_32: return i32;
		case BitDepth_64: return i64;
		default: return builder->getIntNTy(depth);
		}
	}
	llvm::Value* CreateValueByDepth(HandlerBitDepth depth, size_t value) {
		switch (depth) {
		case BitDepth_8: return builder->getInt8(value);
		case BitDepth_16: return builder->getInt16(value);
		case BitDepth_32: return builder->getInt32(value);
		default: return builder->getInt64(value);
		}
	}
	void FlushVspToMemory();
	void InitJitHooks();
	void InitLogFunctions();

private:
	void LiftVmEntry(const VmEntryHandlerData& data);
	void LiftVmPop(const HandlerMatch& match);
	void LiftVmPushReg(const HandlerMatch& match);
	void LiftVmPushConst(const HandlerMatch& match);

	void LiftVmPushVsp(const HandlerMatch& match);
	void LiftVmPopVsp(const HandlerMatch& match);

	void LiftVmReadMem(const HandlerMatch& match);
	void LiftVmReadMemHooked(const HandlerMatch& match);
	void LiftVmWriteMem(const HandlerMatch& match);
	void LiftVmWriteMemHooked(const HandlerMatch& match);

	void LiftVmAdd(const HandlerMatch& match);
	void LiftVmNor(const HandlerMatch& match);
	void LiftVmNand(const HandlerMatch& match);

	void LiftVmShl(const HandlerMatch& match);
	void LiftVmShr(const HandlerMatch& match);
	void LiftVmRol(const HandlerMatch& match);
	void LiftVmRor(const HandlerMatch& match);

	void LiftVmShld(const HandlerMatch& match);
	void LiftVmShrd(const HandlerMatch& match);

	void LiftVmJmpIndirect(const VmJmpData& jmpData);
	void LiftVmExit(const VmExitData& data);
};

void OptimizeModule(llvm::Module& llvmModule, bool enableOptimization);
static void PrintLlvmModuleToConsole(llvm::Module& llvmModule) {
	llvm::outs() << "\n[LLVM IR DUMP START]\n";
	llvmModule.print(llvm::outs(), nullptr);
	llvm::outs() << "\n[LLVM IR DUMP END]\n";
}
static void DumpLlvmModuleToFile(llvm::Module& llvmModule, const char* path) {
	std::error_code EC;
	llvm::raw_fd_ostream file(path, EC);
	llvmModule.print(file, nullptr);
}