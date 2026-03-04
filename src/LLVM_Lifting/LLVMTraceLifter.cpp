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

#include "VmpDevirtualizationPass.hpp"

LLVMTraceLifter::LLVMTraceLifter(std::map<VirtualBasicBlock*, std::vector<VirtualBasicBlock*>> transitions, VirtualBasicBlock* startBasicBlock) {
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
	LiftTraceFunction(transitions, startBasicBlock);
}

void LLVMTraceLifter::CreateNativeContextType() {
	std::vector<llvm::Type*> elements;
	for (int i = 0; i < 16; i++) elements.push_back(i64);
	elements.push_back(i8->getPointerTo());
	elements.push_back(i64);
	elements.push_back(i64);
	nativeContextType = llvm::StructType::create(*llvmContext, elements, "NativeContext");
}

void LLVMTraceLifter::CreateFunction(const char* functionName) {
	using namespace llvm;

	auto* funcType = FunctionType::get(voidTy, {nativeContextType->getPointerTo(), i8->getPointerTo(), i64}, false);
	function = Function::Create(funcType, llvm::Function::ExternalLinkage, functionName, *llvmModule);

	auto argsIt = function->arg_begin();
	nativeContextPtr = argsIt++;
	nativeContextPtr->setName("native_context_ptr");

	realStackPtr = argsIt++;
	realStackPtr->setName("real_stack_ptr");

	imageBaseDif = argsIt++;
	imageBaseDif->setName("image_base_dif");
}

