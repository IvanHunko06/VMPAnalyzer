#include "LLVMBasicBlockLifter.hpp"
#include "NativeContext.hpp"

LLVMBasicBlockLifter::LLVMBasicBlockLifter(const BasicBlockLifterConstructor& ctor) :
	shadowStack(ctor.builder, std::bind(&LLVMBasicBlockLifter::PopWithConstOffset, this, std::placeholders::_1))
{
	using namespace llvm;

	this->context = ctor.context;
	this->llvmModule = ctor.llvmModule;
	this->builder = ctor.builder;
	this->function = ctor.function;

	this->vspPtr = ctor.vsp_ptr;
	this->vspBasePtr = ctor.vspBasePtr;
	this->nativeContext = ctor.nativeContext;
	this->nativeContextType = ctor.nativeContextType;
	this->imageBaseDif = ctor.imageBaseDif;
	this->targetVip = ctor.targetVip;
	this->virtualContext = ctor.virtualContext;
	this->realStackPtr = ctor.realStackPtr;

	i64 = Type::getInt64Ty(*context);
	i32 = Type::getInt32Ty(*context);
	i16 = Type::getInt16Ty(*context);
	i8 = Type::getInt8Ty(*context);
	voidTy = Type::getVoidTy(*context);
	ptrTy = Type::getInt64PtrTy(*context);

	InitJitHooks();
	InitLogFunctions();
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
	shadowStack.Clear();
	calculateFlagsPromises.clear();
	addressInRegMetas.clear();

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
			LiftVmJmpIndirect(logMessages, std::get<VmJmpData>(handler.matchData));
			break;
		case Handler_VmExit:
			LiftVmExit(logMessages, std::get<VmExitData>(handler.matchData));
			break;
		}

		//if (!shadowStack.empty()) {
		//	auto& lastSlot = shadowStack.back();
		//	if (lastSlot.value != nullptr &&
		//		lastSlot.value->getType()->isPointerTy() &&
		//		!lastSlot.isInStackAddress)
		//		__debugbreak();
		//}

	}

	return basicBlock;
}

void LLVMBasicBlockLifter::PushWithConstOffset(llvm::Value* value, HandlerBitDepth bitDepth, bool actuallyWriteToRealStack) {
	int64_t size = bitDepth / 8; // Упрощенно
	virtualStackOffset -= size;  // Стек растет вниз

	llvm::Value* baseAsI8Ptr = builder->CreateBitCast(vspPtr, i8->getPointerTo());

	llvm::Value* ptr = builder->CreateConstInBoundsGEP1_64(
		i8,
		baseAsI8Ptr,
		virtualStackOffset
	);

	llvm::Value* typedPtr = builder->CreateBitCast(
		ptr,
		GetTypeByDepth(bitDepth)->getPointerTo()
	);
	builder->CreateStore(value, typedPtr);

	realStackOffset -= size;

	if (actuallyWriteToRealStack) {
		auto* realStackI8 = builder->CreateBitCast(realStackPtr, i8->getPointerTo(), "real_stack_i8");
		auto* realStackGep = builder->CreateConstGEP1_64(i8, realStackI8, realStackOffset, "real_stack_gep");
		auto* realTypedPtr = builder->CreateBitCast(
			realStackGep,
			GetTypeByDepth(bitDepth)->getPointerTo(),
			"typed_real_stack_gep"
		);
		builder->CreateStore(value, realTypedPtr);
	}
}
llvm::Value* LLVMBasicBlockLifter::PopWithConstOffset(HandlerBitDepth bitDepth) {
	int64_t size = bitDepth / 8;

	// 1. Читаем по текущему КОНСТАНТНОМУ смещению
	llvm::Value* baseAsI8Ptr = builder->CreateBitCast(vspPtr, i8->getPointerTo());

	// 2. Используем GEP с ОДНИМ индексом
	llvm::Value* ptr = builder->CreateConstInBoundsGEP1_64(
		i8,          // Теперь мы говорим: "шагаем по i8"
		baseAsI8Ptr, // От указателя i8*
		virtualStackOffset  // На offset шагов
	);

	llvm::Value* typedPtr = builder->CreateBitCast(ptr, GetTypeByDepth(bitDepth)->getPointerTo());
	llvm::Value* val = builder->CreateLoad(GetTypeByDepth(bitDepth), typedPtr);

	// 2. Меняем оффсет в C++
	virtualStackOffset += size;
	realStackOffset += size;
	return val;
}

