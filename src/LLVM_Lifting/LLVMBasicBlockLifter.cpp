#include "LLVMBasicBlockLifter.hpp"
#include "NativeContext.hpp"

LLVMBasicBlockLifter::LLVMBasicBlockLifter(llvm::LLVMContext* context, llvm::Module* llvmModule, llvm::IRBuilder<>* builder, llvm::Function* function,
	llvm::Value* vsp_ptr, llvm::Value* vspBasePtr, llvm::Value* nativeContext, llvm::Type* nativeContextType, llvm::Value* imageBaseDif,
	llvm::Value* targetVip)
{
	using namespace llvm;

	this->context = context;
	this->llvmModule = llvmModule;
	this->builder = builder;
	this->function = function;

	this->vspPtr = vsp_ptr;
	this->vspBasePtr = vspBasePtr;
	this->nativeContext = nativeContext;
	this->nativeContextType = nativeContextType;
	this->imageBaseDif = imageBaseDif;
	this->targetVip = targetVip;

	i64 = Type::getInt64Ty(*context);
	i32 = Type::getInt32Ty(*context);
	i16 = Type::getInt16Ty(*context);
	i8 = Type::getInt8Ty(*context);
	voidTy = Type::getVoidTy(*context);
	ptrTy = Type::getInt64PtrTy(*context);

	InitJitHooks();
	InitLogFunctions();
}

void LLVMBasicBlockLifter::InitJitHooks() {
	llvm::FunctionType* readType = llvm::FunctionType::get(
		i64,
		{ i64, i32 },
		false
	);
	jitReadFunc = llvmModule->getOrInsertFunction("LLVM_JIT_ReadMem", readType);

	llvm::FunctionType* writeType = llvm::FunctionType::get(
		voidTy,
		{ i64, i64, i32 },
		false
	);
	jitWriteFunc = llvmModule->getOrInsertFunction("LLVM_JIT_WriteMem", writeType);
}
void LLVMBasicBlockLifter::InitLogFunctions() {
	using namespace llvm;

	auto* logPopType = FunctionType::get(
		voidTy,
		{ i32, i64, i64 }, //Bit depth, offset, value
		false
	);
	auto* logPushRegType = logPopType;
	auto* logPushConstType = FunctionType::get(
		voidTy,
		{ i32, i64 }, //Bit depth, value
		false
	);
	auto* logPushVspType = logPushConstType;
	auto* logPopVspType = logPushVspType;

	jitLogPop = llvmModule->getOrInsertFunction("LLVM_JIT_LogPop", logPopType);
	jitLogPushReg = llvmModule->getOrInsertFunction("LLVM_JIT_LogPushReg", logPushRegType);
	jitLogPushConst = llvmModule->getOrInsertFunction("LLVM_JIT_LogPushConst", logPushConstType);

	jitLogPushVsp = llvmModule->getOrInsertFunction("LLVM_JIT_LogPushVsp", logPushVspType);
	jitLogPopVsp = llvmModule->getOrInsertFunction("LLVM_JIT_LogPopVsp", logPopVspType);

	auto* memoryAccessType = FunctionType::get(
		i64,
		{ i32, i64, i64 }, //bit depth, addr, readedValue
		false
	);

	jitLogReadMem = llvmModule->getOrInsertFunction("LLVM_JIT_LogReadMem", memoryAccessType);
	jitLogWriteMem = llvmModule->getOrInsertFunction("LLVM_JIT_LogWriteMem", memoryAccessType);

	auto* vmAluType = FunctionType::get(
		voidTy,
		{ i32, i64, i64, i64, i64 }, //bit depth, arg1, arg2, result, flags
		false
	);
	jitLogAdd = llvmModule->getOrInsertFunction("LLVM_JIT_LogAdd", vmAluType);
	jitLogNor = llvmModule->getOrInsertFunction("LLVM_JIT_LogNor", vmAluType);
	jitLogNand = llvmModule->getOrInsertFunction("LLVM_JIT_LogNand", vmAluType);
	jitLogShl = llvmModule->getOrInsertFunction("LLVM_JIT_LogShl", vmAluType);
	jitLogShr = llvmModule->getOrInsertFunction("LLVM_JIT_LogShr", vmAluType);

	auto* vmShldType = FunctionType::get(
		voidTy,
		{ i32, i64, i64, i8, i64, i64 }, //bit depth, source, dst, shift, result, flags
		false
	);
	jitLogShld = llvmModule->getOrInsertFunction("LLVM_JIT_LogShld", vmShldType);
	jitLogShrd = llvmModule->getOrInsertFunction("LLVM_JIT_LogShrd", vmShldType);

	auto* vmJmpIndirectType = FunctionType::get(
		voidTy,
		{ i64 }, //dst
		false
	);
	jitLogJmpIndirect = llvmModule->getOrInsertFunction("LLVM_JIT_LogJmpIndirect", vmJmpIndirectType);
}

llvm::BasicBlock* LLVMBasicBlockLifter::LiftBasicBlock(const VirtualBasicBlock& vbb, bool useMemoryHooks, bool logMessages) {
	using namespace llvm;

	auto& handlers = vbb.instructions;
	uint64_t vip = handlers[0].vipBefore;

	auto* basicBlock = BasicBlock::Create(*context, "bb_lifted_" + std::to_string(vip), builder->GetInsertBlock()->getParent());
	builder->SetInsertPoint(basicBlock);
	shadowStack.clear();
	shadowContext.clear();
	registerFlagsPromises.clear();

	//llvm::Type* vmContextType = llvm::ArrayType::get(i8, 256);
	//virtualContext = builder->CreateAlloca(vmContextType, nullptr, "virtual_ctx");

	for (auto& inst : handlers) {
		auto& handler = inst;
		auto handlerType = handler.type;
		switch (handlerType)
		{
		case Handler_VmEntry: LiftVmEntry(std::get<VmEntryHandlerData>(handler.matchData)); break;
		case Handler_VmPop: LiftVmPop(logMessages, handler); break;
		case Handler_VmPushConst: LiftVmPushConst(logMessages, handler); break;
		case Handler_VmPushReg: LiftVmPushReg(logMessages, handler); break;
		case Handler_VmReadMem:
			if (useMemoryHooks) LiftVmReadMemHooked(logMessages, handler);
			else LiftVmReadMem(logMessages, handler);
			break;
		case Handler_VmWriteMem:
			if (useMemoryHooks) LiftVmWriteMemHooked(logMessages, handler);
			else LiftVmWriteMem(logMessages, handler);
			break;
		case Handler_VmPushVsp: LiftVmPushVsp(logMessages, handler); break;
		case Handler_VmPopVsp: LiftVmPopVsp(logMessages, handler); break;
		case Handler_VmAdd: LiftVmAdd(logMessages, handler); break;
		case Handler_VmNor: LiftVmNor(logMessages, handler); break;
		case Handler_VmNand: LiftVmNand(logMessages, handler); break;
		case Handler_VmShl: LiftVmShl(logMessages, handler); break;
		case Handler_VmShr: LiftVmShr(logMessages, handler); break;
		case Handler_VmShld: LiftVmShld(logMessages, handler); break;
		case Handler_VmShrd: LiftVmShrd(logMessages, handler); break;
		case Handler_VmJmpIndirect:
		case Handler_VmJmpIndirectRemap:
			LiftVmJmpIndirect(logMessages, std::get<VmJmpData>(handler.matchData));
			break;
		case Handler_VmExit:
			LiftVmExit(logMessages, std::get<VmExitData>(handler.matchData));
			break;
		}

		if (!shadowStack.empty()) {
			auto& lastSlot = shadowStack.back();
			if (lastSlot.value != nullptr &&
				lastSlot.value->getType()->isPointerTy() &&
				!lastSlot.isInStackAddress)
				__debugbreak();
		}
		
	}

	return basicBlock;
}

void LLVMBasicBlockLifter::ShadowPush(llvm::Value* value, HandlerBitDepth bitDepth) {
	ShadowStackSlot slot;
	slot.bitDepth = bitDepth;
	slot.value = value;
	slot.isInStackAddress = false;
	slot.isPointerToVirtualStackSlot = nullptr;
	shadowStack.push_back(slot);
}
void LLVMBasicBlockLifter::PushWithConstOffset(llvm::Value* value, HandlerBitDepth bitDepth) {
	int64_t size = bitDepth / 8; // Упрощенно
	stackOffset -= size;  // Стек растет вниз

	llvm::Value* baseAsI8Ptr = builder->CreateBitCast(vspPtr, i8->getPointerTo());

	// 2. Используем GEP с ОДНИМ индексом
	llvm::Value* ptr = builder->CreateConstInBoundsGEP1_64(
		i8,          // Теперь мы говорим: "шагаем по i8"
		baseAsI8Ptr, // От указателя i8*
		stackOffset  // На offset шагов
	);

	llvm::Value* typedPtr = builder->CreateBitCast(ptr, GetTypeByDepth(bitDepth)->getPointerTo());
	if (value->getType()->isPointerTy()) {
		value = builder->CreatePtrToInt(value, GetTypeByDepth(bitDepth), "push_with_const_offset_ptr_to_int");
	}
	builder->CreateStore(value, typedPtr);
}

bool isPowerOfTwo(unsigned long long x)
{
	return x != 0 && (x & (x - 1)) == 0;
}

