#pragma once
#include "LLVMTraceLifter.hpp"
#include <llvm/IR/Instructions.h>
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Value.h"

class CFGRepatcher {
public:
	static void Patch(LLVMTraceLifter& lifter);
private:
	static void TryPatchBlock(LLVMTraceLifter& lifter, llvm::BasicBlock* bb);
	static llvm::StoreInst* FindVipStore(LLVMTraceLifter& lifter, llvm::BasicBlock* bb);
	static void ReplaceWithDirectJump(LLVMTraceLifter& lifter, llvm::BasicBlock* bb, llvm::StoreInst* storeToRemove, llvm::BasicBlock* targetBB);
	static bool MatchImageBaseAdd(llvm::Value* val, uint64_t& outVip);
	static void RemoveCaseFromSwitch(llvm::SwitchInst* switchInst, uint64_t targetVip);
};