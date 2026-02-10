#pragma once
#include "../VmHandlerMatcher.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>

class LLVMBasicBlockLifter {
	llvm::LLVMContext* context;
	llvm::Module* llvmModule;
	llvm::IRBuilder<>* builder;
	llvm::Function* function;

	llvm::Value* vspPtr;
	llvm::Value* vspBasePtr;
	int64_t stackOffset = 0;
	llvm::Value* nativeContext;
	llvm::Type* nativeContextType;
	llvm::Value* imageBaseDif;
	llvm::Value* targetVip;
	llvm::Value* virtualContext;
private:
	llvm::Type* i64;
	llvm::Type* i32;
	llvm::Type* i16;
	llvm::Type* i8;
	llvm::Type* voidTy;
	llvm::PointerType* ptrTy;
private:
	struct FlagsPromise {
		llvm::Value* op1{ nullptr };
		llvm::Value* op2{ nullptr };
		llvm::Value* op3{ nullptr };
		llvm::Value* res{ nullptr };
		llvm::Type* opType{ nullptr };
		enum OperationType {
			ADD, NOR, NAND, SHL, SHR, SHLD, SHRD
		} operation;
	};
	struct ShadowStackSlot {
		llvm::Value* value{ nullptr };
		uint64_t bitDepth{ BitDepth_64 };
		bool isVspPush{ false };
		std::optional<FlagsPromise> flagsPromise; // Если этот слот связан с операцией, которая обещает флаги, сохраняем эту информацию здесь
	};
	struct ShadowStackPop{
		llvm::Value* value{ nullptr };
		bool isVspPush{ false };
		std::optional<FlagsPromise> flagsPromise;
	};
	std::map<uint64_t, FlagsPromise> flagCalculators;
	std::deque<ShadowStackSlot> shadowStack;

public:
	LLVMBasicBlockLifter(llvm::LLVMContext* context, llvm::Module* llvmModule, llvm::IRBuilder<>* builder, llvm::Function* function,
		llvm::Value* vsp_ptr, llvm::Value* vspBasePtr, llvm::Value* nativeContext, llvm::Type* nativeContextType, llvm::Value* imageBaseDif,
		llvm::Value* targetVip, llvm::Value* virtualContext);
	
	llvm::BasicBlock* LiftBasicBlock(const VirtualBasicBlock& vbb, bool useMemoryHooks = true, bool logMessages = false);
	void FlushVsp(bool clearStack);
	void SetStackOffset(int64_t offset) {
		stackOffset = offset;
	}
private:
	llvm::FunctionCallee jitReadFunc;
	llvm::FunctionCallee jitWriteFunc;

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
	llvm::Type* GetTypeByDepth(HandlerBitDepth depth) {
		switch (depth) {
		case BitDepth_8: return i8;
		case BitDepth_16: return i16;
		case BitDepth_32: return i32;
		case BitDepth_64: return i64;
		default: 
			return builder->getIntNTy(depth);
		}
	}
	llvm::Value* CreateValueByDepth(HandlerBitDepth depth, size_t value) {
		switch (depth) {
		case BitDepth_8: return builder->getInt8(value);
		case BitDepth_16: return builder->getInt16(value);
		case BitDepth_32: return builder->getInt32(value);
		case BitDepth_64: return builder->getInt64(value);
		default: return builder->getIntN(depth, value);
		}
	}
	void Push(llvm::Value* value, HandlerBitDepth bitDepth) {
		ShadowPush(value, bitDepth);
	}
	void PushWithConstOffset(llvm::Value* value, HandlerBitDepth bitDepth);
	void ShadowPush(llvm::Value* value, HandlerBitDepth bitDepth);
	llvm::Value* Pop(HandlerBitDepth bitDepth) {
		return ShadowPop(bitDepth).value;
	}
	ShadowStackPop ShadowPop(HandlerBitDepth bitDepth);
	llvm::Value* PopWithConstOffset(HandlerBitDepth bitDepth);
	llvm::Value* GetCurrentVspVal();
	llvm::Value* CalculateFlagsFromPromise(FlagsPromise& promise);
	
	void InitJitHooks();
	void InitLogFunctions();
	llvm::Value* PackFlags(llvm::Value* res, llvm::Value* cf, llvm::Value* of, llvm::Value* sf = nullptr, llvm::Value* zf = nullptr);
	void GenerateMemoryAccess(
		llvm::Value* targetAddr,
		llvm::Type* dataType,
		llvm::Value* valueToWrite, // Если nullptr, то это READ
		bool useHooks,
		bool isProvenStackAddress);
	void GenerateStackMemoryAccess(
		llvm::Value* targetAddr,
		llvm::Type* dataType,
		llvm::Value* valueToWrite, // Если nullptr, то это READ
		bool useHooks
	);
	void GenerateRamMemoryAccess(
		llvm::Value* targetAddr,
		llvm::Type* dataType,
		llvm::Value* valueToWrite, // Если nullptr, то это READ
		bool useHooks);

	void LiftVmEntry(const VmEntryHandlerData& data);
	void LiftVmPop(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmPushReg(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmPushConst(bool logDebugMessage, const HandlerMatch& match);

	void LiftVmPushVsp(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmPopVsp(bool logDebugMessage, const HandlerMatch& match);

	void LiftVmReadMem(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmReadMemHooked(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmWriteMem(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmWriteMemHooked(bool logDebugMessage, const HandlerMatch& match);

	void LiftVmAdd(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmNor(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmNand(bool logDebugMessage, const HandlerMatch& match);

	void LiftVmShl(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmShr(bool logDebugMessage, const HandlerMatch& match);

	void LiftVmShld(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmShrd(bool logDebugMessage, const HandlerMatch& match);

	void LiftVmJmpIndirect(bool logDebugMessage, const VmJmpData& jmpData);
	void LiftVmExit(bool logDebugMessage, const VmExitData& data);
};