void LLVMTraceLifter::LiftTraceFunction(std::map<VirtualBasicBlock*, std::vector<VirtualBasicBlock*>> transitions, VirtualBasicBlock* startBasicBlock) {
	using namespace llvm;

	auto* entryBasicBlock = BasicBlock::Create(*llvmContext, "entry", function);

	llvm::BasicBlock* fatalErrorBlock = BasicBlock::Create(*llvmContext, "fatal_error", function);
	llvmIrBuilder->SetInsertPoint(fatalErrorBlock);
	llvmIrBuilder->CreateUnreachable();

	// --- Entry Block ---
	llvmIrBuilder->SetInsertPoint(entryBasicBlock);

	// Аллокации переменных
	auto* vsp = llvmIrBuilder->CreateAlloca(i8, llvmIrBuilder->getInt32(4096), "vsp_array");

	auto* vsp_ptr = llvmIrBuilder->CreateConstInBoundsGEP1_32(
		i8,
		vsp,
		2048, // <--- ВОТ ОНО, МАГИЧЕСКОЕ ЧИСЛО
		"vsp_base_middle"
	);

	targetVip = llvmIrBuilder->CreateAlloca(i64, nullptr, "target_vip");

	auto* virtualContext = llvmIrBuilder->CreateAlloca(i8, llvmIrBuilder->getInt32(256), "virtual_ctx");


	BasicBlockLifterConstructor constr;
	constr.context = llvmContext.get();
	constr.llvmModule = llvmModule.get();
	constr.builder = llvmIrBuilder.get();
	constr.function = function;
	constr.vsp_ptr = vsp_ptr;
	constr.vspBasePtr = vsp;
	constr.nativeContext = nativeContextPtr;
	constr.nativeContextType = nativeContextType;
	constr.virtualContext = virtualContext;
	constr.imageBaseDif = imageBaseDif;
	constr.targetVip = targetVip;
	constr.realStackPtr = realStackPtr;


	// --- Init Lifter ---
	basicBlockLifter = std::make_unique<LLVMBasicBlockLifter>(constr);

	std::map<VirtualBasicBlock*, llvm::BasicBlock*> liftedBlocksMap;
	
	// --- 1) Функция DFS для рекурсивного прохода строго по хронологии путей ---
	std::function<llvm::BasicBlock* (VirtualBasicBlock*)> LiftDFS = [&](VirtualBasicBlock* currentVbb) -> llvm::BasicBlock* {
		// Если блок уже поднимали (пришли из другой ветки), просто возвращаем его LLVM-версию
		if (liftedBlocksMap.count(currentVbb)) {
			return liftedBlocksMap[currentVbb];
		}

		// Поднимаем текущий блок
		llvm::BasicBlock* currentLlvmBlock = basicBlockLifter->LiftBasicBlock(*currentVbb, true, false);
		liftedBlocksMap[currentVbb] = currentLlvmBlock;

		// 2) Ищем наследников. Безопасный поиск (если блока нет в ключах, вектор будет пустым)
		std::vector<VirtualBasicBlock*> successors;
		auto transIt = transitions.find(currentVbb);
		if (transIt != transitions.end()) {
			successors = transIt->second;
		}

		llvmIrBuilder->SetInsertPoint(currentLlvmBlock);

		// Если терминатор еще не поставлен внутри LiftBasicBlock
		if (!currentLlvmBlock->getTerminator()) {

			// 6) Нет переходов -> это финальный блок (0x1451b7b0a и 0x1451518bc из вашего примера)
			if (successors.empty()) {
				llvmIrBuilder->CreateRetVoid(); // Выход прямо отсюда!
			}
			// Линейный участок
			else if (successors.size() == 1) {
				llvm::BasicBlock* nextLlvmBlock = LiftDFS(successors[0]);

				// Важно: возвращаем точку вставки в текущий блок после рекурсии
				llvmIrBuilder->SetInsertPoint(currentLlvmBlock);
				llvmIrBuilder->CreateBr(nextLlvmBlock);
			}
			// 4) Развилка
			else {
				llvm::Value* loadedVip = llvmIrBuilder->CreateLoad(i64, targetVip, "loaded_target_vip");
				llvm::SwitchInst* switchInst = llvmIrBuilder->CreateSwitch(loadedVip, fatalErrorBlock, successors.size());

				// 3) Сохраняем состояние лифтера ПЕРЕД развилкой
				auto savedLifterState = basicBlockLifter->SaveState();

				for (VirtualBasicBlock* succVbb : successors) {
					// 3) Откатываем состояние для каждой новой ветки
					basicBlockLifter->ApplyState(savedLifterState);

					// Поднимаем ветку
					llvm::BasicBlock* succLlvmBlock = LiftDFS(succVbb);

					// 5) Учитываем nextVipShift для точного вычисления Case'а
					uint64_t expectedVipValue = succVbb->startAddr - currentVbb->nextVipShift;

					// Добавляем case в switch текущего блока
					llvmIrBuilder->SetInsertPoint(currentLlvmBlock);
					switchInst->addCase(llvmIrBuilder->getInt64(expectedVipValue), succLlvmBlock);
				}
			}
		}

		return currentLlvmBlock;
		};

	// --- Старт лифтинга ---
	llvm::BasicBlock* firstLiftedBlock = LiftDFS(startBasicBlock);

	// Замыкаем Entry блок на первый поднятый
	llvmIrBuilder->SetInsertPoint(entryBasicBlock);
	llvmIrBuilder->CreateBr(firstLiftedBlock);
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

	PipelineTuningOptions PTO;
	PTO.LoopVectorization = false; // Отключаем векторные типы в циклах
	PTO.SLPVectorization = false;  // Отключаем склейку скалярных операций в векторы
	PTO.LoopUnrolling = false;     // Отключаем разворачивание циклов (чтобы CFG не раздувало)

	PassBuilder PB(nullptr, PTO);

	PB.registerModuleAnalyses(MAM);
	PB.registerCGSCCAnalyses(CGAM);
	PB.registerFunctionAnalyses(FAM);
	PB.registerLoopAnalyses(LAM);
	PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

	ModulePassManager MPM;

	if (enableO3Optimization) {
		// 2. Строим стандартный O3, но уже с нашими безопасными PTO
		MPM.addPass(PB.buildPerModuleDefaultPipeline(OptimizationLevel::O3));

		// 3. Добивочный проход для обфускации VMP
		// O3 делает SROA (Scalar Replacement), что иногда оставляет "ошметки" математики.
		// Контрольный проход сворачивает их окончательно.
		FunctionPassManager FPM;
		FPM.addPass(VmpDevirtualizationPass());
		FPM.addPass(InstCombinePass());
		FPM.addPass(SimplifyCFGPass());

		// Адаптируем FunctionPassManager к ModulePassManager
		MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
	}
	else {
		MPM.addPass(PB.buildPerModuleDefaultPipeline(OptimizationLevel::O0));
	}

	// 4. Запуск!
	MPM.run(*llvmModule, MAM);
}