LLVMBasicBlockLifter::ShadowStackPop LLVMBasicBlockLifter::ShadowPop(HandlerBitDepth bitDepth) {
	using namespace llvm;

	if (shadowStack.empty()) {
		ShadowStackPop result;
		result.value = PopWithConstOffset(bitDepth);
		result.isInStackAddress = false;
		result.isPointerToVirtualStackSlot = nullptr;
		result.slotAbsoluteBase = 0;
		result.relativeOffset = 0;
		return result;
	}

	auto& slot = shadowStack.back();
	if (!isPowerOfTwo(bitDepth)) {
		//__debugbreak();
		assert(false && "Strange behaivour");
	}
	if (!isPowerOfTwo(slot.bitDepth)) {
		//__debugbreak();
		assert(false && "Strange behaivour");
	}

	if (slot.isPadding) {
		__debugbreak();
	}

	if (slot.bitDepth == bitDepth) {
		shadowStack.pop_back();
		ShadowStackPop result;
		result.value = slot.value;
		result.isInStackAddress = slot.isInStackAddress;
		result.flagsPromise = slot.flagsPromise;
		result.isPointerToVirtualStackSlot = slot.isPointerToVirtualStackSlot;
		result.slotAbsoluteBase = slot.slotAbsoluteBase;
		result.relativeOffset = slot.relativeOffset;
		return result;
	}

	if (slot.value->getType()->isPointerTy()) {
		slot.value = builder->CreatePtrToInt(slot.value,
			GetTypeByDepth((HandlerBitDepth)slot.bitDepth),
			"shadow_pop_ptr_to_int"
		);
		slot.isPointerToVirtualStackSlot = nullptr;
		slot.isInStackAddress = false;
		slot.flagsPromise.reset();
		slot.slotAbsoluteBase = 0;
	}

	if (slot.bitDepth < bitDepth) {
		auto currentBits = static_cast<HandlerBitDepth>(slot.bitDepth);
		auto lowPart = ShadowPop(currentBits);
		HandlerBitDepth neededBits = (HandlerBitDepth)(bitDepth - currentBits);
		if (!isPowerOfTwo(neededBits)) {
			//__debugbreak();
			assert(false && "Strange behaivour");
		}
		//if (neededBits % 8 != 0) {
		//	__debugbreak();
		//	assert(false && "Strange behaivour");
		//}
		auto highPart = ShadowPop(neededBits);

		llvm::Type* targetType = GetTypeByDepth(bitDepth);
		llvm::Value* lowExt = builder->CreateZExt(lowPart.value, targetType, "merge_low_ext");
		llvm::Value* highExt = builder->CreateZExt(highPart.value, targetType, "merge_high_ext");

		llvm::Value* shiftAmt = builder->getIntN(targetType->getIntegerBitWidth(), currentBits);
		llvm::Value* highShifted = builder->CreateShl(highExt, shiftAmt, "merge_high_shifted");

		llvm::Value* result = builder->CreateOr(lowExt, highShifted, "merged_res");
		ShadowStackPop finalResult;
		finalResult.value = result;
		finalResult.isInStackAddress = false;
		finalResult.isPointerToVirtualStackSlot = nullptr;
		return finalResult;
	}

	auto* fullVal = slot.value;
	unsigned fullWidth = fullVal->getType()->getIntegerBitWidth();

	auto* result = builder->CreateTrunc(
		fullVal,
		GetTypeByDepth(bitDepth),
		"pop_slice"
	);
	// 2. Сдвигаем, чтобы получить остаток
	auto* shiftAmt = builder->getIntN(fullWidth, bitDepth);
	auto* remainingValHigh = builder->CreateLShr(fullVal, shiftAmt, "stack_rem_shifted");

	unsigned newBitDepth = fullWidth - bitDepth;
	if (!isPowerOfTwo(newBitDepth)) {
		//__debugbreak();
		assert(false && "Strange behaivour");
	}

	// Важно: GetTypeByDepth может вернуть стандартные типы, 
	// но нам нужен точный IntegerType произвольной ширины для промежуточного хранения
	auto* remainingValTrunc = builder->CreateTrunc(
		remainingValHigh,
		builder->getIntNTy(newBitDepth),
		"stack_rem_trunc"
	);

	slot.bitDepth = newBitDepth; // Обновляем размер
	slot.value = remainingValTrunc; // Теперь типы совпадают (i48 в слоте с depth 48)
	slot.isInStackAddress = false;
	slot.isPointerToVirtualStackSlot = nullptr;
	slot.flagsPromise.reset();
	slot.slotAbsoluteBase = 0;

	if (slot.bitDepth == 0) {
		shadowStack.pop_back();
	}
	ShadowStackPop finalResult;
	finalResult.value = result;
	finalResult.isInStackAddress = false;
	finalResult.isPointerToVirtualStackSlot = nullptr;

	return finalResult;
}
llvm::Value* LLVMBasicBlockLifter::PopWithConstOffset(HandlerBitDepth bitDepth) {
	int64_t size = bitDepth / 8;

	// 1. Читаем по текущему КОНСТАНТНОМУ смещению
	llvm::Value* baseAsI8Ptr = builder->CreateBitCast(vspPtr, i8->getPointerTo());

	// 2. Используем GEP с ОДНИМ индексом
	llvm::Value* ptr = builder->CreateConstInBoundsGEP1_64(
		i8,          // Теперь мы говорим: "шагаем по i8"
		baseAsI8Ptr, // От указателя i8*
		stackOffset  // На offset шагов
	);

	llvm::Value* typedPtr = builder->CreateBitCast(ptr, GetTypeByDepth(bitDepth)->getPointerTo());
	llvm::Value* val = builder->CreateLoad(GetTypeByDepth(bitDepth), typedPtr);

	// 2. Меняем оффсет в C++
	stackOffset += size;
	return val;
}
void LLVMBasicBlockLifter::FlushVsp(bool clearStack) {
	if (shadowStack.empty()) return;

	for (auto& slot : shadowStack) {
		if (slot.flagsPromise.has_value()) continue;
		if (slot.isPadding) {
			stackOffset += slot.bitDepth;
			continue;
		}
		if (slot.isInStackAddress) {
			slot.value = CreateValueByDepth((HandlerBitDepth)slot.bitDepth, slot.slotAbsoluteBase + slot.relativeOffset);
		}

		PushWithConstOffset(slot.value, static_cast<HandlerBitDepth>(slot.bitDepth));
	}
	
	if (clearStack) shadowStack.clear();
}
llvm::Value* LLVMBasicBlockLifter::GetCurrentVspVal() {
	using namespace llvm;

	Value* baseVsp = vspPtr; //builder->CreateLoad(ptrTy, vspPtr, "vsp_base_load");
	int64_t stackOffsetWithShadowStack = stackOffset - GetFullShadowStackSize();

	if (stackOffsetWithShadowStack == 0)
		return builder->CreateBitCast(baseVsp, ptrTy);

	llvm::Value* baseI8 = builder->CreateBitCast(baseVsp, builder->getInt8PtrTy());

	llvm::Value* currentVspI8 = builder->CreateConstInBoundsGEP1_64(
		builder->getInt8Ty(),
		baseI8,
		stackOffsetWithShadowStack,
		"vsp_current_offset"
	);

	return builder->CreateBitCast(currentVspI8, ptrTy);
}
llvm::Value* LLVMBasicBlockLifter::PackFlags(llvm::Value* res, llvm::Value* cf, llvm::Value* of, llvm::Value* sf, llvm::Value* zf) {
	using namespace llvm;
	llvm::Value* flags = builder->getInt64(0);

	// ZF (Bit 6) - Если не передан явно, вычисляем (res == 0)
	if (!zf) zf = builder->CreateICmpEQ(res, ConstantInt::get(res->getType(), 0));
	flags = builder->CreateOr(flags, builder->CreateShl(builder->CreateZExt(zf, i64), 6));

	// SF (Bit 7) - Если не передан явно, вычисляем (res < 0)
	if (!sf) sf = builder->CreateICmpSLT(res, ConstantInt::get(res->getType(), 0));
	flags = builder->CreateOr(flags, builder->CreateShl(builder->CreateZExt(sf, i64), 7));

	// CF (Bit 0)
	if (cf) flags = builder->CreateOr(flags, builder->CreateZExt(cf, i64));

	// OF (Bit 11)
	if (of) flags = builder->CreateOr(flags, builder->CreateShl(builder->CreateZExt(of, i64), 11));

	return flags;
}

