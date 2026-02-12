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
		bool isInStackAddress{ false }; // Является ли это значение адресом в стеке (для оптимизаций доступа к стеку)
		ShadowStackSlot* isPointerToVirtualStackSlot{ nullptr }; // Если это значение является указателем на виртуальный стек, сохраняем указание на соответствующий слот
		std::optional<FlagsPromise> flagsPromise; // Если этот слот связан с операцией, которая обещает флаги, сохраняем эту информацию здесь
	};
	struct ShadowStackPop {
		llvm::Value* value{ nullptr };
		bool isInStackAddress{ false }; // Является ли это значение адресом в стеке (для оптимизаций доступа к стеку)
		ShadowStackSlot* isPointerToVirtualStackSlot{ nullptr }; // Если это значение является указателем на виртуальный стек, сохраняем указание на соответствующий слот
		std::optional<FlagsPromise> flagsPromise; // Если этот слот связан с операцией, которая обещает флаги, сохраняем эту информацию здесь
	};
	std::deque<ShadowStackSlot> shadowStack;
private:
	struct ShadowContextPartialWrite {
		int32_t offset{ 0 };
		int32_t width{ 0 };
		llvm::Value* value{ nullptr };
	};
	struct ShadowContextSlot {
		llvm::Value* baseValue{ nullptr };
		ShadowStackSlot* isVspPointer{ nullptr }; // Является ли это значение указателем на стек (для оптимизаций доступа к стеку)
		bool isInStackPointer{ false }; // Является ли это значение указателем, который может указывать в стек (для оптимизаций доступа к стеку)
		std::vector<ShadowContextPartialWrite> partialWrites;
	};
	std::map<int32_t, ShadowContextSlot> shadowContext; // Ключ - смещение в структуре NativeContext
	std::map<int32_t, FlagsPromise> registerFlagsPromises; // Если регистр, который обещает флаги, был перезаписан, сохраняем информацию о том, какие флаги он обещал, чтобы можно было попытаться восстановить эти флаги при необходимости

public:
	LLVMBasicBlockLifter(llvm::LLVMContext* context, llvm::Module* llvmModule, llvm::IRBuilder<>* builder, llvm::Function* function,
		llvm::Value* vsp_ptr, llvm::Value* vspBasePtr, llvm::Value* nativeContext, llvm::Type* nativeContextType, llvm::Value* imageBaseDif,
		llvm::Value* targetVip);
	
	llvm::BasicBlock* LiftBasicBlock(const VirtualBasicBlock& vbb, bool useMemoryHooks = true, bool logMessages = false);
	void FlushVsp(bool clearStack);
private:
#pragma region JIT Functions
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
#pragma endregion
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

#pragma region Shadow Stack Operations
	void PushWithConstOffset(llvm::Value* value, HandlerBitDepth bitDepth);
	void ShadowPush(llvm::Value* value, HandlerBitDepth bitDepth);
	ShadowStackPop ShadowPop(HandlerBitDepth bitDepth);
	llvm::Value* PopWithConstOffset(HandlerBitDepth bitDepth);
#pragma endregion

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

#pragma region Stack Operations
	void LiftVmPop(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmPushReg(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmPushConst(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmPushVsp(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmPopVsp(bool logDebugMessage, const HandlerMatch& match);
#pragma endregion
	
#pragma region Memory Accesses
	void LiftVmReadMem(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmReadMemHooked(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmWriteMem(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmWriteMemHooked(bool logDebugMessage, const HandlerMatch& match);
#pragma endregion

#pragma region ALU Operations
	void LiftVmAdd(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmNor(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmNand(bool logDebugMessage, const HandlerMatch& match);

	void LiftVmShl(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmShr(bool logDebugMessage, const HandlerMatch& match);

	void LiftVmShld(bool logDebugMessage, const HandlerMatch& match);
	void LiftVmShrd(bool logDebugMessage, const HandlerMatch& match);
#pragma endregion

	void LiftVmJmpIndirect(bool logDebugMessage, const VmJmpData& jmpData);
	void LiftVmExit(bool logDebugMessage, const VmExitData& data);
};