#include "LLVMTraceLifter.hpp"
#include <llvm/IR/Verifier.h>

#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/AliasAnalysis.h>

#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h> // Не подключаем, но знаем о нем
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

LLVMTraceLifter::LLVMTraceLifter(const std::vector<VirtualBasicBlock> blocks) {
	using namespace llvm;

	llvmContext = std::make_unique<LLVMContext>();
	llvmModule = std::make_unique<Module>("vm_lifted", *llvmContext);
	llvmIrBuilder = std::make_unique<IRBuilder<>>(*llvmContext);

	i64 = Type::getInt64Ty(*llvmContext);
	i32 = Type::getInt32Ty(*llvmContext);
	i16 = Type::getInt16Ty(*llvmContext);
	i8 = Type::getInt8Ty(*llvmContext);
	voidTy = Type::getVoidTy(*llvmContext);
	ptrTy = Type::getInt64PtrTy(*llvmContext);

	CreateNativeContextType();
	CreateFunction("ProtectedTrace");
	LiftTraceFunction(blocks);
}

void LLVMTraceLifter::CreateNativeContextType() {
	std::vector<llvm::Type*> elements;
	for (int i = 0; i < 18; i++) elements.push_back(i64);
	nativeContextType = llvm::StructType::create(*llvmContext, elements, "NativeContext");
}

void LLVMTraceLifter::CreateFunction(const char* functionName) {
	using namespace llvm;

	auto* funcType = FunctionType::get(voidTy, {nativeContextType->getPointerTo(), i64}, false);
	function = Function::Create(funcType, llvm::Function::ExternalLinkage, functionName, *llvmModule);

	auto argsIt = function->arg_begin();
	nativeContextPtr = argsIt++;
	nativeContextPtr->setName("native_context_ptr");

	imageBaseDif = argsIt++;
	imageBaseDif->setName("image_base_dif");
}

void LLVMTraceLifter::LiftTraceFunction(const std::vector<VirtualBasicBlock> basicBlocks) {
	using namespace llvm;
	entryBasicBlock = BasicBlock::Create(*llvmContext, "entry", function);
	dispatchBlock = BasicBlock::Create(*llvmContext, "dispatch_loop", function);
	exitBlock = BasicBlock::Create(*llvmContext, "function_exit", function);

	// --- Entry Block ---
	llvmIrBuilder->SetInsertPoint(entryBasicBlock);

	// Аллокации переменных
	llvm::Type* vmStackType = llvm::ArrayType::get(i8, 4096);
	auto* vsp = llvmIrBuilder->CreateAlloca(vmStackType, nullptr, "vsp_array");
	llvmIrBuilder->CreateMemSet(vsp, llvmIrBuilder->getInt8(0), 4096, llvm::MaybeAlign(1));

	auto* vsp_ptr = llvmIrBuilder->CreateConstInBoundsGEP2_32(
		vmStackType,
		vsp,
		0,
		2048, // <--- ВОТ ОНО, МАГИЧЕСКОЕ ЧИСЛО
		"vsp_base_middle"
	);

	targetVip = llvmIrBuilder->CreateAlloca(i64, nullptr, "target_vip");
	if (!basicBlocks.empty()) {
		llvmIrBuilder->CreateStore(
			llvmIrBuilder->getInt64(basicBlocks[0].startAddr), 
			targetVip
		);
	}

	// --- Dispatcher Block ---
	llvmIrBuilder->SetInsertPoint(dispatchBlock);
	Value* currentVip = llvmIrBuilder->CreateLoad(i64, targetVip, "current_vip");
	dispatchSwitch = llvmIrBuilder->CreateSwitch(currentVip, exitBlock, basicBlocks.size());

	dispatchSwitch->addCase(llvmIrBuilder->getInt64(0), exitBlock);

	// --- Init Lifter ---
	basicBlockLifter = std::make_unique<LLVMBasicBlockLifter>(
		llvmContext.get(),
		llvmModule.get(),
		llvmIrBuilder.get(),
		function,
		vsp_ptr, // передаем аллоку указателя
		vsp,     // передаем базу стека
		nativeContextPtr,
		nativeContextType,
		imageBaseDif,
		targetVip
	);

	std::map<uint64_t, VirtualBasicBlock*> vipToBlockMap;
	std::vector<uint64_t> vipsOrder;

	for (int i = 0; i < basicBlocks.size(); i++) {
		uint64_t blockVip = basicBlocks[i].startAddr;
		if (i > 0) {
			blockVip -= basicBlocks[i - 1].nextVipShift;
		}
		virtualBasicBlockMap[blockVip] = nullptr;
		vipToBlockMap[blockVip] = const_cast<VirtualBasicBlock*>(&basicBlocks[i]);
		vipsOrder.push_back(blockVip);
	}

	// --- Lifting Blocks ---
	for (auto& vip : vipsOrder) {
		auto& bb = virtualBasicBlockMap[vip];
		if (bb != nullptr) continue; // Уже обработано
		auto& vbb = *vipToBlockMap[vip];
		//basicBlockLifter->SetStackOffset(vbb.stackOffset);

		bb = basicBlockLifter->LiftBasicBlock(vbb, true, false);
		if (vbb.instructions[0].type == Handler_VmEntry) {
			protectedCodeEntryBlock = bb;
		}

		dispatchSwitch->addCase(llvmIrBuilder->getInt64(vip), bb);

		// 3. В конце блока (если там еще нет терминатора) прыгаем обратно в диспетчер
		// Примечание: VM_EXIT и JMP_INDIRECT сами управляют потоком, но
		// структурно блок должен заканчиваться переходом.
		llvm::BasicBlock* currentTailBlock = llvmIrBuilder->GetInsertBlock();
		if (!currentTailBlock->getTerminator()) {
			//currentVip = llvmIrBuilder->CreateLoad(i64, targetVip, "current_vip");
			llvmIrBuilder->CreateBr(dispatchBlock);
		}

		PrintBasicBlock(bb);
	}

	if (protectedCodeEntryBlock) {
		llvmIrBuilder->SetInsertPoint(entryBasicBlock);
		llvmIrBuilder->CreateBr(protectedCodeEntryBlock);
	}
	else {
		llvmIrBuilder->SetInsertPoint(entryBasicBlock);
		llvmIrBuilder->CreateBr(dispatchBlock);
	}

	// --- Exit Block ---
	llvmIrBuilder->SetInsertPoint(exitBlock);
	llvmIrBuilder->CreateRetVoid();
}

void LLVMTraceLifter::OptimizeModule(bool enableO3Optimization) {
	using namespace llvm;
	if (llvm::verifyModule(*llvmModule, &llvm::errs())) {
		llvm::errs() << "!!! CRITICAL ERROR: Module verification failed AFTER INSTRUMENTATION !!!\n";
		llvmModule->print(llvm::errs(), nullptr);
		return;
	}

	LoopAnalysisManager LAM;
	FunctionAnalysisManager FAM;
	CGSCCAnalysisManager CGAM;
	ModuleAnalysisManager MAM;

	PassBuilder PB;

	PB.registerModuleAnalyses(MAM);
	PB.registerCGSCCAnalyses(CGAM);
	PB.registerFunctionAnalyses(FAM);
	PB.registerLoopAnalyses(LAM);
	PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

	ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(enableO3Optimization ?
		OptimizationLevel::O3 :
		OptimizationLevel::O0);

	MPM.run(*llvmModule, MAM);
}