void LLVMBasicBlockLifter::LiftVmPop(bool logDebugMessage, const HandlerMatch& match) {
	auto& data = std::get<VmPopRegData>(match.matchData);
	// 1. Выравнивание (Align down to 8 bytes)
	int32_t baseOffset = (data.offset / 8) * 8;
	int32_t relativeOffset = data.offset % 8; // Смещение внутри 64-битного слота

	//if (baseOffset == 8 && match.bitDepth == BitDepth_64) {
	//	__debugbreak();
	//}

	auto popDepth = match.bitDepth == BitDepth_8 ? BitDepth_16 : match.bitDepth; // Если 8 бит, то читаем 16 бит для выравнивания
	auto popResult = ShadowPop(popDepth);
	if (match.bitDepth == BitDepth_8) {
		// Если нам нужно 8 бит, а в стеке 16 бит, то извлекаем нужные 8 бит из результата
		popResult.value = builder->CreateTrunc(popResult.value, i8, "pop_trunc_8");
	}

	if (registerFlagsPromises.contains(baseOffset)) {
		registerFlagsPromises.erase(baseOffset);
	}

	if (popResult.flagsPromise.has_value()) {
		registerFlagsPromises[baseOffset] = popResult.flagsPromise.value();
		return;
	}

	if (match.bitDepth == BitDepth_64 && relativeOffset != 0) {
		__debugbreak();
		assert(false && "Strange behaivour");
		return;
	}

	auto slot = shadowContext.find(baseOffset);
	if (slot == shadowContext.end()){
		ShadowContextSlot newSlot;
		if (match.bitDepth == BitDepth_64) {
			newSlot.baseValue = popResult.value;
			newSlot.isVspPointer = popResult.isPointerToVirtualStackSlot;
			newSlot.isInStackPointer = popResult.isInStackAddress;
			newSlot.slotAbsoluteBase = popResult.slotAbsoluteBase;
			newSlot.relativeOffset = popResult.relativeOffset;
		}
		else {
			ShadowContextPartialWrite partialWrite;
			partialWrite.offset = relativeOffset;
			partialWrite.width = match.bitDepth / 8;
			partialWrite.value = popResult.value;
			newSlot.partialWrites.push_back(partialWrite);
		}
		shadowContext[baseOffset] = newSlot;
	}
	else {
		int32_t newWidth = match.bitDepth / 8; // В байтах
		int32_t newStart = relativeOffset;
		int32_t newEnd = newStart + newWidth;
		// Удаляем все частичные записи, которые полностью перекрываются новой записью
		slot->second.partialWrites.erase(
			std::remove_if(
				slot->second.partialWrites.begin(),
				slot->second.partialWrites.end(),
				[newStart, newEnd](const ShadowContextPartialWrite& pw) {
					int32_t pwStart = pw.offset;
					int32_t pwEnd = pw.offset + (pw.width / 8);
					return pwStart >= newStart && pwEnd <= newEnd;
				}
			),
			slot->second.partialWrites.end()
		);

		slot->second.isVspPointer = nullptr;
		slot->second.isInStackPointer = false;
		slot->second.slotAbsoluteBase = 0;
		slot->second.relativeOffset = 0;

		if (match.bitDepth == BitDepth_64) {
			slot->second.baseValue = popResult.value;
			slot->second.isVspPointer = popResult.isPointerToVirtualStackSlot;
			slot->second.isInStackPointer = popResult.isInStackAddress;
			slot->second.partialWrites.clear();
			slot->second.slotAbsoluteBase = popResult.slotAbsoluteBase;
			slot->second.relativeOffset = popResult.relativeOffset;
		}
		else {
			auto& partialWrites = slot->second.partialWrites;
			ShadowContextPartialWrite partialWrite;
			partialWrite.offset = relativeOffset;
			partialWrite.width = match.bitDepth / 8;
			partialWrite.value = popResult.value;
			bool writeFound = false;
			for (auto& pw : partialWrites) {
				if (partialWrite.offset == pw.offset &&
					partialWrite.width == pw.width) {
					pw.value = popResult.value;
					writeFound = true;
				}
			}

			if (!writeFound)
				partialWrites.push_back(partialWrite);
		}
	}

	// --- LOGGING ---
	if (logDebugMessage) {
		llvm::Value* val64 = (popResult.value->getType()->isIntegerTy(64))
			? popResult.value
			: builder->CreateZExt(popResult.value, builder->getInt64Ty(), "log_val_ext");

		builder->CreateCall(jitLogPop, {
			builder->getInt32(match.bitDepth),
			builder->getInt64(data.offset),
			val64
			});
	}
	// ----------------

}
void LLVMBasicBlockLifter::LiftVmPushReg(bool logDebugMessage, const HandlerMatch& match) {
	auto& data = std::get<VmPushRegData>(match.matchData);

	llvm::Value* valueToPush = nullptr;
	int32_t baseOffset = (data.regOffset / 8) * 8;
	int32_t relativeOffset = data.regOffset % 8; // Смещение внутри 64-битного слота
	if (relativeOffset < 0 || relativeOffset > 8) {
		__debugbreak();
	}
	if (baseOffset < 0 || baseOffset > 256) {
		__debugbreak();
	}

	bool isFlagPromise = registerFlagsPromises.contains(baseOffset);
	auto slot = shadowContext.find(baseOffset);
	if (match.bitDepth == BitDepth_64 && isFlagPromise) {
		auto& promise = registerFlagsPromises[baseOffset];
		if (promise.res == nullptr) promise.res = CalculateFlagsFromPromise(promise);
		ShadowStackSlot newSlot;
		newSlot.value = promise.res;
		newSlot.bitDepth = BitDepth_64;
		newSlot.isInStackAddress = false;
		newSlot.isPointerToVirtualStackSlot = nullptr;
		shadowStack.push_back(newSlot);

		if (slot != shadowContext.end()) {
			slot->second.baseValue = promise.res;
			slot->second.isInStackPointer = false;
			slot->second.isVspPointer = nullptr;
			slot->second.partialWrites.clear();
			slot->second.relativeOffset = 0;
			slot->second.slotAbsoluteBase = 0;
			registerFlagsPromises.erase(baseOffset);
		}
		
		return;
	}

	
	
	bool hasNoPartialWrites = slot->second.partialWrites.empty();
	bool hasPartialWrites = !hasNoPartialWrites;

	if (match.bitDepth == BitDepth_64 && slot->second.baseValue && !isFlagPromise && hasNoPartialWrites ) {
		ShadowStackSlot newSlot;
		newSlot.value = slot->second.baseValue;
		newSlot.bitDepth = BitDepth_64;
		newSlot.isInStackAddress = slot->second.isInStackPointer;
		newSlot.isPointerToVirtualStackSlot = slot->second.isVspPointer;
		newSlot.slotAbsoluteBase = slot->second.slotAbsoluteBase;
		newSlot.relativeOffset = slot->second.relativeOffset;
		valueToPush = slot->second.baseValue;
		shadowStack.push_back(newSlot);
	}


	bool matchFoundInPartialWrites = false;
	if (hasPartialWrites && match.bitDepth != BitDepth_64 && valueToPush == nullptr) {
		auto& partialWrites = slot->second.partialWrites;
		auto pushDepth = match.bitDepth == BitDepth_8 ? BitDepth_16 : match.bitDepth;
		for (auto& pw : partialWrites) {
			if (pw.offset == relativeOffset && pw.width * 8 == match.bitDepth) {
				ShadowStackSlot newSlot;
				newSlot.value = pw.value;
				if (match.bitDepth == BitDepth_8) {
					newSlot.value = builder->CreateZExt(newSlot.value, i16, "push_reg_ext_8_to_16");
				}
				newSlot.bitDepth = pushDepth;
				newSlot.isInStackAddress = false;
				newSlot.isPointerToVirtualStackSlot = nullptr;
				newSlot.slotAbsoluteBase = 0;
				valueToPush = newSlot.value;
				shadowStack.push_back(newSlot);
				matchFoundInPartialWrites = true;
				break;
			}
		}
	}

	if (!matchFoundInPartialWrites && valueToPush == nullptr) {
		// Если у нас есть частичные записи, но ни одна из них не совпала с нашим смещением и размером, это может означать, что мы пытаемся прочитать часть регистра, которая была частично перезаписана. В этом случае нам нужно собрать полное значение регистра, учитывая все частичные записи, и затем извлечь нужную часть.
		llvm::Value* fullRegVal = slot->second.baseValue;
		auto& partialWrites = slot->second.partialWrites;
		slot->second.isInStackPointer = false;
		slot->second.isVspPointer = nullptr;
		if (fullRegVal == nullptr) {
			fullRegVal = builder->getInt64(0); // Если базового значения нет, начинаем с 0
		}
		else {
			if (fullRegVal->getType()->isPointerTy())
				fullRegVal = builder->CreatePtrToInt(fullRegVal, i64, "full_reg_ptr_to_int");

			if (fullRegVal->getType()->getIntegerBitWidth() < 64) {
				fullRegVal = builder->CreateZExt(fullRegVal, i64, "full_reg_ext");
			}
		}
		
		for (auto& pw : partialWrites) {
			uint64_t widthBits = pw.width * 8;
			uint64_t maskRaw = (widthBits == 64) ? -1ULL : ((1ULL << widthBits) - 1);

			llvm::APInt maskAPI(64, maskRaw);
			maskAPI = maskAPI.shl(pw.offset * 8);

			llvm::Value* preserveMask = builder->getIntN(64, ~maskAPI.getZExtValue());

			// Clear: удаляем биты там, куда будем писать
			fullRegVal = builder->CreateAnd(fullRegVal, preserveMask, "collapse_clear");

			// Prepare value: берем значение, кастуем к i64, сдвигаем
			llvm::Value* valToWrite = pw.value;
			if (valToWrite->getType()->isPointerTy()) valToWrite = builder->CreatePtrToInt(valToWrite, i64);
			else if (valToWrite->getType()->getIntegerBitWidth() < 64) valToWrite = builder->CreateZExt(valToWrite, i64);

			llvm::Value* shiftedVal = builder->CreateShl(valToWrite, pw.offset * 8, "collapse_shl");

			// Insert: вставляем новые биты
			fullRegVal = builder->CreateOr(fullRegVal, shiftedVal, "collapse_or");
		}

		auto* resultVal = fullRegVal;
		slot->second.baseValue = fullRegVal;
		slot->second.partialWrites.clear();

		if (relativeOffset > 0) {
			resultVal = builder->CreateLShr(resultVal, relativeOffset * 8, "extract_shr");
		}

		if (match.bitDepth != BitDepth_64) {
			resultVal = builder->CreateTrunc(resultVal, GetTypeByDepth(match.bitDepth), "extract_trunc");
		}

		auto pushDepth = match.bitDepth;
		if (match.bitDepth == BitDepth_8) {
			resultVal = builder->CreateZExt(resultVal, i16, "extract_ext_8_to_16");
			pushDepth = BitDepth_16;
		}

		ShadowStackSlot newSlot;
		newSlot.value = resultVal;
		newSlot.bitDepth = pushDepth;
		newSlot.isInStackAddress = false;
		newSlot.isPointerToVirtualStackSlot = nullptr;
		newSlot.slotAbsoluteBase = hasNoPartialWrites ? slot->second.slotAbsoluteBase : 0;
		newSlot.relativeOffset = hasNoPartialWrites ? slot->second.relativeOffset : 0;
		valueToPush = resultVal;
		shadowStack.push_back(newSlot);
	}

	// --- LOGGING ---
	if (logDebugMessage) {
		if (valueToPush->getType()->isPointerTy()) {
			valueToPush = builder->CreatePtrToInt(valueToPush, i64, "log_push_ptr_to_int");
		}

		llvm::Value* val64 = (valueToPush->getType()->isIntegerTy(64))
			? valueToPush
			: builder->CreateZExt(valueToPush, i64, "log_val_ext");

		builder->CreateCall(jitLogPushReg, {
			builder->getInt32(match.bitDepth),
			builder->getInt64(data.regOffset),
			val64
			});
	}
	// ----------------
}
void LLVMBasicBlockLifter::LiftVmPushConst(bool logDebugMessage, const HandlerMatch& match) {
	auto& data = std::get<VmPushConstData>(match.matchData);

	// --- LOGGING ---
	if (logDebugMessage) {
		// Логируем оригинальное значение (data.value уже 64 бит в структуре, скорее всего, или расширим)
		builder->CreateCall(jitLogPushConst, {
			builder->getInt32(match.bitDepth),
			builder->getInt64(data.value)
			});
	}
	// ----------------

	if (match.bitDepth == BitDepth_8) {
		// Правило 2: Константа 8 бит, но в стек кладем 16 бит
		// Создаем сразу i16 константу (маскируя лишнее, если нужно)
		llvm::Value* constVal16 = builder->getInt16(static_cast<uint8_t>(data.value));
		ShadowPush(constVal16, BitDepth_16);
	}
	else {
		llvm::Value* constVal = CreateValueByDepth(match.bitDepth, data.value);
		ShadowPush(constVal, match.bitDepth);
	}
}