void LLVMBasicBlockLifter::FlushVsp(bool clearStack, bool writeToRealStack) {
	for (auto& slot : shadowStack) {
		auto& metadata = slot.metadata;
		if (metadata.flagsPromise.has_value()) {
			slot.value = CalculateFlagsFromPromise(*metadata.flagsPromise);
		}
		if (metadata.isPadding) {
			virtualStackOffset += slot.bitDepth;
			realStackOffset += slot.bitDepth;
			continue;
		}

		auto& addressMeta = metadata.addressMeta;
		if (addressMeta.isInStackAddress) {
			addressMetasStorage[virtualStackOffset - slot.bitDepth] = addressMeta;
		}

		PushWithConstOffset(
			slot.value,
			static_cast<HandlerBitDepth>(slot.bitDepth),
			writeToRealStack
		);
	}

	if (clearStack) shadowStack.Clear();
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
	auto& data = std::get<VmContextAccessData>(match.matchData);
	// 1. Выравнивание (Align down to 8 bytes)
	int32_t baseOffset = (data.offset / 8) * 8;

	auto popDepth = match.bitDepth == BitDepth_8 ? BitDepth_16 : match.bitDepth; // Если 8 бит, то читаем 16 бит для выравнивания
	auto popResult = Pop(popDepth);

	if (calculateFlagsPromises.contains(baseOffset)) {
		calculateFlagsPromises.erase(baseOffset);
	}

	if (popResult.metadata.flagsPromise.has_value()) {
		calculateFlagsPromises[baseOffset] = popResult.metadata.flagsPromise.value();
		return;
	}

	if (popResult.value->getType()->isPointerTy()) {
		popResult.value = builder->CreatePtrToInt(
			popResult.value,
			GetTypeByDepth(popDepth),
			"pop_ptr_to_int"
		);
	}

	if (match.bitDepth == BitDepth_8) {
		// Если нам нужно 8 бит, а в стеке 16 бит, то извлекаем нужные 8 бит из результата
		popResult.value = builder->CreateTrunc(
			popResult.value,
			i8,
			"pop_trunc_8"
		);
	}

	auto* virtualContextI8 = builder->CreateBitCast(virtualContext, i8->getPointerTo());
	auto* slotPtrI8 = builder->CreateConstInBoundsGEP1_32(i8, virtualContextI8, data.offset);
	auto* storeType = GetTypeByDepth(match.bitDepth);
	auto* typedSlotPtr = builder->CreateBitCast(slotPtrI8, storeType->getPointerTo());
	builder->CreateStore(popResult.value, typedSlotPtr);

	if (popResult.metadata.addressMeta.isInStackAddress) {
		addressInRegMetas[baseOffset] = popResult.metadata.addressMeta;
	}
	else {
		addressInRegMetas.erase(baseOffset);
	}

	// --- LOGGING ---
	if (logDebugMessage) {
		llvm::Value* val64 = popResult.value;
		if (match.bitDepth != BitDepth_64) {
			val64 = builder->CreateZExt(val64, i64);
		}

		builder->CreateCall(jitLogPop, {
			builder->getInt32(match.bitDepth),
			builder->getInt64(data.offset),
			val64
			});
	}
	// ----------------

}
void LLVMBasicBlockLifter::LiftVmPushReg(bool logDebugMessage, const HandlerMatch& match) {
	auto& data = std::get<VmContextAccessData>(match.matchData);

	llvm::Value* valueToPush = nullptr;
	int32_t baseOffset = (data.offset / 8) * 8;

	auto* virtualContextI8 = builder->CreateBitCast(virtualContext, i8->getPointerTo());
	auto* slotPtrI8 = builder->CreateConstInBoundsGEP1_32(i8, virtualContextI8, data.offset);
	auto* loadType = GetTypeByDepth(match.bitDepth);
	auto* typedSlotPtr = builder->CreateBitCast(slotPtrI8, loadType->getPointerTo());

	bool isFlagPromise = calculateFlagsPromises.contains(baseOffset);
	if (match.bitDepth == BitDepth_64 && isFlagPromise) {
		auto& promise = calculateFlagsPromises[baseOffset];

		if (promise.res == nullptr) {
			promise.res = CalculateFlagsFromPromise(promise);
			builder->CreateStore(promise.res, typedSlotPtr);
		}

		Push(promise.res, BitDepth_64);
		return;
	}

	auto pushDepth = match.bitDepth == BitDepth_8 ? BitDepth_16 : match.bitDepth;

	valueToPush = builder->CreateLoad(loadType, typedSlotPtr);
	if (match.bitDepth == BitDepth_8) {
		valueToPush = builder->CreateZExt(valueToPush, i16);
	}

	StackMetadata meta;
	auto addressMeta = addressInRegMetas.find(baseOffset);
	if (addressMeta != addressInRegMetas.end()) {
		meta.addressMeta = addressMeta->second;
	}

	shadowStack.Push(pushDepth, valueToPush, meta);

	// --- LOGGING ---
	if (logDebugMessage) {
		llvm::Value* val64 = valueToPush;
		if (match.bitDepth != BitDepth_64) {
			val64 = builder->CreateZExt(val64, i64);
		}

		builder->CreateCall(jitLogPushReg, {
			builder->getInt32(match.bitDepth),
			builder->getInt64(data.offset),
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
		Push(constVal16, BitDepth_16);
	}
	else {
		llvm::Value* constVal = CreateValueByDepth(match.bitDepth, data.value);
		Push(constVal, match.bitDepth);
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
		auto saddFunc = Intrinsic::getDeclaration(llvmModule, Intrinsic::sadd_with_overflow, { promise.opType });
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

	auto shadowStackSize = shadowStack.GetFullShadowStackSize();
	auto finalOffset = virtualStackOffset - shadowStackSize;

	// --- LOGGING ---
	if (logDebugMessage) {
		// Читаем текущий адрес стека (значение переменной vsp_ptr)
		llvm::Value* currentVsp = builder->CreateBitCast(vspPtr, ptrTy);

		llvm::Value* vspAsInt = builder->CreatePtrToInt(currentVsp, GetTypeByDepth(match.bitDepth), "vsp_int");
		llvm::Value* val64 = (vspAsInt->getType()->isIntegerTy(64))
			? vspAsInt
			: builder->CreateZExt(vspAsInt, builder->getInt64Ty());

		auto* offset = builder->getInt64(finalOffset);
		val64 = builder->CreateAdd(val64, offset);
		builder->CreateCall(jitLogPushVsp, {
			builder->getInt32(match.bitDepth),
			val64
			});
	}

	auto pushDepth = match.bitDepth == BitDepth_8 ? BitDepth_16 : match.bitDepth;

	StackMetadata metadata;
	metadata.addressMeta.isInStackAddress = match.bitDepth == BitDepth_64;
	metadata.addressMeta.slotAbsoluteBase = finalOffset;

	auto* vspValue = CreateValueByDepth(
		pushDepth,
		fakeStackBase + finalOffset
	);


	shadowStack.Push(pushDepth, vspValue, metadata);
}
void LLVMBasicBlockLifter::LiftVmPopVsp(bool logDebugMessage, const HandlerMatch& match) {
	auto vspPopData = std::get<VmPopVspData>(match.matchData);

	int64_t offset = vspPopData.offset;

	auto targetAddr = Pop(match.bitDepth).value;
	//auto constTargetAddr = GetConstantInt(targetAddr);
	//if (constTargetAddr) {
	//	uint64_t currentAddr = fakeStackBase - virtualStackOffset - shadowStack.GetFullShadowStackSize();
	//	offset = *constTargetAddr - currentAddr;
	//}

	StackMetadata meta;
	meta.isPadding = true;



	shadowStack.Push(
		(HandlerBitDepth)offset,
		nullptr,
		meta);

	if (vspPopData.offset > 0) {
		FlushVsp(true, false);
	}

	//FlushVsp(true);

	// --- LOGGING ---

	builder->CreateCall(jitLogPopVsp, {
		builder->getInt32(match.bitDepth),
		builder->getInt64(offset) // Логируем смещение, на которое был изменен VSP
		});
	// ----------------

}

void LLVMBasicBlockLifter::LiftVmReadMem(bool logDebugMessage, const HandlerMatch& match) {
	auto addrSlot = Pop(BitDepth_64);
	auto& addrMeta = addrSlot.metadata.addressMeta;

	auto* targetAddr = addrSlot.value;
	if (addrMeta.isInStackAddress) {
		int64_t fullOffset = addrMeta.slotAbsoluteBase + addrMeta.relativeOffset;
		auto* stackBaseI8 = builder->CreateBitCast(vspPtr, i8->getPointerTo());
		targetAddr = builder->CreateConstInBoundsGEP1_32(i8, stackBaseI8, fullOffset);
	}

	GenerateMemoryAccess(
		targetAddr,
		nullptr,
		GetTypeByDepth(match.bitDepth),
		nullptr,
		false,
		addrMeta.isInStackAddress // <--- Передаем флаг
	);
}
void LLVMBasicBlockLifter::LiftVmReadMemHooked(bool logDebugMessage, const HandlerMatch& match) {
	auto addrSlot = Pop(BitDepth_64);
	auto& addrMeta = addrSlot.metadata.addressMeta;

	auto* targetAddr = addrSlot.value;
	if (addrMeta.isInStackAddress) {
		int64_t fullOffset = addrMeta.slotAbsoluteBase + addrMeta.relativeOffset;
		auto* stackBaseI8 = builder->CreateBitCast(vspPtr, i8->getPointerTo());
		targetAddr = builder->CreateConstInBoundsGEP1_32(i8, stackBaseI8, fullOffset);
	}

	GenerateMemoryAccess(
		targetAddr,
		nullptr,
		GetTypeByDepth(match.bitDepth),
		nullptr,
		true,
		addrMeta.isInStackAddress // <--- Передаем флаг
	);

}
void LLVMBasicBlockLifter::LiftVmWriteMem(bool logDebugMessage, const HandlerMatch& match) {
	llvm::Value* val;
	llvm::Type* type;

	auto addrSlot = Pop(BitDepth_64);

	if (match.bitDepth == BitDepth_8) {
		// Пишем 8 бит, но на стеке лежит 16
		llvm::Value* val16 = Pop(BitDepth_16).value;
		val = builder->CreateTrunc(val16, i8);
		type = i8;
	}
	else {
		val = Pop(match.bitDepth).value;
		type = GetTypeByDepth(match.bitDepth);
	}

	auto& addrMeta = addrSlot.metadata.addressMeta;

	auto* targetAddr = addrSlot.value;
	llvm::Value* realStackAddr = nullptr;
	if (addrMeta.isInStackAddress) {
		int64_t fullOffset = addrMeta.slotAbsoluteBase + addrMeta.relativeOffset;

		auto* stackBaseI8 = builder->CreateBitCast(vspPtr, i8->getPointerTo());
		targetAddr = builder->CreateConstInBoundsGEP1_32(i8, stackBaseI8, fullOffset);

		auto* realStackI8 = builder->CreateBitCast(realStackPtr, i8->getPointerTo());
		realStackAddr = builder->CreateConstInBoundsGEP1_32(i8, realStackI8, fullOffset);
	}

	GenerateMemoryAccess(
		targetAddr,
		realStackAddr,
		type,
		val,
		false,
		addrMeta.isInStackAddress
	);
}
void LLVMBasicBlockLifter::LiftVmWriteMemHooked(bool logDebugMessage, const HandlerMatch& match) {
	llvm::Value* val;
	llvm::Type* type;

	auto addrSlot = Pop(BitDepth_64);

	if (match.bitDepth == BitDepth_8) {
		// Пишем 8 бит, но на стеке лежит 16
		llvm::Value* val16 = Pop(BitDepth_16).value;
		val = builder->CreateTrunc(val16, i8);
		type = i8;
	}
	else {
		val = Pop(match.bitDepth).value;
		type = GetTypeByDepth(match.bitDepth);
	}

	auto& addrMeta = addrSlot.metadata.addressMeta;

	auto* targetAddr = addrSlot.value;
	llvm::Value* realStackAddr = nullptr;
	if (addrMeta.isInStackAddress) {
		int64_t fullOffset = addrMeta.slotAbsoluteBase + addrMeta.relativeOffset;

		auto* stackBaseI8 = builder->CreateBitCast(vspPtr, i8->getPointerTo());
		targetAddr = builder->CreateConstInBoundsGEP1_32(i8, stackBaseI8, fullOffset);

		auto* realStackI8 = builder->CreateBitCast(realStackPtr, i8->getPointerTo());
		realStackAddr = builder->CreateConstInBoundsGEP1_32(i8, realStackI8, fullOffset);
	}

	GenerateMemoryAccess(
		targetAddr,
		realStackAddr,
		type,
		val,
		true,
		addrMeta.isInStackAddress
	);
}


void LLVMBasicBlockLifter::LiftVmAdd(bool logDebugMessage, const HandlerMatch& match) {
	using namespace llvm;

	llvm::Value* valA, * valB;
	llvm::Type* calcType;
	HandlerBitDepth pushDepth = match.bitDepth;
	bool hasSignleVspOperand = false;

	HandlerBitDepth popDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;
	auto valBSlot = Pop(popDepth);
	auto valASlot = Pop(popDepth);
	valB = valBSlot.value;
	valA = valASlot.value;
	int64_t slotAbsoluteBase = 0;
	int64_t relativeOffset = 0;

	llvm::Value* result = nullptr;
	auto& valBAddrMeta = valBSlot.metadata.addressMeta;
	auto& valAAddrMeta = valASlot.metadata.addressMeta;

	if (valBAddrMeta.isInStackAddress && !valAAddrMeta.isInStackAddress) {
		slotAbsoluteBase = valBAddrMeta.slotAbsoluteBase;
		//valB = builder->getInt64(
		//	fakeStackBase +
		//	valBAddrMeta.slotAbsoluteBase +
		//	valBAddrMeta.relativeOffset
		//);
		hasSignleVspOperand = true;
		auto constValue = GetConstantInt(valA);
		if (constValue) relativeOffset = valBAddrMeta.relativeOffset + *constValue;
		else __debugbreak();
	}
	else if (valAAddrMeta.isInStackAddress && !valBAddrMeta.isInStackAddress) {
		slotAbsoluteBase = valAAddrMeta.slotAbsoluteBase;
		//valA = builder->getInt64(
		//	fakeStackBase +
		//	valASlot.metadata.slotAbsoluteBase +
		//	valASlot.metadata.relativeOffset
		//);
		hasSignleVspOperand = true;
		auto constValue = GetConstantInt(valB);
		if (constValue) relativeOffset = valAAddrMeta.relativeOffset + *constValue;
		else __debugbreak();
	}

	calcType = GetTypeByDepth(match.bitDepth);
	if (match.bitDepth == BitDepth_8) {
		valB = builder->CreateTrunc(valB, i8);
		valA = builder->CreateTrunc(valA, i8);
		pushDepth = BitDepth_16; // Результат 8-битного сложения всегда 16 бит
		hasSignleVspOperand = false;

	}

	if (hasSignleVspOperand && fakeStackBase + relativeOffset + slotAbsoluteBase > fakeStackBase) {
		hasSignleVspOperand = false;
		relativeOffset = 0;
		slotAbsoluteBase = 0;
	}

	result = builder->CreateAdd(valA, valB, "add_res");
	if (match.bitDepth == BitDepth_8) {
		result = builder->CreateZExt(result, i16);
	}

	StackMetadata resultMeta;
	resultMeta.addressMeta.isInStackAddress = hasSignleVspOperand;
	resultMeta.addressMeta.relativeOffset = relativeOffset;
	resultMeta.addressMeta.slotAbsoluteBase = slotAbsoluteBase;

	shadowStack.Push(pushDepth, result, resultMeta);

	FlagsPromise flagsPromise;
	flagsPromise.op1 = valA;
	flagsPromise.op2 = valB;
	flagsPromise.operation = FlagsPromise::ADD;
	flagsPromise.res = nullptr;
	flagsPromise.opType = calcType;

	StackMetadata flagsMeta;
	flagsMeta.flagsPromise = flagsPromise;

	shadowStack.Push(BitDepth_64, nullptr, flagsMeta);

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
	valB = Pop(popDepth).value;
	valA = Pop(popDepth).value;

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


	Push(result, pushDepth);

	FlagsPromise flagsPromise;
	flagsPromise.op1 = valA;
	flagsPromise.op2 = valB;
	flagsPromise.operation = FlagsPromise::NOR;
	flagsPromise.res = nullptr;
	flagsPromise.opType = flagsType;

	StackMetadata flagsMeta;
	flagsMeta.flagsPromise = flagsPromise;

	shadowStack.Push(BitDepth_64, nullptr, flagsMeta);

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
	valB = Pop(popDepth).value;
	valA = Pop(popDepth).value;

	if (match.bitDepth == BitDepth_8) {
		valB = builder->CreateTrunc(valB, i8);
		valA = builder->CreateTrunc(valA, i8);
		pushDepth = BitDepth_16; // Результат 8-битного NAND всегда 16 бит
		flagsType = i8;
	}

	Value* result = builder->CreateNot(builder->CreateAnd(valA, valB));

	if (match.bitDepth == BitDepth_8) {
		result = builder->CreateZExt(result, i16);
	}


	Push(result, pushDepth);

	FlagsPromise flagsPromise;
	flagsPromise.op1 = valA;
	flagsPromise.op2 = valB;
	flagsPromise.operation = FlagsPromise::NOR;
	flagsPromise.res = nullptr;
	flagsPromise.opType = flagsType;

	StackMetadata flagsMeta;
	flagsMeta.flagsPromise = flagsPromise;

	shadowStack.Push(BitDepth_64, nullptr, flagsMeta);

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
	Value* val = Pop(popDepth).value;
	Type* opType = GetTypeByDepth(popDepth);

	// 2. Next: Amt (всегда 16 бит в стеке, но эффективно 8 бит)
	Value* amt16 = Pop(BitDepth_16).value;
	Value* amt8 = builder->CreateTrunc(amt16, i8);
	Value* amt = builder->CreateZExt(amt8, opType); // Приводим к типу значения для сдвига

	// 3. Вычисляем результат
	Value* result = builder->CreateShl(val, amt);

	auto pushDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;

	Push(result, pushDepth);

	FlagsPromise flagsPromise;
	flagsPromise.op1 = val;
	flagsPromise.op2 = amt;
	flagsPromise.operation = FlagsPromise::SHL;
	flagsPromise.res = nullptr;
	flagsPromise.opType = opType;

	StackMetadata meta;
	meta.flagsPromise = flagsPromise;
	shadowStack.Push(BitDepth_64, nullptr, meta);

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
	Value* val = Pop(popDepth).value;
	Type* opType = GetTypeByDepth(popDepth);

	// 2. Next: Amt (всегда 16 бит в стеке, но эффективно 8 бит)
	Value* amt16 = Pop(BitDepth_16).value;
	Value* amt8 = builder->CreateTrunc(amt16, i8);
	Value* amt = builder->CreateZExt(amt8, opType); // Приводим к типу значения для сдвига

	// 3. Вычисляем результат
	Value* result = builder->CreateLShr(val, amt);

	auto pushDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;

	Push(result, pushDepth);

	FlagsPromise flagsPromise;
	flagsPromise.op1 = val;
	flagsPromise.op2 = amt;
	flagsPromise.operation = FlagsPromise::SHR;
	flagsPromise.res = nullptr;
	flagsPromise.opType = opType;

	StackMetadata meta;
	meta.flagsPromise = flagsPromise;

	shadowStack.Push(BitDepth_64, nullptr, meta);

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
	llvm::Value* dst = Pop(popDepth).value;
	if (match.bitDepth == BitDepth_8) {
		dst = builder->CreateTrunc(dst, i8);
	}
	llvm::Value* src = Pop(popDepth).value;
	if (match.bitDepth == BitDepth_8) {
		src = builder->CreateTrunc(src, i8);
	}
	llvm::Value* count16 = Pop(BitDepth_16).value;
	llvm::Value* count8 = builder->CreateTrunc(count16, i8);
	llvm::Value* count = builder->CreateZExt(count8, dst->getType());

	llvm::Type* opType = dst->getType();
	auto pushDepth = (match.bitDepth == BitDepth_8) ? BitDepth_16 : match.bitDepth;


	// Интринсик fshl: (Val1 << Amt) | (Val2 >> (Width - Amt))
	// Для SHLD(Dst, Src, Count): Result = (Dst << Count) | (Src >> (Width - Count))
	// Значит аргументы fshl: Op1=Dst, Op2=Src, Amt=Count

	auto fshlFunc = Intrinsic::getDeclaration(llvmModule, Intrinsic::fshl, { opType });
	llvm::Value* result = builder->CreateCall(fshlFunc, { dst, src, count });


	Push(result, pushDepth);

	FlagsPromise flagsPromise;
	flagsPromise.op1 = dst;
	flagsPromise.op2 = src;
	flagsPromise.op3 = count;
	flagsPromise.operation = FlagsPromise::SHLD;
	flagsPromise.res = nullptr;
	flagsPromise.opType = opType;

	StackMetadata meta;
	meta.flagsPromise = flagsPromise;

	shadowStack.Push(BitDepth_64, nullptr, meta);

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
	llvm::Value* dst = Pop(popDepth).value;
	if (match.bitDepth == BitDepth_8) {
		dst = builder->CreateTrunc(dst, i8);
	}
	llvm::Value* src = Pop(popDepth).value;
	if (match.bitDepth == BitDepth_8) {
		src = builder->CreateTrunc(src, i8);
	}
	llvm::Value* count16 = Pop(BitDepth_16).value;
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


	Push(result, pushDepth);

	FlagsPromise flagsPromise;
	flagsPromise.op1 = dst;
	flagsPromise.op2 = src;
	flagsPromise.op3 = count;
	flagsPromise.operation = FlagsPromise::SHRD;
	flagsPromise.res = nullptr;
	flagsPromise.opType = opType;

	StackMetadata meta;
	meta.flagsPromise = flagsPromise;

	shadowStack.Push(BitDepth_64, nullptr, meta);

	if (logDebugMessage) {
		builder->CreateCall(jitLogShrd, {
		   builder->getInt32(match.bitDepth),
		   builder->CreateZExt(src, i64), builder->CreateZExt(dst, i64),
		   builder->CreateTrunc(count, i8),
		   builder->CreateZExt(result, i64), builder->getInt64(0)
			});
	}
}


void LLVMBasicBlockLifter::GenerateStackMemoryAccess(
	llvm::Value* targetAddr,
	llvm::Value* realStackAddr,
	llvm::Type* dataType,
	llvm::Value* valueToWrite, // Если nullptr, то это READ
	bool useHooks
) {
	auto* typedPtr = builder->CreateBitCast(targetAddr, dataType->getPointerTo(), "vsp_base_typed");
	if (valueToWrite) { // WRITE
		auto* typedRealPtr = builder->CreateBitCast(realStackAddr, dataType->getPointerTo(), "real_stack_ptr_typed");
		builder->CreateStore(valueToWrite, typedPtr);
		builder->CreateStore(valueToWrite, typedRealPtr);
	}
	else { // READ
		llvm::Value* loadedStackVal = builder->CreateLoad(dataType, typedPtr, "stack_load");
		auto bitWidth = dataType->getIntegerBitWidth();
		auto pushDepth = (bitWidth == 8) ? BitDepth_16 : static_cast<HandlerBitDepth>(bitWidth);
		Push(loadedStackVal, pushDepth);
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
		Push(loadedRamVal, (HandlerBitDepth)bitWidth);
	}

}
void LLVMBasicBlockLifter::GenerateMemoryAccess(
	llvm::Value* targetAddr,
	llvm::Value* realStackAddr,
	llvm::Type* dataType,
	llvm::Value* valueToWrite, // Если nullptr, то это READ
	bool useHooks,
	bool isProvenStackAddress)
{

	using namespace llvm;

	if (isProvenStackAddress) {
		FlushVsp(true, false);
		GenerateStackMemoryAccess(
			targetAddr,
			realStackAddr,
			dataType,
			valueToWrite,
			useHooks
		);
		return;
	}

	GenerateRamMemoryAccess(targetAddr, dataType, valueToWrite, useHooks);
	return;

}


void LLVMBasicBlockLifter::LiftVmEntry(const VmEntryHandlerData& data) {
	// 1. Пушим заглушки (как в оригинале VMP)
	Push(builder->getInt64(0), BitDepth_64);
	Push(builder->getInt64(0), BitDepth_64);

	// 2. Пушим регистры из NativeContext в виртуальный стек
	// data.popedRegsOrder содержит порядок, в котором VMP ожидает регистры в стеке
	for (auto& reg : data.pushRegsOrder) {
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

		Push(regVal, BitDepth_64);
	}

	// 3. Пушим ImageBaseDifference (аргумент функции)
	//Push(imageBaseDif, BitDepth_64);
	Push(builder->getInt64(0), BitDepth_64);
}
void LLVMBasicBlockLifter::LiftVmJmpIndirect(bool logDebugMessage, const VmJmpData& jmpData) {
	llvm::Value* rawAddr = Pop(BitDepth_64).value;

	auto* targetAddr = rawAddr;

	std::cout << "ShadowStack size before JMP: " << shadowStack.Size() << std::endl;

	// 2. Логирование
	builder->CreateCall(jitLogJmpIndirect, { targetAddr });

	// 3. Пишем адрес в аллоку targetVip, чтобы диспетчер его увидел
	builder->CreateStore(targetAddr, targetVip);


	auto* realStackI8 = builder->CreateBitCast(
		realStackPtr,
		i8->getPointerTo()
	);

	int index = 0;
	int64_t tempOffset = realStackOffset;
	for (auto& slot : shadowStack) {
		if (index++ >= shadowStack.Size() - 19) break;
		tempOffset -= slot.bitDepth;
		auto* realStackGep = builder->CreateConstGEP1_64(
			i8,
			realStackI8,
			tempOffset
		);
		auto* typedStackGep = builder->CreateBitCast(realStackGep, i64->getPointerTo());
		builder->CreateStore(builder->getInt64(0), typedStackGep);
	}

	// 5. Сбрасываем теневой стек в память, так как переходим в другой блок
	FlushVsp(true, false);
}
void LLVMBasicBlockLifter::LiftVmExit(bool logDebugMessage, const VmExitData& data) {
	std::cout << "ShadowStack size before Exit: " << shadowStack.Size() << std::endl;

	// 1. Выгружаем регистры из виртуального стека обратно в NativeContext
	for (auto& reg : data.popedRegsOrder) {
		auto slot = Pop(BitDepth_64);
		auto* value = slot.value;

		int64_t index = GetNativeOffset(reg.id);

		if (index == -1) continue;

		llvm::Value* regPtr = builder->CreateStructGEP(
			nativeContextType,
			nativeContext,
			static_cast<unsigned int>(index),
			"native_reg_" + reg.name
		);

		auto& addrMeta = slot.metadata.addressMeta;
		if (!addrMeta.isInStackAddress) {
			builder->CreateStore(value, regPtr);
			continue;
		}

		auto* realStackAddrI8 = builder->CreateBitCast(
			realStackPtr,
			i8->getPointerTo(),
			"real_stack_i8"
		);
		auto* realStackAddrGep = builder->CreateConstInBoundsGEP1_64(
			i8,
			realStackAddrI8,
			addrMeta.slotAbsoluteBase + addrMeta.relativeOffset,
			"real_stack_typed_gep"
		);
		auto* addrValue = builder->CreatePtrToInt(
			realStackAddrGep,
			i64
		);
		builder->CreateStore(addrValue, regPtr);
	}

	// 2. Выгружаем адрес выхода (VmExitAddr)
	// Обычно VMP хранит его в одном из слотов контекста (например, вместо одного из регистров или в спец. поле)
	{
		auto* value = Pop(BitDepth_64).value;

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
		auto* value = Pop(BitDepth_64).value;

		// retAddrOffset - это индекс в структуре (int)
		llvm::Value* regPtr = builder->CreateStructGEP(
			nativeContextType,
			nativeContext,
			static_cast<unsigned int>(retAddrOffset),
			"vm_ret_addr"
		);
		builder->CreateStore(value, regPtr);
	}

	// 4. Записываем rsp в контекст
	{
		auto* rspValue = builder->CreateConstInBoundsGEP1_64(
			i8,
			realStackPtr,
			realStackOffset,
			"final_rsp"
		);
		llvm::Value* regPtr = builder->CreateStructGEP(
			nativeContextType,
			nativeContext,
			static_cast<unsigned int>(rspOffset),
			"native_rsp"
		);
		builder->CreateStore(rspValue, regPtr);
	}

	// 4. Сигнализируем Диспетчеру (TraceLifter), что нужно остановиться.
	// Запись 0 в targetVip приведет к выходу из switch-цикла диспетчера.
	builder->CreateStore(builder->getInt64(0), targetVip);

}