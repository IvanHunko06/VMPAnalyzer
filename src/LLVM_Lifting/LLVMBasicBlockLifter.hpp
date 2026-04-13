#pragma once
#include "../VmHandlerMatcher.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include "ShadowStack.hpp"

struct BasicBlockLifterConstructor {
	llvm::LLVMContext* context;
	llvm::Module* llvmModule;
	llvm::IRBuilder<>* builder;
	llvm::Function* function;

	llvm::Value* vsp_ptr;
	llvm::Value* vspBasePtr;
	llvm::Value* nativeContext;
	llvm::Type* nativeContextType;
	llvm::Value* virtualContext;
	llvm::Value* realStackPtr;

	llvm::Value* imageBaseDif;
	llvm::Value* targetVip;

	uint64_t aslrDifference{ 0 };
};

struct StackAddressMeta {
	bool isInStackAddress{ false }; // Является ли это значение адресом в стеке (для оптимизаций доступа к стеку)
	int32_t slotAbsoluteBase{ 0 };
	int32_t relativeOffset{ 0 };
};

struct BasicBlockLifterState {
	std::map<int32_t, StackAddressMeta> addressMetasStorage;
	int64_t virtualStackOffset = 0;
	int64_t realStackOffset{ 0 };
};

class LLVMBasicBlockLifter {
	llvm::LLVMContext* context;
	llvm::Module* llvmModule;
	llvm::IRBuilder<>* builder;
	llvm::Function* function;

	llvm::Value* vspPtr;
	llvm::Value* vspBasePtr;
	llvm::Value* virtualContext;
	llvm::Value* nativeContext;
	llvm::Type* nativeContextType;
	llvm::Value* imageBaseDif;
	llvm::Value* targetVip;
	llvm::Value* realStackPtr;
	uint64_t aslrDifference{ 0 };

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
	struct StackMetadata {
		bool isPadding{ false };
		StackAddressMeta addressMeta;
		std::optional<FlagsPromise> flagsPromise; // Если этот слот связан с операцией, которая обещает флаги, сохраняем эту информацию здесь
	};
	using ShadowStackType = ShadowStack<StackMetadata>;
	ShadowStackType shadowStack;
	static constexpr uint64_t fakeStackBase = 0x140000;


private:
	std::map<int32_t, StackAddressMeta> addressInRegMetas;
	std::map<int32_t, FlagsPromise> calculateFlagsPromises; // Если регистр, который обещает флаги, был перезаписан, сохраняем информацию о том, какие флаги он обещал, чтобы можно было попытаться восстановить эти флаги при необходимости
	std::map<int32_t, StackAddressMeta> addressMetasStorage;

	int64_t virtualStackOffset = 0;
	int64_t realStackOffset{ 0 };

public:
	LLVMBasicBlockLifter(const BasicBlockLifterConstructor& ctor);
	
	llvm::BasicBlock* LiftBasicBlock(const VirtualBasicBlock& vbb, bool useMemoryHooks = true, bool logMessages = false);
	void FlushVsp(bool clearStack, bool writeToRealStack);
	BasicBlockLifterState SaveState() {
		BasicBlockLifterState state;
		state.addressMetasStorage = addressMetasStorage;
		state.realStackOffset = realStackOffset;
		state.virtualStackOffset = virtualStackOffset;
		return state;
	}
	void ApplyState(const BasicBlockLifterState& state) {
		addressMetasStorage = state.addressMetasStorage;
		realStackOffset = state.realStackOffset;
		virtualStackOffset = state.virtualStackOffset;
	}

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
	llvm::Value* CreateValueByDepth(HandlerBitDepth depth, int64_t value) {
		switch (depth) {
		case BitDepth_8: return builder->getInt8(value);
		case BitDepth_16: return builder->getInt16(value);
		case BitDepth_32: return builder->getInt32(value);
		case BitDepth_64: return builder->getInt64(value);
		default: return builder->getIntN(depth, value);
		}
	}

#pragma region Shadow Stack Operations
	void PushWithConstOffset(llvm::Value* value, HandlerBitDepth bitDepth, bool actuallyWriteToRealStack);
	void Push(llvm::Value* value, HandlerBitDepth bitDepth) {
		StackMetadata meta;
		shadowStack.Push(bitDepth, value, meta);
	}
	
	llvm::Value* PopWithConstOffset(HandlerBitDepth bitDepth);
	ShadowStackType::PopType Pop(HandlerBitDepth bitDepth) {
		auto result = shadowStack.Pop(bitDepth);
		auto addrMeta = addressMetasStorage.find(virtualStackOffset - bitDepth);
		if (addrMeta != addressMetasStorage.end()) {
			result.metadata.addressMeta = addrMeta->second;
			addressMetasStorage.erase(addrMeta);
		}
		return result;
	}
#pragma endregion

	llvm::Value* CalculateFlagsFromPromise(FlagsPromise& promise);
	
	void InitJitHooks();
	void InitLogFunctions();
	llvm::Value* PackFlags(llvm::Value* res, llvm::Value* cf, llvm::Value* of, llvm::Value* sf = nullptr, llvm::Value* zf = nullptr);
	
	void GenerateMemoryAccess(
		llvm::Value* targetAddr,
		llvm::Value* realStackAddr,
		llvm::Type* dataType,
		llvm::Value* valueToWrite, // Если nullptr, то это READ
		bool useHooks,
		bool isProvenStackAddress);
	void GenerateStackMemoryAccess(
		llvm::Value* targetAddr,
		llvm::Value* realStackAddr,
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

	void LiftRdtsc(bool logDebugMessage);

	void LiftVmJmpIndirect(bool logDebugMessage, const VmJmpData& jmpData);
	void LiftVmExit(bool logDebugMessage, const VmExitData& data);
};