llvm::Value* LLVMBasicBlockLifter::CalculateFlagsFromPromise(FlagsPromise& promise) {
	using namespace llvm;
	auto calcCommonFlags = [&](Value* result) {
		// SF = MSB (Sign Bit)
		Value* sf = builder->CreateICmpSLT(result, ConstantInt::get(result->getType(), 0), "sf");
		// ZF = (result == 0)
		Value* zf = builder->CreateICmpEQ(result, ConstantInt::get(result->getType(), 0), "zf");
		// PF (Parity) - обычно VMP его игнорирует, но если нужно - это xor всех байт
		// Для простоты пока вернем false или честный расчет, если критично
		Value* pf = builder->getFalse();
		return std::make_tuple(sf, zf, pf);
	};
	
	if (promise.operation == FlagsPromise::ADD) {	
		// Исправлено: берем тип операнда, а не opType (enum)
		auto saddFunc = Intrinsic::getDeclaration(llvmModule, Intrinsic::sadd_with_overflow, { promise.opType});
		auto sRes = builder->CreateCall(saddFunc, { promise.op1, promise.op2 });

		Value* sum = builder->CreateExtractValue(sRes, 0, "add_res");
		Value* of = builder->CreateExtractValue(sRes, 1, "add_of");

		// CF = (UnsignedSum < Op1)
		Value* cf = builder->CreateICmpULT(sum, promise.op1, "add_cf");

		// ZF, SF, PF вычисляются внутри PackFlags от результата (sum)
		// Если ваш PackFlags принимает их явно:
		auto [sf, zf, pf] = calcCommonFlags(sum);
		return PackFlags(sum, cf, of, sf, zf);
	}
	else if (promise.operation == FlagsPromise::NOR) {
		// NOR (NOT OR). В x86 логические операции (AND, OR, XOR) обнуляют CF и OF.
		// VMP NOR: ~(op1 | op2)
		Value* orVal = builder->CreateOr(promise.op1, promise.op2);
		Value* res = builder->CreateNot(orVal, "nor_res");

		Value* cf = builder->getFalse(); // CF = 0
		Value* of = builder->getFalse(); // OF = 0

		auto [sf, zf, pf] = calcCommonFlags(res);
		return PackFlags(res, cf, of, sf, zf);
	}
	else if (promise.operation == FlagsPromise::NAND) {
		// NAND (NOT AND). CF = 0, OF = 0.
		// VMP NAND: ~(op1 & op2)
		Value* andVal = builder->CreateAnd(promise.op1, promise.op2);
		Value* res = builder->CreateNot(andVal, "nand_res");

		Value* cf = builder->getFalse();
		Value* of = builder->getFalse();

		auto [sf, zf, pf] = calcCommonFlags(res);
		return PackFlags(res, cf, of, sf, zf);
	}
	else if (promise.operation == FlagsPromise::SHL) {
		// SHL dest, count
		// op1 = value, op2 = shift_amount
		Value* val = promise.op1;
		Value* amt = promise.op2;
		unsigned bitWidth = val->getType()->getIntegerBitWidth();

		// Результат
		Value* res = builder->CreateShl(val, amt, "shl_res");

		// CF = последний выдвинутый бит
		// Для SHL это бит с индексом (BitWidth - ShiftAmt) исходного числа
		// Но проще: CF = (val >> (BitWidth - amt)) & 1
		Value* bitToShift = builder->CreateSub(builder->getIntN(bitWidth, bitWidth), amt);
		Value* valShifted = builder->CreateLShr(val, bitToShift);
		Value* cf = builder->CreateTrunc(valShifted, builder->getInt1Ty(), "shl_cf");

		// OF (определен только если сдвиг == 1)
		// OF = MSB(Result) ^ CF
		// (Знак результата изменился по сравнению с выдвинутым битом)
		// VMP часто считает его всегда как (MSB_res ^ CF)
		Value* msbRes = builder->CreateICmpSLT(res, builder->getIntN(bitWidth, 0)); // MSB результата
		Value* of = builder->CreateXor(msbRes, cf, "shl_of");

		auto [sf, zf, pf] = calcCommonFlags(res);
		return PackFlags(res, cf, of, sf, zf);
	}
	else if (promise.operation == FlagsPromise::SHR) {
		// SHR (LOGICAL) dest, count
		Value* val = promise.op1;
		Value* amt = promise.op2;
		unsigned bitWidth = val->getType()->getIntegerBitWidth();

		Value* res = builder->CreateLShr(val, amt, "shr_res");

		// CF = последний выдвинутый бит (младший)
		// CF = (val >> (amt - 1)) & 1
		Value* shiftForCf = builder->CreateSub(amt, builder->getIntN(bitWidth, 1));
		Value* valForCf = builder->CreateLShr(val, shiftForCf);
		Value* cf = builder->CreateTrunc(valForCf, builder->getInt1Ty(), "shr_cf");

		// OF (определен только если сдвиг == 1)
		// Для логического SHR: OF = MSB(Original)
		Value* of = builder->CreateICmpSLT(val, builder->getIntN(bitWidth, 0), "shr_of");

		auto [sf, zf, pf] = calcCommonFlags(res);
		return PackFlags(res, cf, of, sf, zf);
	}
	else if (promise.operation == FlagsPromise::SHLD) {
		// SHLD Dest, Source, Count
		// В VMP обычно op1 = Dest, op2 = Source, op3 = Count (нужно расширить Promise)
		// Результат: (Dest << Count) | (Source >> (BitWidth - Count))

		Value* dest = promise.op1;
		Value* src = promise.op2;
		Value* count = promise.op3; // Убедитесь, что добавили op3 в FlagsPromise!
		unsigned bitWidth = dest->getType()->getIntegerBitWidth();

		// Вычисляем результат
		Value* part1 = builder->CreateShl(dest, count);
		Value* shiftRightAmt = builder->CreateSub(builder->getIntN(bitWidth, bitWidth), count);
		Value* part2 = builder->CreateLShr(src, shiftRightAmt);
		Value* res = builder->CreateOr(part1, part2, "shld_res");

		// CF = последний бит, ушедший из Dest
		// То же самое, что в SHL: (Dest >> (BitWidth - Count)) & 1
		Value* destShifted = builder->CreateLShr(dest, shiftRightAmt);
		Value* cf = builder->CreateTrunc(destShifted, builder->getInt1Ty(), "shld_cf");

		// OF = MSB(Result) ^ MSB(OriginalDest) (если count=1)
		// Или стандартно для сдвигов влево: MSB(Res) ^ CF
		Value* msbRes = builder->CreateICmpSLT(res, builder->getIntN(bitWidth, 0));
		Value* of = builder->CreateXor(msbRes, cf, "shld_of");

		auto [sf, zf, pf] = calcCommonFlags(res);
		return PackFlags(res, cf, of, sf, zf);
	}
	else if (promise.operation == FlagsPromise::SHRD) {
		// SHRD Dest, Source, Count
		// Результат: (Dest >> Count) | (Source << (BitWidth - Count))

		Value* dest = promise.op1;
		Value* src = promise.op2;
		Value* count = promise.op3;
		unsigned bitWidth = dest->getType()->getIntegerBitWidth();

		Value* part1 = builder->CreateLShr(dest, count);
		Value* shiftLeftAmt = builder->CreateSub(builder->getIntN(bitWidth, bitWidth), count);
		Value* part2 = builder->CreateShl(src, shiftLeftAmt);
		Value* res = builder->CreateOr(part1, part2, "shrd_res");

		// CF = последний бит, ушедший из Dest (младший)
		// (Dest >> (Count - 1)) & 1
		Value* shiftForCf = builder->CreateSub(count, builder->getIntN(bitWidth, 1));
		Value* valForCf = builder->CreateLShr(dest, shiftForCf);
		Value* cf = builder->CreateTrunc(valForCf, builder->getInt1Ty(), "shrd_cf");

		// OF = (MSB(Result) != MSB(OriginalDest)) (только при count=1)
		// XOR MSB результата и оригинала
		Value* msbRes = builder->CreateICmpSLT(res, builder->getIntN(bitWidth, 0));
		Value* msbOrig = builder->CreateICmpSLT(dest, builder->getIntN(bitWidth, 0));
		Value* of = builder->CreateXor(msbRes, msbOrig, "shrd_of");

		auto [sf, zf, pf] = calcCommonFlags(res);
		return PackFlags(res, cf, of, sf, zf);
	}

	return nullptr; // На всякий случай, если будет вызван с неподдерживаемой операцией
}

