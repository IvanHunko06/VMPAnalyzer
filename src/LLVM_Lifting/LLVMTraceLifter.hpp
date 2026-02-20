#pragma once
#include "../VmHandlerMatcher.hpp"
#include <map>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include "LLVMBasicBlockLifter.hpp"

class CFGRepatcher;

class LLVMTraceLifter {
private:
	std::unique_ptr<llvm::LLVMContext> llvmContext;
	std::unique_ptr<llvm::Module> llvmModule;
	std::unique_ptr<llvm::IRBuilder<>> llvmIrBuilder;
	std::unique_ptr<LLVMBasicBlockLifter> basicBlockLifter;

	llvm::Function* function;
	std::map<uint64_t, llvm::BasicBlock*> virtualBasicBlockMap;

	llvm::BasicBlock* entryBasicBlock;
	llvm::BasicBlock* dispatchBlock;
	llvm::BasicBlock* exitBlock;

	llvm::Type* i64;
	llvm::Type* i32;
	llvm::Type* i16;
	llvm::Type* i8;
	llvm::Type* voidTy;
	llvm::PointerType* ptrTy;

private:
	llvm::StructType* nativeContextType;
	llvm::Value* nativeContextPtr;
	llvm::Value* imageBaseDif;
	llvm::Value* realStackPtr;
	llvm::AllocaInst* targetVip;
	llvm::SwitchInst* dispatchSwitch;
	llvm::BasicBlock* protectedCodeEntryBlock;
	friend class CFGRepatcher;

public:
	LLVMTraceLifter(const std::vector<VirtualBasicBlock> basicBlocks);
	void OptimizeModule(bool enableO3Optimization);
	void PrintModule() {
		llvm::outs() << "\n[LLVM IR DUMP START]\n";
		llvmModule->print(llvm::outs(), nullptr);
		llvm::outs() << "\n[LLVM IR DUMP END]\n";
	}
	void PrintBasicBlock(llvm::BasicBlock* bb) {
		llvm::outs() << "\n[Basic Block Dump Start]\n";
		bb->print(llvm::outs());
		llvm::outs() << "\n[Basic Block Dump End]\n";
	}
	void DumpModuleToFile(const std::string& filePath) {
		std::error_code EC;
		llvm::raw_fd_ostream file(filePath, EC);
		llvmModule->print(file, nullptr);
	}
private:
	void CreateNativeContextType();
	void CreateFunction(const char* functionName);
	void LiftTraceFunction(const std::vector<VirtualBasicBlock> basicBlocks);
};