void LLVMBasicBlockLifter::LiftVmPushVsp(bool logDebugMessage, const HandlerMatch& match) {
	// Читаем текущий адрес стека (значение переменной vsp_ptr)
	llvm::Value* currentVsp = builder->CreateBitCast(vspPtr, ptrTy);

	// Превращаем указатель в число (ptr -> i64), чтобы положить в стек
	//llvm::Value* vspAsInt = builder->CreatePtrToInt(currentVsp, GetTypeByDepth(match.bitDepth), "vsp_int");

	// --- LOGGING ---
	if (logDebugMessage) {
		llvm::Value* vspAsInt = builder->CreatePtrToInt(currentVsp, GetTypeByDepth(match.bitDepth), "vsp_int");
		llvm::Value* val64 = (vspAsInt->getType()->isIntegerTy(64))
			? vspAsInt
			: builder->CreateZExt(vspAsInt, builder->getInt64Ty());

		builder->CreateCall(jitLogPushVsp, {
			builder->getInt32(match.bitDepth),
			val64
			});
	}

	auto pushDepth = match.bitDepth == BitDepth_8 ? BitDepth_16 : match.bitDepth;

	ShadowStackSlot slot;
	slot.value = currentVsp;
	if (match.bitDepth != BitDepth_64) {
		slot.value = builder->CreatePtrToInt(currentVsp, GetTypeByDepth(pushDepth), "vsp_int_for_push");
	}
	slot.bitDepth = pushDepth;
	slot.isInStackAddress = match.bitDepth == BitDepth_64;
	slot.isPointerToVirtualStackSlot = nullptr;
	slot.slotAbsoluteBase = stackOffset - GetFullShadowStackSize();
	if (!shadowStack.empty()) {
		slot.isPointerToVirtualStackSlot = match.bitDepth == BitDepth_64 ? 
			&shadowStack.back() : 
			nullptr;
	}
	
	shadowStack.push_back(slot);
}
void LLVMBasicBlockLifter::LiftVmPopVsp(bool logDebugMessage, const HandlerMatch& match) {
	auto vspPopData = std::get<VmPopVspData>(match.matchData);
	
	ShadowPop(match.bitDepth);

	if (vspPopData.offset > 0) {
		__debugbreak(); //TODO: обрезание теневого стека
	}

	//FlushVsp(true);

	// --- LOGGING ---

	builder->CreateCall(jitLogPopVsp, {
		builder->getInt32(match.bitDepth),
		builder->getInt64(vspPopData.offset) // Логируем смещение, на которое был изменен VSP
	});
	// ----------------

	// Превращаем число обратно в указатель (i64 -> i64*)
	//llvm::Value* newVspPtr = builder->CreateIntToPtr(newVspInt, ptrTy, "new_vsp_ptr");

	// Обновляем переменную vspPtrAddr
	//builder->CreateStore(newVspPtr, vspPtr);

	ShadowStackSlot slot;
	slot.bitDepth = vspPopData.offset;
	slot.isPadding = true;
	shadowStack.push_back(slot);

	//stackOffset += vspPopData.offset;
}

void LLVMBasicBlockLifter::LiftVmReadMem(bool logDebugMessage, const HandlerMatch& match) {
	auto addrSlot = ShadowPop(BitDepth_64);
	if (TryGenerateShadowStackAccess(&addrSlot, match.bitDepth, nullptr)) {
		return;
	}

	auto* targetAddr = addrSlot.value;
	if (addrSlot.slotAbsoluteBase) {
		targetAddr = builder->CreateBitCast(targetAddr, i8->getPointerTo());
		targetAddr = builder->CreateConstInBoundsGEP1_32(i8, targetAddr, addrSlot.slotAbsoluteBase + addrSlot.relativeOffset);
	}

	GenerateMemoryAccess(
		targetAddr,
		GetTypeByDepth(match.bitDepth),
		nullptr,
		false,
		addrSlot.isInStackAddress // <--- Передаем флаг
	);
}
void LLVMBasicBlockLifter::LiftVmReadMemHooked(bool logDebugMessage, const HandlerMatch& match) {
	auto addrSlot = ShadowPop(BitDepth_64);
	if (TryGenerateShadowStackAccess(&addrSlot, match.bitDepth, nullptr)) {
		return;
	}

	auto* targetAddr = addrSlot.value;
	if (addrSlot.slotAbsoluteBase) {
		targetAddr = builder->CreateBitCast(targetAddr, i8->getPointerTo());
		targetAddr = builder->CreateConstInBoundsGEP1_32(i8, targetAddr, addrSlot.slotAbsoluteBase + addrSlot.relativeOffset);
	}

	GenerateMemoryAccess(
		targetAddr,
		GetTypeByDepth(match.bitDepth),
		nullptr,
		true,
		addrSlot.isInStackAddress // <--- Передаем флаг
	);

}
void LLVMBasicBlockLifter::LiftVmWriteMem(bool logDebugMessage, const HandlerMatch& match) {
	llvm::Value* val;
	llvm::Type* type;

	auto addrSlot = ShadowPop(BitDepth_64);

	if (match.bitDepth == BitDepth_8) {
		// Пишем 8 бит, но на стеке лежит 16
		llvm::Value* val16 = ShadowPop(BitDepth_16).value;
		val = builder->CreateTrunc(val16, i8);
		type = i8;
	}
	else {
		val = ShadowPop(match.bitDepth).value;
		type = GetTypeByDepth(match.bitDepth);
	}

	if (TryGenerateShadowStackAccess(&addrSlot, match.bitDepth, val)) {
		return;
	}

	auto* targetAddr = addrSlot.value;
	if (addrSlot.slotAbsoluteBase) {
		targetAddr = builder->CreateBitCast(targetAddr, i8->getPointerTo());
		targetAddr = builder->CreateConstInBoundsGEP1_32(i8, targetAddr, addrSlot.slotAbsoluteBase + addrSlot.relativeOffset);
	}

	GenerateMemoryAccess(targetAddr, type, val, false, addrSlot.isInStackAddress);
}
void LLVMBasicBlockLifter::LiftVmWriteMemHooked(bool logDebugMessage, const HandlerMatch& match) {
	llvm::Value* val;
	llvm::Type* type;

	auto addrSlot = ShadowPop(BitDepth_64);

	if (match.bitDepth == BitDepth_8) {
		// Пишем 8 бит, но на стеке лежит 16
		llvm::Value* val16 = ShadowPop(BitDepth_16).value;
		val = builder->CreateTrunc(val16, i8);
		type = i8;
	}
	else {
		val = ShadowPop(match.bitDepth).value;
		type = GetTypeByDepth(match.bitDepth);
	}

	if (TryGenerateShadowStackAccess(&addrSlot, match.bitDepth, val)) {
		return;
	}

	auto* targetAddr = addrSlot.value;
	if (addrSlot.slotAbsoluteBase) {
		targetAddr = builder->CreateBitCast(targetAddr, i8->getPointerTo());
		targetAddr = builder->CreateConstInBoundsGEP1_32(i8, targetAddr, addrSlot.slotAbsoluteBase + addrSlot.relativeOffset);
	}

	GenerateMemoryAccess(targetAddr, type, val, true, addrSlot.isInStackAddress);
}

// Возвращает std::optional, если значение - константа, иначе nullopt
std::optional<int64_t> GetConstantInt(llvm::Value* val) {
	// Проверяем, является ли Value* экземпляром ConstantInt
	if (auto* constInt = llvm::dyn_cast<llvm::ConstantInt>(val)) {
		// getSExtValue() - Sign Extended (со знаком). Важно для отрицательных смещений!
		// getZExtValue() - Zero Extended (без знака).
		return constInt->getSExtValue();
	}
	return std::nullopt;
}

void LLVMBasicBlockLifter::LiftVmAdd(bool logDebugMessage, const HandlerMatch& match) {
	using namespace llvm;

	llvm::Value* valA, * valB;
	llvm::Type* calcType;
	HandlerBitDepth pushDepth = match.bitDepth;
	bool hasSignleVspOperand = false;

	HandlerBitDepth popDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;
	auto valBSlot = ShadowPop(popDepth);
	auto valASlot = ShadowPop(popDepth);
	valB = valBSlot.value;
	valA = valASlot.value;
	ShadowStackSlot* vspSlot = nullptr;
	int64_t slotAbsoluteBase = 0;
	int64_t relativeOffse = 0;

	llvm::Value* result = nullptr;
	if (valBSlot.isInStackAddress && !valASlot.isInStackAddress) {
		vspSlot = valBSlot.isPointerToVirtualStackSlot;
		slotAbsoluteBase = valBSlot.slotAbsoluteBase;
		result = valB;
		valB = builder->getInt64(
			fakeStackBase +
			valBSlot.slotAbsoluteBase +
			valBSlot.relativeOffset
		);
		hasSignleVspOperand = true;
		auto constValue = GetConstantInt(valA);
		if (constValue) relativeOffse = *constValue;
	}
	else if (valASlot.isInStackAddress && !valBSlot.isInStackAddress) {
		vspSlot = valASlot.isPointerToVirtualStackSlot;
		slotAbsoluteBase = valASlot.slotAbsoluteBase;
		result = valA;
		valA = builder->getInt64(
			fakeStackBase + 
			valASlot.slotAbsoluteBase + 
			valASlot.relativeOffset
		);
		hasSignleVspOperand = true;
		auto constValue = GetConstantInt(valB);
		if (constValue) relativeOffse = *constValue;
	}
	else {
		if (valA->getType()->isPointerTy()) __debugbreak();
		if (valB->getType()->isPointerTy()) __debugbreak();
		result = builder->CreateAdd(valA, valB, "add_res");
	}


	calcType = GetTypeByDepth(match.bitDepth);
	if (match.bitDepth == BitDepth_8) {
		valB = builder->CreateTrunc(valB, i8);
		valA = builder->CreateTrunc(valA, i8);
		pushDepth = BitDepth_16; // Результат 8-битного сложения всегда 16 бит
	}

	ShadowStackSlot resultSlot;
	resultSlot.isInStackAddress = hasSignleVspOperand; // Если один из операндов был VSP, результат тоже помечаем как VSP
	resultSlot.isPointerToVirtualStackSlot = vspSlot;
	resultSlot.bitDepth = pushDepth;
	resultSlot.value = result;
	resultSlot.slotAbsoluteBase = slotAbsoluteBase;
	resultSlot.relativeOffset = relativeOffse;
	shadowStack.push_back(resultSlot);

	FlagsPromise flagsPromise;
	flagsPromise.op1 = valA;
	flagsPromise.op2 = valB;
	flagsPromise.operation = FlagsPromise::ADD;
	flagsPromise.res = nullptr;
	flagsPromise.opType = calcType;

	ShadowStackSlot flagsSlot;
	flagsSlot.isInStackAddress = false; // Если один из операндов был VSP, результат тоже помечаем как VSP
	flagsSlot.isPointerToVirtualStackSlot = nullptr;
	flagsSlot.bitDepth = BitDepth_64;
	flagsSlot.flagsPromise = flagsPromise;

	shadowStack.push_back(flagsSlot);

	if (logDebugMessage) {
		if (result->getType()->isPointerTy()) {
			result = builder->CreatePtrToInt(result, i64);
		}
		builder->CreateCall(jitLogAdd, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(valA, i64), builder->CreateZExt(valB, i64),
			builder->CreateZExt(result, i64), builder->getInt64(0)
			});
	}
}
void LLVMBasicBlockLifter::LiftVmNor(bool logDebugMessage, const HandlerMatch& match) {
	using namespace llvm;
	Value* valA, * valB;
	HandlerBitDepth pushDepth = match.bitDepth;
	Type* flagsType = GetTypeByDepth(match.bitDepth);

	auto popDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;
	auto valBSlot = ShadowPop(popDepth);
	auto valASlot = ShadowPop(popDepth);
	valB = valBSlot.value;
	valA = valASlot.value;
	if (valBSlot.isInStackAddress) {
		valB = builder->getInt64(fakeStackBase + valBSlot.slotAbsoluteBase + valBSlot.relativeOffset);
	}
	if (valASlot.isInStackAddress) {
		valA = builder->getInt64(fakeStackBase + valASlot.slotAbsoluteBase + valASlot.relativeOffset);
	}

	if (match.bitDepth == BitDepth_8) {
		valB = builder->CreateTrunc(valB, i8);
		valA = builder->CreateTrunc(valA, i8);
		pushDepth = BitDepth_16; // Результат 8-битного NOR всегда 16 бит
		flagsType = i8;
	}

	Value* result = builder->CreateNot(builder->CreateOr(valA, valB));

	if (match.bitDepth == BitDepth_8) {
		result = builder->CreateZExt(result, i16);
	}


	ShadowPush(result, pushDepth);
	
	FlagsPromise flagsPromise;
	flagsPromise.op1 = valA;
	flagsPromise.op2 = valB;
	flagsPromise.operation = FlagsPromise::NOR;
	flagsPromise.res = nullptr;
	flagsPromise.opType = flagsType;

	ShadowStackSlot flagsSlot;
	flagsSlot.isInStackAddress = false;
	flagsSlot.isPointerToVirtualStackSlot = nullptr;
	flagsSlot.bitDepth = BitDepth_64;
	flagsSlot.flagsPromise = flagsPromise;

	shadowStack.push_back(flagsSlot);

	if (logDebugMessage) {
		builder->CreateCall(jitLogNor, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(valA, i64), builder->CreateZExt(valB, i64),
			builder->CreateZExt(result, i64), builder->getInt64(0)
			});
	}
}
void LLVMBasicBlockLifter::LiftVmNand(bool logDebugMessage, const HandlerMatch& match) {
	using namespace llvm;
	Value* valA, * valB;
	HandlerBitDepth pushDepth = match.bitDepth;
	Type* flagsType = GetTypeByDepth(match.bitDepth);

	auto popDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;
	auto valBSlot = ShadowPop(popDepth);
	auto valASlot = ShadowPop(popDepth);
	valB = valBSlot.value;
	valA = valASlot.value;
	if (valBSlot.isInStackAddress) {
		valB = builder->getInt64(fakeStackBase + valBSlot.slotAbsoluteBase + valBSlot.relativeOffset);
	}
	if (valASlot.isInStackAddress) {
		valA = builder->getInt64(fakeStackBase + valASlot.slotAbsoluteBase + valASlot.relativeOffset);
	}

	if (match.bitDepth == BitDepth_8) {
		valB = builder->CreateTrunc(valB, i8);
		valA = builder->CreateTrunc(valA, i8);
		pushDepth = BitDepth_16; // Результат 8-битного NOR всегда 16 бит
		flagsType = i8;
	}

	Value* result = builder->CreateNot(builder->CreateAnd(valA, valB));

	if (match.bitDepth == BitDepth_8) {
		result = builder->CreateZExt(result, i16);
	}

	ShadowPush(result, pushDepth);

	FlagsPromise flagsPromise;
	flagsPromise.op1 = valA;
	flagsPromise.op2 = valB;
	flagsPromise.operation = FlagsPromise::NAND;
	flagsPromise.res = nullptr;
	flagsPromise.opType = flagsType;

	ShadowStackSlot flagsSlot;
	flagsSlot.isInStackAddress = false;
	flagsSlot.isPointerToVirtualStackSlot = nullptr;
	flagsSlot.bitDepth = BitDepth_64;
	flagsSlot.flagsPromise = flagsPromise;

	shadowStack.push_back(flagsSlot);

	if (logDebugMessage) {
		builder->CreateCall(jitLogNand, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(valA, i64), builder->CreateZExt(valB, i64),
			builder->CreateZExt(result, i64), builder->getInt64(0)
			});
	}
}

void LLVMBasicBlockLifter::LiftVmShl(bool logDebugMessage, const HandlerMatch& match) {
	using namespace llvm;

	// 1. Top: Val
	auto popDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;
	Value* val = ShadowPop(popDepth).value;
	Type* opType = GetTypeByDepth(popDepth);

	// 2. Next: Amt (всегда 16 бит в стеке, но эффективно 8 бит)
	Value* amt16 = ShadowPop(BitDepth_16).value;
	Value* amt8 = builder->CreateTrunc(amt16, i8);
	Value* amt = builder->CreateZExt(amt8, opType); // Приводим к типу значения для сдвига

	// 3. Вычисляем результат
	Value* result = builder->CreateShl(val, amt);

	auto pushDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;

	ShadowPush(result, pushDepth);
	
	FlagsPromise flagsPromise;
	flagsPromise.op1 = val;
	flagsPromise.op2 = amt;
	flagsPromise.operation = FlagsPromise::SHL;
	flagsPromise.res = nullptr;
	flagsPromise.opType = opType;

	ShadowStackSlot flagsSlot;
	flagsSlot.isInStackAddress = false;
	flagsSlot.isPointerToVirtualStackSlot = nullptr;
	flagsSlot.bitDepth = BitDepth_64;
	flagsSlot.flagsPromise = flagsPromise;

	shadowStack.push_back(flagsSlot);

	if (logDebugMessage) {
		builder->CreateCall(jitLogShl, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(val, i64), builder->CreateZExt(amt, i64),
			builder->CreateZExt(result, i64), builder->getInt64(0)
			});
	}
}
void LLVMBasicBlockLifter::LiftVmShr(bool logDebugMessage, const HandlerMatch& match) {
	using namespace llvm;

	// 1. Top: Val
	auto popDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;
	Value* val = ShadowPop(popDepth).value;
	Type* opType = GetTypeByDepth(popDepth);

	// 2. Next: Amt (всегда 16 бит в стеке, но эффективно 8 бит)
	Value* amt16 = ShadowPop(BitDepth_16).value;
	Value* amt8 = builder->CreateTrunc(amt16, i8);
	Value* amt = builder->CreateZExt(amt8, opType); // Приводим к типу значения для сдвига

	// 3. Вычисляем результат
	Value* result = builder->CreateLShr(val, amt);

	auto pushDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;

	ShadowPush(result, pushDepth);

	FlagsPromise flagsPromise;
	flagsPromise.op1 = val;
	flagsPromise.op2 = amt;
	flagsPromise.operation = FlagsPromise::SHR;
	flagsPromise.res = nullptr;
	flagsPromise.opType = opType;

	ShadowStackSlot flagsSlot;
	flagsSlot.isInStackAddress = false;
	flagsSlot.isPointerToVirtualStackSlot = nullptr;
	flagsSlot.bitDepth = BitDepth_64;
	flagsSlot.flagsPromise = flagsPromise;

	shadowStack.push_back(flagsSlot);

	if (logDebugMessage) {
		builder->CreateCall(jitLogShl, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(val, i64), builder->CreateZExt(amt, i64),
			builder->CreateZExt(result, i64), builder->getInt64(0)
			});
	}
}

void LLVMBasicBlockLifter::LiftVmShld(bool logDebugMessage, const HandlerMatch& match) {
	using namespace llvm;

	// Stack: Dst, Src, Count
	auto popDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;
	llvm::Value* dst = ShadowPop(popDepth).value;
	if (match.bitDepth == BitDepth_8) {
		dst = builder->CreateTrunc(dst, i8);
	}
	llvm::Value* src = ShadowPop(popDepth).value;
	if (match.bitDepth == BitDepth_8) {
		src = builder->CreateTrunc(src, i8);
	}
	llvm::Value* count16 = ShadowPop(BitDepth_16).value;
	llvm::Value* count8 = builder->CreateTrunc(count16, i8);
	llvm::Value* count = builder->CreateZExt(count8, dst->getType());

	llvm::Type* opType = dst->getType();
	auto pushDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;


	// Интринсик fshl: (Val1 << Amt) | (Val2 >> (Width - Amt))
	// Для SHLD(Dst, Src, Count): Result = (Dst << Count) | (Src >> (Width - Count))
	// Значит аргументы fshl: Op1=Dst, Op2=Src, Amt=Count

	auto fshlFunc = Intrinsic::getDeclaration(llvmModule, Intrinsic::fshl, { opType });
	llvm::Value* result = builder->CreateCall(fshlFunc, { dst, src, count });


	ShadowPush(result, pushDepth);
	
	FlagsPromise flagsPromise;
	flagsPromise.op1 = dst;
	flagsPromise.op2 = src;
	flagsPromise.op3 = count;
	flagsPromise.operation = FlagsPromise::SHLD;
	flagsPromise.res = nullptr;
	flagsPromise.opType = opType;

	ShadowStackSlot flagsSlot;
	flagsSlot.isInStackAddress = false;
	flagsSlot.isPointerToVirtualStackSlot = nullptr;
	flagsSlot.bitDepth = BitDepth_64;
	flagsSlot.flagsPromise = flagsPromise;

	shadowStack.push_back(flagsSlot);

	if (logDebugMessage) {
		builder->CreateCall(jitLogShld, {
		   builder->getInt32(match.bitDepth),
		   builder->CreateZExt(src, i64), builder->CreateZExt(dst, i64),
		   builder->CreateTrunc(count, i8),
		   builder->CreateZExt(result, i64), builder->getInt64(0)
			});
	}
}
void LLVMBasicBlockLifter::LiftVmShrd(bool logDebugMessage, const HandlerMatch& match) {
	using namespace llvm;

	// Stack: Dst, Src, Count
	auto popDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;
	llvm::Value* dst = ShadowPop(popDepth).value;
	if (match.bitDepth == BitDepth_8) {
		dst = builder->CreateTrunc(dst, i8);
	}
	llvm::Value* src = ShadowPop(popDepth).value;
	if (match.bitDepth == BitDepth_8) {
		src = builder->CreateTrunc(src, i8);
	}
	llvm::Value* count16 = ShadowPop(BitDepth_16).value;
	llvm::Value* count8 = builder->CreateTrunc(count16, i8);
	llvm::Value* count = builder->CreateZExt(count8, dst->getType());

	llvm::Type* opType = dst->getType();
	auto pushDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;

	// Intrinsic fshr: (Val1 >> Amt) | (Val2 << (Width - Amt))
	// Для SHRD(Dst, Src, Count): Result = (Dst >> Count) | (Src << (Width - Count))
	// Args: Op1=Src, Op2=Dst, Amt=Count (Обратите внимание на порядок для FSHR!)
	// fshr(High, Low, Amt). High bits come from Src? No.
	// SHRD fills Dst from Src (High bits). 
	// Правильно: fshr(Op1=Src, Op2=Dst, Amt).

	auto fshrFunc = Intrinsic::getDeclaration(llvmModule, Intrinsic::fshr, { opType });
	llvm::Value* result = builder->CreateCall(fshrFunc, { src, dst, count });


	ShadowPush(result, pushDepth);
	
	FlagsPromise flagsPromise;
	flagsPromise.op1 = dst;
	flagsPromise.op2 = src;
	flagsPromise.op3 = count;
	flagsPromise.operation = FlagsPromise::SHRD;
	flagsPromise.res = nullptr;
	flagsPromise.opType = opType;

	ShadowStackSlot flagsSlot;
	flagsSlot.isInStackAddress = false;
	flagsSlot.isPointerToVirtualStackSlot = nullptr;
	flagsSlot.bitDepth = BitDepth_64;
	flagsSlot.flagsPromise = flagsPromise;

	shadowStack.push_back(flagsSlot);

	if (logDebugMessage) {
		builder->CreateCall(jitLogShrd, {
		   builder->getInt32(match.bitDepth),
		   builder->CreateZExt(src, i64), builder->CreateZExt(dst, i64),
		   builder->CreateTrunc(count, i8),
		   builder->CreateZExt(result, i64), builder->getInt64(0)
			});
	}
}

std::pair<int, int64_t> LLVMBasicBlockLifter::FindSlotByOffset(ShadowStackSlot* startSlot, int64_t offset) {
	if (shadowStack.empty()) return { -1, 0 };
	if (offset < 0) { __debugbreak(); return { -1, 0 }; }

	// 1. Находим индекс стартового слота
	// Так как это deque, память не непрерывная, адресная арифметика не сработает.
	// Приходится искать перебором. Оптимизация: ищем с конца, так как работаем с верхушкой.
	int startPoint = -1;
	for (int i = shadowStack.size() - 1; i >= 0; --i) {
		if (&shadowStack[i] == startSlot) {
			startPoint = i;
			break;
		}
	}

	if (startPoint == -1) return { -1, 0 };
	if (offset == 0) return { startPoint, 0 };

	// 2. Сканируем "вглубь" стека (в сторону уменьшения индексов)
	int64_t currentScannedOffset = 0;

	// ЦИКЛ ИДЕТ ВНИЗ (--i), так как offset увеличивает адрес (к старым элементам)
	for (int i = startPoint; i >= 0; --i) {
		auto& slot = shadowStack[i];

		int64_t slotSize = (slot.bitDepth == BitDepth_8) ? 1 : (slot.bitDepth / 8);

		// Диапазон: [current, current + size)
		if (offset < (currentScannedOffset + slotSize)) {
			// Нашли!
			int64_t offsetInThisSlot = offset - currentScannedOffset;
			return { i, offsetInThisSlot };
		}

		currentScannedOffset += slotSize;
	}

	// Уперлись в дно стека (индекс < 0)
	return { -1, 0 };
}

bool LLVMBasicBlockLifter::TryGenerateShadowStackAccess(
	ShadowStackPop* targetAddr,
	HandlerBitDepth dataType,
	llvm::Value* valueToWrite
) {
	auto slotInfo = FindSlotByOffset(
		targetAddr->isPointerToVirtualStackSlot, 
		targetAddr->relativeOffset
	);
	if (slotInfo.first == -1) { return false; }

	int currentIndex = slotInfo.first;
	int64_t currentOffsetInSlot = slotInfo.second;

	uint64_t totalBytes = dataType / 8;
	if (totalBytes == 0) return false;

	// =========================================================
	// ВЕТКА ЗАПИСИ (WRITE)
	// =========================================================
	if (valueToWrite != nullptr) {
		uint64_t bytesWritten = 0;

		// Приведение типов (Ptr -> Int, Bitcast)
		llvm::Value* sourceInt = valueToWrite;

		while (bytesWritten < totalBytes) {
			// ПРОВЕРКА: Если индекс ушел в минус, мы вышли за дно стека
			if (currentIndex < 0) {
				__debugbreak(); // Stack Underflow / Access Violation
				return false;
			}

			ShadowStackSlot& slot = shadowStack[currentIndex];

			uint64_t slotSize = slot.bitDepth / 8;
			uint64_t bytesAvailableInSlot = slotSize - currentOffsetInSlot;
			uint64_t bytesRemaining = totalBytes - bytesWritten;
			uint64_t bytesToProcess = std::min(bytesAvailableInSlot, bytesRemaining);

			// --- Подготовка значения слота ---
			llvm::Value* slotVal = slot.value;
			if (slotVal->getType()->isPointerTy()) {
				slotVal = builder->CreatePtrToInt(slotVal, builder->getInt64Ty(), "slot_ptr2int");
				slot.isInStackAddress = false;
				slot.isPointerToVirtualStackSlot = nullptr;
			}
			else if (slotVal->getType()->getIntegerBitWidth() < 64) {
				slotVal = builder->CreateZExt(slotVal, builder->getInt64Ty(), "slot_zext");
			}

			// --- 1. Подготовка куска (Chunk) ---
			llvm::Value* chunk = sourceInt;
			if (bytesWritten > 0) {
				chunk = builder->CreateLShr(chunk, bytesWritten * 8, "write_chunk_shr");
			}
			chunk = builder->CreateTrunc(chunk, i64, "write_chunk_trunc");

			uint64_t chunkMaskVal = (bytesToProcess == 8) ? -1ULL : ((1ULL << (bytesToProcess * 8)) - 1);
			chunk = builder->CreateAnd(chunk, builder->getInt64(chunkMaskVal));

			// --- 2. Очистка (RMW) ---
			llvm::APInt maskAPI(64, chunkMaskVal);
			maskAPI = maskAPI.shl(currentOffsetInSlot * 8);
			llvm::Value* clearMask = builder->getIntN(64, ~maskAPI.getZExtValue());

			slotVal = builder->CreateAnd(slotVal, clearMask, "slot_clear");

			// --- 3. Вставка ---
			llvm::Value* shiftedChunk = builder->CreateShl(chunk, currentOffsetInSlot * 8, "chunk_shl");
			slotVal = builder->CreateOr(slotVal, shiftedChunk, "slot_update");

			// --- 4. Сохранение ---
			if (slot.bitDepth != BitDepth_64) {
				slot.value = builder->CreateTrunc(slotVal, GetTypeByDepth((HandlerBitDepth)slot.bitDepth), "slot_restore");
			}
			else {
				slot.value = slotVal;
			}

			// --- ОБНОВЛЕНИЕ ИТЕРАТОРОВ (ИДЕМ ВНИЗ) ---
			bytesWritten += bytesToProcess;
			currentOffsetInSlot = 0;
			currentIndex--; // <--- ДВИЖЕНИЕ К ИНДЕКСУ 0 (старые записи, след. адрес памяти)
		}

		return true;
	}
	// =========================================================
	// ВЕТКА ЧТЕНИЯ (READ)
	// =========================================================
	else {
		uint64_t bytesRead = 0;
		llvm::Type* accumulatorType = GetTypeByDepth(dataType);
		llvm::Value* resultAcc = CreateValueByDepth(dataType, 0);

		while (bytesRead < totalBytes) {
			// ПРОВЕРКА: Выход за дно стека
			if (currentIndex < 0) {
				__debugbreak();
				return false;
			}

			ShadowStackSlot& slot = shadowStack[currentIndex];
			uint64_t slotSize = (slot.bitDepth == BitDepth_8) ? 1 : (slot.bitDepth / 8);
			uint64_t bytesAvailableInSlot = slotSize - currentOffsetInSlot;
			uint64_t bytesRemaining = totalBytes - bytesRead;
			uint64_t bytesToProcess = std::min(bytesAvailableInSlot, bytesRemaining);

			// --- 1. Извлечение ---
			llvm::Value* slotVal = slot.value;
			if (slotVal->getType()->isPointerTy()) {
				slotVal = builder->CreatePtrToInt(slotVal, builder->getInt64Ty(), "read_ptr2int");
			}
			else if (slotVal->getType()->getIntegerBitWidth() < 64) {
				slotVal = builder->CreateZExt(slotVal, builder->getInt64Ty(), "read_zext");
			}

			// --- 2. Сдвиг и обрезка ---
			if (currentOffsetInSlot > 0) {
				slotVal = builder->CreateLShr(slotVal, currentOffsetInSlot * 8, "read_shr");
			}
			llvm::Value* chunk = builder->CreateTrunc(slotVal, builder->getIntNTy(bytesToProcess * 8), "read_trunc");

			// --- 3. Вставка в аккумулятор ---
			llvm::Value* chunkExt = builder->CreateZExt(chunk, accumulatorType, "read_ext");
			if (bytesRead > 0) {
				chunkExt = builder->CreateShl(chunkExt, bytesRead * 8, "read_shl");
			}
			resultAcc = builder->CreateOr(resultAcc, chunkExt, "read_or");

			// --- ОБНОВЛЕНИЕ ИТЕРАТОРОВ (ИДЕМ ВНИЗ) ---
			bytesRead += bytesToProcess;
			currentOffsetInSlot = 0;
			currentIndex--; // <--- ДВИЖЕНИЕ К ИНДЕКСУ 0
		}

		// PUSH результата
		HandlerBitDepth resDepth = BitDepth_64;
		if (totalBytes == 1) resDepth = BitDepth_16;
		else if (totalBytes == 2) resDepth = BitDepth_16;
		else if (totalBytes == 4) resDepth = BitDepth_32;

		ShadowPush(resultAcc, resDepth);
		return true;
	}

	return false;
}

void LLVMBasicBlockLifter::GenerateStackMemoryAccess(
	llvm::Value* targetAddr,
	llvm::Type* dataType,
	llvm::Value* valueToWrite, // Если nullptr, то это READ
	bool useHooks
) {
	if (!targetAddr->getType()->isPointerTy()) {
		__debugbreak(); // Ожидаем указатель на стек
		assert(false && "Expected pointer type for stack access");
	}
	auto* typedPtr = builder->CreateBitCast(targetAddr, dataType->getPointerTo(), "vsp_base_typed");
	if (valueToWrite) { // WRITE
		builder->CreateStore(valueToWrite, typedPtr);
	}
	else { // READ
		llvm::Value* loadedStackVal = builder->CreateLoad(dataType, typedPtr, "stack_load");
		auto bitWidth = dataType->getIntegerBitWidth();
		auto pushDepth = (bitWidth == 8) ? BitDepth_16 : static_cast<HandlerBitDepth>(bitWidth);
		ShadowPush(loadedStackVal, pushDepth);
	}
}
void LLVMBasicBlockLifter::GenerateRamMemoryAccess(
	llvm::Value* targetAddr,
	llvm::Type* dataType,
	llvm::Value* valueToWrite, // Если nullptr, то это READ
	bool useHooks) {

	using namespace llvm;
	Value* addrInt = targetAddr; 
	if (addrInt->getType()->isPointerTy()) {
		__debugbreak(); // Ожидаем адрес в виде i64, а не указателя
		assert(false && "Expected integer type for RAM address");
	}
	Value* loadedRamVal = nullptr;
	if (useHooks) {
		if (valueToWrite) { // WRITE HOOK
			// LLVM_JIT_WriteMem(addr, val, bits)
			builder->CreateCall(jitWriteFunc, {
				addrInt,
				builder->CreateZExt(valueToWrite, i64),
				builder->getInt32(dataType->getIntegerBitWidth())
				});
		}
		else { // READ HOOK
			// LLVM_JIT_ReadMem(addr, bits)
			Value* readRes = builder->CreateCall(jitReadFunc, {
				addrInt,
				builder->getInt32(dataType->getIntegerBitWidth())
				});
			loadedRamVal = builder->CreateTrunc(readRes, dataType);
		}
	}
	else {
		// RELEASE MODE: Raw Pointers
		Value* ptrRam = builder->CreateIntToPtr(addrInt, dataType->getPointerTo());
		if (valueToWrite) {
			builder->CreateStore(valueToWrite, ptrRam);
		}
		else {
			loadedRamVal = builder->CreateLoad(dataType, ptrRam, "ram_load");
		}
	}

	if (loadedRamVal) {
		unsigned bitWidth = dataType->getIntegerBitWidth();
		if (bitWidth == 8) bitWidth = 16; // Правило для 8 бит: расширяем до 16 при чтении из RAM
		ShadowPush(loadedRamVal, static_cast<HandlerBitDepth>(bitWidth));
	}

}
void LLVMBasicBlockLifter::GenerateMemoryAccess(
	llvm::Value* targetAddr,
	llvm::Type* dataType,
	llvm::Value* valueToWrite, // Если nullptr, то это READ
	bool useHooks,
	bool isProvenStackAddress)
{
	
	using namespace llvm;
	//FlushVsp(true);

	if (isProvenStackAddress) {
		GenerateStackMemoryAccess(targetAddr, dataType, valueToWrite, useHooks);
		return;
	}

	GenerateRamMemoryAccess(targetAddr, dataType, valueToWrite, useHooks);
	return;

}


void LLVMBasicBlockLifter::LiftVmEntry(const VmEntryHandlerData& data) {
	// 1. Пушим заглушки (как в оригинале VMP)
	ShadowPush(builder->getInt64(0), BitDepth_64);
	ShadowPush(builder->getInt64(0), BitDepth_64);

	// 2. Пушим регистры из NativeContext в виртуальный стек
	// data.popedRegsOrder содержит порядок, в котором VMP ожидает регистры в стеке
	for (auto& reg : data.popedRegsOrder) {
		int64_t index = GetNativeOffset(reg.id);

		if (index == -1) continue;

		// Используем CreateStructGEP для доступа к полям структуры
		// nativeContextType — это llvm::StructType*
		// nativeContextPtr — это указатель на эту структуру (аргумент функции)
		llvm::Value* regPtr = builder->CreateStructGEP(
			nativeContextType,
			nativeContext,
			static_cast<unsigned int>(index),
			"native_reg_" + reg.name
		);

		llvm::Value* regVal = builder->CreateLoad(i64, regPtr, "reg_val");

		ShadowPush(regVal, BitDepth_64);
	}

	// 3. Пушим ImageBaseDifference (аргумент функции)
	ShadowPush(builder->getInt64(0), BitDepth_64);
}
void LLVMBasicBlockLifter::LiftVmJmpIndirect(bool logDebugMessage, const VmJmpData& jmpData) {
	llvm::Value* rawAddr = ShadowPop(BitDepth_64).value;
	//auto* targetAddr = builder->CreateAdd(
	//	rawAddr,
	//	builder->getInt64(jmpData.newVipShift),
	//	"jmp_target_addr");

	auto* targetAddr = rawAddr;

	std::cout << "ShadowStack size before JMP: " << shadowStack.size() << std::endl;

	// 2. Логирование
	builder->CreateCall(jitLogJmpIndirect, { targetAddr });

	// 3. Пишем адрес в аллоку targetVip, чтобы диспетчер его увидел
	builder->CreateStore(targetAddr, targetVip);

	// 4. Сбрасываем теневой стек в память, так как переходим в другой блок
	FlushVsp(true);

}
void LLVMBasicBlockLifter::LiftVmExit(bool logDebugMessage, const VmExitData& data) {
	// 1. Выгружаем регистры из виртуального стека обратно в NativeContext
	for (auto& reg : data.popedRegsOrder) {
		auto slot = ShadowPop(BitDepth_64);
		auto* value = slot.value;

		int64_t index = GetNativeOffset(reg.id);

		if (index == -1) continue;

		llvm::Value* regPtr = builder->CreateStructGEP(
			nativeContextType,
			nativeContext,
			static_cast<unsigned int>(index),
			"native_reg_" + reg.name
		);

		if (!slot.isInStackAddress) 
			builder->CreateStore(value, regPtr);
		else 
			builder->CreateStore(builder->getInt64(slot.slotAbsoluteBase + slot.relativeOffset), regPtr);
	}

	// 2. Выгружаем адрес выхода (VmExitAddr)
	// Обычно VMP хранит его в одном из слотов контекста (например, вместо одного из регистров или в спец. поле)
	{
		auto* value = ShadowPop(BitDepth_64).value;

		// vmExitAddr - это индекс в структуре (int)
		llvm::Value* regPtr = builder->CreateStructGEP(
			nativeContextType,
			nativeContext,
			static_cast<unsigned int>(vmExitAddr),
			"vm_exit_addr"
		);
		builder->CreateStore(value, regPtr);
	}

	// 3. Выгружаем адрес возврата (RetAddr)
	{
		auto* value = ShadowPop(BitDepth_64).value;

		// retAddrOffset - это индекс в структуре (int)
		llvm::Value* regPtr = builder->CreateStructGEP(
			nativeContextType,
			nativeContext,
			static_cast<unsigned int>(retAddrOffset),
			"vm_ret_addr"
		);
		builder->CreateStore(value, regPtr);
	}

	// 4. Сигнализируем Диспетчеру (TraceLifter), что нужно остановиться.
	// Запись 0 в targetVip приведет к выходу из switch-цикла диспетчера.
	builder->CreateStore(builder->getInt64(0), targetVip, true);

}