#include "LLVMFunctionLifter.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Verifier.h>

#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/AliasAnalysis.h>

LLVMFunctionLifter::LLVMFunctionLifter(
	llvm::LLVMContext* ctx,
	llvm::Module* llvmModule,
	llvm::IRBuilder<>* builder,
	bool jitHookMemoryAccess,
	bool jitDebugLogs
) {
	this->ctx = ctx;
	this->llvmModule = llvmModule;
	this->builder = builder;
	i64 = llvm::Type::getInt64Ty(*ctx);
	i32 = llvm::Type::getInt32Ty(*ctx);
	i16 = llvm::Type::getInt16Ty(*ctx);
	i8 = llvm::Type::getInt8Ty(*ctx);
	ptrTy = llvm::Type::getInt64PtrTy(*ctx);
	voidTy = llvm::Type::getVoidTy(*ctx);
	this->jitHookMemoryAccess = jitHookMemoryAccess;
	this->jitDebugLogs = jitDebugLogs;
	
	if (jitHookMemoryAccess) InitJitHooks();
	if (jitDebugLogs) InitLogFunctions();
}

void LLVMFunctionLifter::CreateFunction(const std::string& name) {

	std::vector<llvm::Type*> args{ ptrTy, ptrTy->getPointerTo(), i64};
	auto* funcType = llvm::FunctionType::get(i64, args, false);
	function = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, name, llvmModule);


	auto argsIt = function->arg_begin();
	nativeContextPtr = argsIt++;
	nativeContextPtr->setName("native_context");

	vspPtrAddr = argsIt++;
	vspPtrAddr->setName("vsp_ptr");

	imageBaseDif = argsIt++;
	imageBaseDif->setName("image_base_dif");
}

void LLVMFunctionLifter::InitJitHooks() {
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
void LLVMFunctionLifter::InitLogFunctions() {
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
		{ i32, i64, i64, i64, i64}, //bit depth, arg1, arg2, result, flags
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

llvm::Value* LLVMFunctionLifter::Pop(HandlerBitDepth bitDepth) {

	return ShadowPop(bitDepth);
}
llvm::Value* LLVMFunctionLifter::LegacyPop(HandlerBitDepth bitDepth) {
	llvm::Value* currentVsp = builder->CreateLoad(ptrTy, vspLocalVar, "vsp_val");

	llvm::Type* opType = GetTypeByDepth(bitDepth);

	// 2. !!! FIX: Кастим перед чтением !!!
	llvm::Value* typedPtr = builder->CreateBitCast(currentVsp, opType->getPointerTo(), "vsp_casted");

	// 3. Читаем (Load i16 from i16*)
	llvm::Value* val = builder->CreateLoad(opType, typedPtr, "pop_val");

	// 4. Двигаем стек (POP увеличивает адрес)
	llvm::Value* newTypedPtr = builder->CreateConstInBoundsGEP1_64(opType, typedPtr, 1, "vsp_inc");

	// 5. Кастим обратно и сохраняем
	llvm::Value* newVsp = builder->CreateBitCast(newTypedPtr, ptrTy, "vsp_back_to_i64");
	builder->CreateStore(newVsp, vspLocalVar);

	return val;
}
llvm::Value* LLVMFunctionLifter::ShadowPop(HandlerBitDepth bitDepth) {
	using namespace llvm;

	if (shadowStack.empty()) {
		return LegacyPop(bitDepth);
	}

	auto& slot = shadowStack.back();
	

	if (slot.bitDepth == bitDepth) {
		shadowStack.pop_back();
		return slot.value;
	}

	if (slot.bitDepth < bitDepth) {
		auto currentBits = static_cast<HandlerBitDepth>(slot.bitDepth);
		llvm::Value* lowPart = ShadowPop(currentBits);
		HandlerBitDepth neededBits = (HandlerBitDepth)(bitDepth - currentBits);
		llvm::Value* highPart = ShadowPop(neededBits);

		llvm::Type* targetType = GetTypeByDepth(bitDepth);
		llvm::Value* lowExt = builder->CreateZExt(lowPart, targetType, "merge_low_ext");
		llvm::Value* highExt = builder->CreateZExt(highPart, targetType, "merge_high_ext");

		llvm::Value* shiftAmt = builder->getIntN(targetType->getIntegerBitWidth(), currentBits);
		llvm::Value* highShifted = builder->CreateShl(highExt, shiftAmt, "merge_high_shifted");

		llvm::Value* result = builder->CreateOr(lowExt, highShifted, "merged_res");

		return result;
	}

	auto* fullVal = slot.value;
	unsigned fullWidth = fullVal->getType()->getIntegerBitWidth();

	auto* result = builder->CreateTrunc(
		fullVal,
		GetTypeByDepth(bitDepth),
		"pop_slice"
	);
	auto* shiftAmt = builder->getIntN(fullWidth, bitDepth);
	auto* remainingVal = builder->CreateLShr(fullVal, shiftAmt, "stack_rem_shifted");

	slot.bitDepth -= bitDepth;
	slot.value = remainingVal;
	if (slot.bitDepth == 0) {
		shadowStack.pop_back();
	}

	return result;
}
void LLVMFunctionLifter::Push(llvm::Value* value, HandlerBitDepth bitDepth) {
	ShadowPush(value, bitDepth);
}
void LLVMFunctionLifter::LegacyPush(llvm::Value* value, HandlerBitDepth bitDepth) {
	llvm::Value* currentVsp = builder->CreateLoad(ptrTy, vspLocalVar, "vsp_val");
	llvm::Type* opType = GetTypeByDepth(bitDepth);

	// 3. !!! FIX: Приводим указатель к нужному типу (i64* -> i16*) !!!
	// Теперь typedPtr — это указатель на i16.
	llvm::Value* typedPtr = builder->CreateBitCast(currentVsp, opType->getPointerTo(), "vsp_casted");

	// 4. Выполняем GEP (арифметику) над типизированным указателем
	// Теперь это легально: база i16*, шаг i16.
	llvm::Value* newTypedPtr = builder->CreateConstInBoundsGEP1_64(opType, typedPtr, -1, "vsp_dec");

	// 5. Записываем значение (Store i16 to i16*)
	builder->CreateStore(value, newTypedPtr);

	// 6. Приводим указатель обратно к i64*, чтобы сохранить в переменную стека
	llvm::Value* newVsp = builder->CreateBitCast(newTypedPtr, ptrTy, "vsp_back_to_i64");
	builder->CreateStore(newVsp, vspLocalVar);
}
void LLVMFunctionLifter::ShadowPush(llvm::Value* value, HandlerBitDepth bitDepth) {
	ShadowStackSlot slot;
	slot.bitDepth = bitDepth;
	slot.value = value;
	shadowStack.push_back(slot);
}
void LLVMFunctionLifter::FlushVspToMemory() {
	for (auto& val : shadowStack) {
		HandlerBitDepth bitDepth = val.bitDepth % 8 == 0 ? 
			static_cast<HandlerBitDepth>(val.bitDepth) : 
			BitDepth_64;

		LegacyPush(val.value, bitDepth);
	}
	shadowStack.clear();

	llvm::Value* finalVsp = builder->CreateLoad(ptrTy, vspLocalVar, "final_vsp");

	builder->CreateStore(finalVsp, vspPtrAddr);
}

void LLVMFunctionLifter::LiftVmEntry(const VmEntryHandlerData& data) {
	Push(builder->getInt64(0), BitDepth_64);
	Push(builder->getInt64(0), BitDepth_64);
	for (auto& reg : data.pushRegsOrder) {
		int64_t index = GetNativeOffset(reg.id);

		if (index == -1) continue;

		llvm::Value* regPtr = builder->CreateConstInBoundsGEP1_64(
			i64,                // Тип элемента массива
			nativeContextPtr,   // Базовый указатель (i64*)
			index,              // Индекс (0 для RAX, 1 для RBX...)
			"native_reg_" + reg.name       // Имя переменной для отладки IR
		);

		llvm::Value* regVal = builder->CreateLoad(i64, regPtr, "reg_val");

		Push(regVal, BitDepth_64);
	}
	Push(imageBaseDif, BitDepth_64);
}

void LLVMFunctionLifter::LiftVmPop(const HandlerMatch& match) {
	auto& data = std::get<VmContextAccessData>(match.matchData);
	
	llvm::Value* valueToStore = nullptr;
	llvm::Type* storeType = nullptr;

	if (match.bitDepth == BitDepth_8) {
		// Правило 4: Берем 2 байта со стека, обрезаем до 1 байта
		llvm::Value* val16 = Pop(BitDepth_16);
		valueToStore = builder->CreateTrunc(val16, i8, "pop8_trunc");
		storeType = i8;
	}
	else {
		// Стандартное поведение
		valueToStore = Pop(match.bitDepth);
		storeType = valueToStore->getType();
	}

	// --- LOGGING ---
	if (jitDebugLogs) {
		llvm::Value* val64 = (valueToStore->getType()->isIntegerTy(64))
			? valueToStore
			: builder->CreateZExt(valueToStore, builder->getInt64Ty(), "log_val_ext");

		builder->CreateCall(jitLogPop, {
			builder->getInt32(match.bitDepth),
			builder->getInt64(data.offset),
			val64
			});
	}
	// ----------------

	auto* ctxBaseI8 = builder->CreateBitCast(
		virtualContextPtr,
		llvm::Type::getInt8PtrTy(*ctx)
	);

	auto* destPtrI8 = builder->CreateConstInBoundsGEP1_32(
		i8,
		ctxBaseI8,
		data.offset,
		"pop_reg_offset_" + std::to_string(data.offset)
	);

	llvm::Value* destPtrTyped = builder->CreateBitCast(
		destPtrI8,
		storeType->getPointerTo(),
		"pop_reg_ptr"
	);

	builder->CreateStore(valueToStore, destPtrTyped);
}
void LLVMFunctionLifter::LiftVmPushReg(const HandlerMatch& match) {
	auto& data = std::get<VmContextAccessData>(match.matchData);
	
	llvm::Type* loadType = (match.bitDepth == BitDepth_8) ? builder->getInt8Ty() : GetTypeByDepth(match.bitDepth);

	llvm::Value* ctxBaseI8 = builder->CreateBitCast(
		virtualContextPtr,
		llvm::Type::getInt8PtrTy(*ctx)
	);

	llvm::Value* regPtrI8 = builder->CreateConstInBoundsGEP1_32(
		llvm::Type::getInt8Ty(*ctx), // Шагаем по байтам
		ctxBaseI8,                  // База
		data.offset,           // Смещение (например, 0x00 для RAX)
		"push_reg_offset_" + std::to_string(data.offset)
	);

	llvm::Value* typedRegPtr = builder->CreateBitCast(
		regPtrI8,
		loadType->getPointerTo(),
		"push_reg_ptr_typed"
	);

	llvm::Value* val = builder->CreateLoad(
		loadType,
		typedRegPtr,
		"reg_val_loaded"
	);

	// --- LOGGING ---
	if (jitDebugLogs) {
		llvm::Value* val64 = (val->getType()->isIntegerTy(64))
			? val
			: builder->CreateZExt(val, builder->getInt64Ty(), "log_val_ext");

		builder->CreateCall(jitLogPushReg, {
			builder->getInt32(match.bitDepth),
			builder->getInt64(data.offset),
			val64
			});
	}
	// ----------------

	if (match.bitDepth == BitDepth_8) {
		// Правило 1: Читаем 1 байт, расширяем (ZExt) до 2 байт и пушим
		llvm::Value* val16 = builder->CreateZExt(val, i16, "reg8_to_16");
		Push(val16, BitDepth_16);
	}
	else {
		Push(val, match.bitDepth);
	}
}
void LLVMFunctionLifter::LiftVmPushConst(const HandlerMatch& match) {
	auto& data = std::get<VmPushConstData>(match.matchData);

	// --- LOGGING ---
	if (jitDebugLogs) {
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

void LLVMFunctionLifter::LiftVmPushVsp(const HandlerMatch& match) {
	FlushVspToMemory();

	// Читаем текущий адрес стека (значение переменной vsp_ptr)
	llvm::Value* currentVsp = builder->CreateLoad(ptrTy, vspLocalVar, "vsp_val");

	// Превращаем указатель в число (ptr -> i64), чтобы положить в стек
	llvm::Value* vspAsInt = builder->CreatePtrToInt(currentVsp, GetTypeByDepth(match.bitDepth), "vsp_int");

	// --- LOGGING ---
	if (jitDebugLogs) {
		llvm::Value* val64 = (vspAsInt->getType()->isIntegerTy(64))
			? vspAsInt
			: builder->CreateZExt(vspAsInt, builder->getInt64Ty());

		builder->CreateCall(jitLogPushVsp, {
			builder->getInt32(match.bitDepth),
			val64
			});
	}
	// ----------------

	// Пушим это число в стек
	Push(vspAsInt, match.bitDepth);
}
void LLVMFunctionLifter::LiftVmPopVsp(const HandlerMatch& match) {
	// Снимаем значение со стека (это новый адрес стека)
	llvm::Value* newVspInt = Pop(match.bitDepth);

	// --- LOGGING ---
	if (jitDebugLogs) {
		llvm::Value* val64 = (newVspInt->getType()->isIntegerTy(64))
			? newVspInt
			: builder->CreateZExt(newVspInt, builder->getInt64Ty());

		builder->CreateCall(jitLogPopVsp, {
			builder->getInt32(match.bitDepth),
			val64
			});
	}
	// ----------------

	// Превращаем число обратно в указатель (i64 -> i64*)
	llvm::Value* newVspPtr = builder->CreateIntToPtr(newVspInt, ptrTy, "new_vsp_ptr");

	// Обновляем переменную vspPtrAddr
	builder->CreateStore(newVspPtr, vspLocalVar);
}

void LLVMFunctionLifter::LiftVmReadMem(const HandlerMatch& match) {
	// 1. Снимаем адрес памяти со стека (всегда 64 бита)
	llvm::Value* addrInt = Pop(BitDepth_64);
	llvm::Value* valLog = nullptr; // Для лога

	if (match.bitDepth == BitDepth_8) {
		// --- Логика 8 бит ---
		// Читаем 1 байт, но в стек кладем 2 байта
		llvm::Value* ptr = builder->CreateIntToPtr(addrInt, builder->getInt8PtrTy(), "mem_read_ptr_8");
		llvm::Value* val8 = builder->CreateLoad(builder->getInt8Ty(), ptr, "mem_val_8");
		valLog = val8;

		// Расширяем до 16 бит для стека
		llvm::Value* val16 = builder->CreateZExt(val8, builder->getInt16Ty(), "val_ext_16");
		Push(val16, BitDepth_16);
	}
	else {
		// --- Логика 16/32/64 бит ---
		llvm::Type* readType = GetTypeByDepth(match.bitDepth);
		llvm::Value* ptr = builder->CreateIntToPtr(addrInt, readType->getPointerTo(), "mem_read_ptr");
		llvm::Value* val = builder->CreateLoad(readType, ptr, "mem_val");

		valLog = val;

		Push(val, match.bitDepth);
	}
	// --- LOGGING ---
	if (jitDebugLogs && valLog) {
		llvm::Value* val64 = (valLog->getType()->isIntegerTy(64))
			? valLog
			: builder->CreateZExt(valLog, builder->getInt64Ty());

		builder->CreateCall(jitLogReadMem, {
			builder->getInt32(match.bitDepth),
			addrInt, // Адрес всегда i64
			val64
			});
	}
	// ----------------
}
void LLVMFunctionLifter::LiftVmReadMemHooked(const HandlerMatch& match) {
	llvm::Value* addrInt = Pop(BitDepth_64);

	// Вычисляем размер
	int byteSize = (match.bitDepth == BitDepth_8) ? 1 : (match.bitDepth / 8);
	llvm::Value* sizeVal = builder->getInt32(byteSize);

	// Вызов хука
	llvm::Value* val64 = builder->CreateCall(jitReadFunc, { addrInt, sizeVal }, "mem_read_call");
	llvm::Value* valLog = nullptr;

	if (match.bitDepth == BitDepth_8) {
		// JIT вернул i64, нам нужен младший байт, который мы положим в стек как i16
		llvm::Value* val8 = builder->CreateTrunc(val64, builder->getInt8Ty(), "trunc_to_8");
		valLog = val8;
		llvm::Value* val16 = builder->CreateZExt(val8, builder->getInt16Ty(), "ext_to_16");
		Push(val16, BitDepth_16);
	}
	else {
		llvm::Type* targetType = GetTypeByDepth(match.bitDepth);
		llvm::Value* finalVal = val64;
		if (targetType != builder->getInt64Ty()) {
			finalVal = builder->CreateTrunc(val64, targetType, "mem_val_trunc");
		}
		valLog = finalVal;
		Push(finalVal, match.bitDepth);
	}

	// --- LOGGING ---
	if (jitDebugLogs && valLog) {
		// LogReadMem ожидает (bitDepth, addr, readedValue)
		// Мы можем передать val64 напрямую, но корректнее передать то, что реально "прочиталось" с учетом размера
		llvm::Value* finalLogVal = (valLog->getType()->isIntegerTy(64)) ? valLog : builder->CreateZExt(valLog, builder->getInt64Ty());

		builder->CreateCall(jitLogReadMem, {
			builder->getInt32(match.bitDepth),
			addrInt,
			finalLogVal
			});
	}
	// ----------------
}

void LLVMFunctionLifter::LiftVmWriteMem(const HandlerMatch& match) {
	// Порядок POP: Сначала Адрес, потом Значение (согласно вашему описанию)
	llvm::Value* addrInt = Pop(BitDepth_64);
	llvm::Value* valLog = nullptr;

	if (match.bitDepth == BitDepth_8) {
		// --- Логика 8 бит ---
		// Снимаем 2 байта (так как в стеке 8 бит хранятся как 16)
		llvm::Value* val16 = Pop(BitDepth_16);

		// Обрезаем до 1 байта для записи в память
		llvm::Value* val8 = builder->CreateTrunc(val16, builder->getInt8Ty(), "val_trunc_8");
		valLog = val8;

		llvm::Value* ptr = builder->CreateIntToPtr(addrInt, builder->getInt8PtrTy(), "mem_write_ptr_8");
		builder->CreateStore(val8, ptr);
	}
	else {
		// --- Логика 16/32/64 бит ---
		llvm::Value* valInt = Pop(match.bitDepth);
		llvm::Type* storeType = GetTypeByDepth(match.bitDepth);

		// Защита от несовпадения типов (например, если Pop вернул i64, а пишем i32)
		if (valInt->getType() != storeType) {
			valInt = builder->CreateTrunc(valInt, storeType, "val_trunc");
		}
		valLog = valInt;

		llvm::Value* ptr = builder->CreateIntToPtr(addrInt, storeType->getPointerTo(), "mem_write_ptr");
		builder->CreateStore(valInt, ptr);
	}

	// --- LOGGING ---
	if (jitDebugLogs && valLog) {
		llvm::Value* val64 = (valLog->getType()->isIntegerTy(64))
			? valLog
			: builder->CreateZExt(valLog, builder->getInt64Ty());

		builder->CreateCall(jitLogWriteMem, {
			builder->getInt32(match.bitDepth),
			addrInt,
			val64
			});
	}
	// ----------------
}
void LLVMFunctionLifter::LiftVmWriteMemHooked(const HandlerMatch& match) {
	llvm::Value* addrInt = Pop(BitDepth_64);

	llvm::Value* valToPass = nullptr; // Значение, которое передадим в хук (всегда i64)
	llvm::Value* valLog = nullptr; // Для лога
	int byteSize = 0;

	if (match.bitDepth == BitDepth_8) {
		byteSize = 1;
		llvm::Value* val16 = Pop(BitDepth_16); // Снимаем выровненное значение
		// Нам нужно передать i64, содержащее наш байт
		// Trunc(8) -> ZExt(64) гарантирует очистку мусора в старшем байте val16
		llvm::Value* val8 = builder->CreateTrunc(val16, builder->getInt8Ty());
		valLog = val8;
		valToPass = builder->CreateZExt(val8, builder->getInt64Ty(), "val_ext_64");
	}
	else {
		byteSize = match.bitDepth / 8;
		llvm::Value* valInt = Pop(match.bitDepth);
		valLog = valInt;
		valToPass = valInt;
		if (valInt->getType() != builder->getInt64Ty()) {
			valToPass = builder->CreateZExt(valInt, builder->getInt64Ty(), "val_ext_64");
		}
	}

	// --- LOGGING ---
	if (jitDebugLogs && valLog) {
		llvm::Value* val64 = (valLog->getType()->isIntegerTy(64)) ? valLog : builder->CreateZExt(valLog, builder->getInt64Ty());
		builder->CreateCall(jitLogWriteMem, {
			builder->getInt32(match.bitDepth),
			addrInt,
			val64
			});
	}
	// ----------------

	llvm::Value* sizeVal = builder->getInt32(byteSize);
	builder->CreateCall(jitWriteFunc, { addrInt, valToPass, sizeVal });
}

void LLVMFunctionLifter::LiftVmAdd(const HandlerMatch& match) {
	auto& aluData = std::get<VmAluData>(match.matchData);
	llvm::Value* op1, *op2, *res;

	if (match.bitDepth == BitDepth_8) {
		// Правило 3: Работаем с 2 байтами на стеке, вычисляем в 1 байте
		llvm::Value* op2_16 = Pop(BitDepth_16);
		llvm::Value* op1_16 = Pop(BitDepth_16);

		llvm::Value* op2_8 = builder->CreateTrunc(op2_16, builder->getInt8Ty(), "op2_trunc");
		llvm::Value* op1_8 = builder->CreateTrunc(op1_16, builder->getInt8Ty(), "op1_trunc");

		llvm::Value* res8 = builder->CreateAdd(op1_8, op2_8, "add8_res");
		op1 = op1_8; op2 = op2_8; res = res8;

		// Расширяем обратно до 16 для записи в стек
		llvm::Value* res16 = builder->CreateZExt(res8, i16, "res8_ext");
		Push(res16, BitDepth_16);
	}
	else {
		op2 = Pop(match.bitDepth);
		op1 = Pop(match.bitDepth);
		res = builder->CreateAdd(op1, op2, "add_res");
		Push(res, match.bitDepth);
	}

	// --- LOGGING ---
	if (jitDebugLogs) {
		// LogAdd(bitDepth, arg1, arg2, result, flags)
		builder->CreateCall(jitLogAdd, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(op1, i64),
			builder->CreateZExt(op2, i64),
			builder->CreateZExt(res, i64),
			builder->getInt64(aluData.flags)
			});
	}
	// ----------------

	Push(builder->getInt64(aluData.flags), BitDepth_64);
}
void LLVMFunctionLifter::LiftVmNor(const HandlerMatch& match) {
	auto& aluData = std::get<VmAluData>(match.matchData);
	llvm::Value* op1, * op2, * res;

	if (match.bitDepth == BitDepth_8) {
		llvm::Value* op2_16 = Pop(BitDepth_16);
		llvm::Value* op1_16 = Pop(BitDepth_16);

		llvm::Value* op2_8 = builder->CreateTrunc(op2_16, builder->getInt8Ty());
		llvm::Value* op1_8 = builder->CreateTrunc(op1_16, builder->getInt8Ty());

		llvm::Value* orVal = builder->CreateOr(op1_8, op2_8);
		llvm::Value* res8 = builder->CreateNot(orVal, "nor8_res");
		op1 = op1_8; op2 = op2_8; res = res8;

		llvm::Value* res16 = builder->CreateZExt(res8, builder->getInt16Ty());
		Push(res16, BitDepth_16);
	}
	else {
		op2 = Pop(match.bitDepth);
		op1 = Pop(match.bitDepth);
		llvm::Value* orVal = builder->CreateOr(op1, op2, "or_res");
		res = builder->CreateNot(orVal, "nor_res");
		Push(res, match.bitDepth);
	}

	// --- LOGGING ---
	if (jitDebugLogs) {
		builder->CreateCall(jitLogNor, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(op1, i64),
			builder->CreateZExt(op2, i64),
			builder->CreateZExt(res, i64),
			builder->getInt64(aluData.flags)
			});
	}
	// ----------------

	Push(builder->getInt64(aluData.flags), BitDepth_64);
}
void LLVMFunctionLifter::LiftVmNand(const HandlerMatch& match) {
	auto& aluData = std::get<VmAluData>(match.matchData);
	llvm::Value* op1, * op2, * res;

	if (match.bitDepth == BitDepth_8) {
		llvm::Value* op2_16 = Pop(BitDepth_16);
		llvm::Value* op1_16 = Pop(BitDepth_16);

		llvm::Value* op2_8 = builder->CreateTrunc(op2_16, builder->getInt8Ty());
		llvm::Value* op1_8 = builder->CreateTrunc(op1_16, builder->getInt8Ty());

		llvm::Value* andVal = builder->CreateAnd(op1_8, op2_8);
		llvm::Value* res8 = builder->CreateNot(andVal, "nand8_res");
		op1 = op1_8; op2 = op2_8; res = res8;

		llvm::Value* res16 = builder->CreateZExt(res8, builder->getInt16Ty());
		Push(res16, BitDepth_16);
	}
	else {
		op2 = Pop(match.bitDepth);
		op1 = Pop(match.bitDepth);
		llvm::Value* andVal = builder->CreateAnd(op1, op2, "and_res");
		res = builder->CreateNot(andVal, "nand_res");
		Push(res, match.bitDepth);
	}

	// --- LOGGING ---
	if (jitDebugLogs) {
		builder->CreateCall(jitLogNand, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(op1, builder->getInt64Ty()),
			builder->CreateZExt(op2, builder->getInt64Ty()),
			builder->CreateZExt(res, builder->getInt64Ty()),
			builder->getInt64(aluData.flags)
			});
	}
	// ----------------

	Push(builder->getInt64(aluData.flags), BitDepth_64);
}

void LLVMFunctionLifter::LiftVmShl(const HandlerMatch& match) {
	// Порядок: Val, Amt
	auto& aluData = std::get<VmAluData>(match.matchData);
	llvm::Value* amt = nullptr, * val = nullptr, * res = nullptr;
	if (match.bitDepth == BitDepth_8) {
		llvm::Value* val16 = Pop(BitDepth_16);
		
		llvm::Value* val8 = builder->CreateTrunc(val16, i8);
		amt = builder->CreateTrunc(Pop(BitDepth_16), i8);

		llvm::Value* res8 = builder->CreateShl(val8, amt, "shl8_res");

		val = val8; res = res8;
		llvm::Value* res16 = builder->CreateZExt(res8, i16, "res8_ext");

		Push(res16, BitDepth_16);
	}
	else {
		val = Pop(match.bitDepth);
		amt = builder->CreateZExt(Pop(BitDepth_16), GetTypeByDepth(match.bitDepth));
		res = builder->CreateShl(val, amt, "shl_res");
		Push(res, match.bitDepth);
	}

	// --- LOGGING ---
	if (jitDebugLogs) {
		builder->CreateCall(jitLogShl, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(val, builder->getInt64Ty()),
			builder->CreateZExt(amt, builder->getInt64Ty()),
			builder->CreateZExt(res, builder->getInt64Ty()),
			builder->getInt64(aluData.flags)
			});
	}
	// ----------------

	Push(builder->getInt64(aluData.flags), BitDepth_64);
}
void LLVMFunctionLifter::LiftVmShr(const HandlerMatch& match) {
	auto& aluData = std::get<VmAluData>(match.matchData);
	llvm::Value* amt = nullptr, * val = nullptr, * res = nullptr;

	if (match.bitDepth == BitDepth_8) {
		llvm::Value* val16 = Pop(BitDepth_16);

		llvm::Value* val8 = builder->CreateTrunc(val16, i8);
		amt = builder->CreateTrunc(Pop(BitDepth_16), i8);

		// Logical Shift Right (без знака)
		llvm::Value* res8 = builder->CreateLShr(val8, amt, "shr8_res");
		val = val8; res = res8;
		llvm::Value* res16 = builder->CreateZExt(res8, i16, "res8_ext");

		Push(res16, BitDepth_16);
	}
	else {
		val = Pop(match.bitDepth);
		amt = builder->CreateZExt(Pop(BitDepth_16), GetTypeByDepth(match.bitDepth));
		res = builder->CreateLShr(val, amt, "shr_res");
		Push(res, match.bitDepth);
	}

	// --- LOGGING ---
	if (jitDebugLogs) {
		builder->CreateCall(jitLogShr, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(val, builder->getInt64Ty()),
			builder->CreateZExt(amt, builder->getInt64Ty()),
			builder->CreateZExt(res, builder->getInt64Ty()),
			builder->getInt64(aluData.flags)
			});
	}
	// ----------------

	Push(builder->getInt64(aluData.flags), BitDepth_64);
}
void LLVMFunctionLifter::LiftVmRol(const HandlerMatch& match) {
	if (match.bitDepth == BitDepth_8) {
		llvm::Value* amt16 = Pop(BitDepth_16);
		llvm::Value* val16 = Pop(BitDepth_16);

		llvm::Value* amt8 = builder->CreateTrunc(amt16, builder->getInt8Ty());
		llvm::Value* val8 = builder->CreateTrunc(val16, builder->getInt8Ty());

		// ROL(x, n) = fshl(x, x, n)
		llvm::Function* fshl = llvm::Intrinsic::getDeclaration(llvmModule, llvm::Intrinsic::fshl, { builder->getInt8Ty() });
		llvm::Value* res8 = builder->CreateCall(fshl, { val8, val8, amt8 }, "rol8_res");

		Push(builder->CreateZExt(res8, builder->getInt16Ty()), BitDepth_16);
	}
	else {
		llvm::Value* amt = Pop(match.bitDepth);
		llvm::Value* val = Pop(match.bitDepth);

		llvm::Type* valType = GetTypeByDepth(match.bitDepth);
		llvm::Function* fshl = llvm::Intrinsic::getDeclaration(llvmModule, llvm::Intrinsic::fshl, { valType });

		llvm::Value* res = builder->CreateCall(fshl, { val, val, amt }, "rol_res");
		Push(res, match.bitDepth);
	}
}
void LLVMFunctionLifter::LiftVmRor(const HandlerMatch& match) {
	if (match.bitDepth == BitDepth_8) {
		llvm::Value* amt16 = Pop(BitDepth_16);
		llvm::Value* val16 = Pop(BitDepth_16);

		llvm::Value* amt8 = builder->CreateTrunc(amt16, builder->getInt8Ty());
		llvm::Value* val8 = builder->CreateTrunc(val16, builder->getInt8Ty());

		// ROR(x, n) = fshr(x, x, n)
		llvm::Function* fshr = llvm::Intrinsic::getDeclaration(llvmModule, llvm::Intrinsic::fshr, { builder->getInt8Ty() });
		llvm::Value* res8 = builder->CreateCall(fshr, { val8, val8, amt8 }, "ror8_res");

		Push(builder->CreateZExt(res8, builder->getInt16Ty()), BitDepth_16);
	}
	else {
		llvm::Value* amt = Pop(match.bitDepth);
		llvm::Value* val = Pop(match.bitDepth);

		llvm::Type* valType = GetTypeByDepth(match.bitDepth);
		llvm::Function* fshr = llvm::Intrinsic::getDeclaration(llvmModule, llvm::Intrinsic::fshr, { valType });

		llvm::Value* res = builder->CreateCall(fshr, { val, val, amt }, "ror_res");
		Push(res, match.bitDepth);
	}
}

void LLVMFunctionLifter::LiftVmShld(const HandlerMatch& match) {
	// Правило 5: В стеке лежат Dest, Source, Count.
	// Так как стек LIFO, порядок Pop обратный: Count, Source, Dest.

	// 1. Снимаем Count. Он всегда 1 байт (но на стеке занимает 2 байта, если режим 8 бит или выравнивание).
	// В VMP count обычно лежит как слово (16 бит) или в соответствии с битностью операнда, 
	// но ваше правило гласит "count всегда 1 байт".
	// Предположим безопасный вариант: снимаем BitDepth_16 (минимальный слот стека), обрезаем до i8, затем ZExt до типа операнда.

	auto& shldData = std::get<VmShldData>(match.matchData);

	llvm::Value* opSource = nullptr;
	llvm::Value* opDest = nullptr;
	llvm::Type* valType = nullptr;

	if (match.bitDepth == BitDepth_8) {
		// Режим 8 бит: Source и Dest лежат как 16 бит
		llvm::Value* dest16 = Pop(BitDepth_16);
		llvm::Value* src16 = Pop(BitDepth_16);

		opDest = builder->CreateTrunc(dest16, builder->getInt8Ty(), "dest_trunc_8");
		opSource = builder->CreateTrunc(src16, builder->getInt8Ty(), "src_trunc_8");
		
		valType = builder->getInt8Ty();
	}
	else {
		// Режим 16/32/64
		opDest = Pop(match.bitDepth);
		opSource = Pop(match.bitDepth);
		valType = GetTypeByDepth(match.bitDepth);
	}

	llvm::Value* countRaw = Pop(BitDepth_16); // Всегда снимаем минимум 2 байта для Count
	llvm::Value* count8 = builder->CreateTrunc(countRaw, builder->getInt8Ty(), "count_trunc_8");

	// Готовим Count к типу операнда (fshl требует одинаковых типов)
	llvm::Value* countFinal = builder->CreateZExt(count8, valType, "count_ext");

	// SHLD = Funnel Shift Left
	llvm::Function* fshl = llvm::Intrinsic::getDeclaration(llvmModule, llvm::Intrinsic::fshl, { valType });
	llvm::Value* res = builder->CreateCall(fshl, { opDest, opSource, countFinal }, "shld_res");
	
	// --- LOGGING ---
	if (jitDebugLogs) {
		// jitLogShld: bit depth, source, dst, shift(i8), result, flags
		// Обратите внимание на порядок source/dst в сигнатуре логгера!
		builder->CreateCall(jitLogShld, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(opSource, builder->getInt64Ty()),
			builder->CreateZExt(opDest, builder->getInt64Ty()),
			count8, // i8
			builder->CreateZExt(res, builder->getInt64Ty()),
			builder->getInt64(shldData.flags)
			});
	}
	// ----------------

	if (match.bitDepth == BitDepth_8) {
		// Результат 8 бит -> расширяем до 16 и пушим
		llvm::Value* res16 = builder->CreateZExt(res, builder->getInt16Ty(), "res_ext_16");
		Push(res16, BitDepth_16);
	}
	else {
		Push(res, match.bitDepth);
	}
	Push(builder->getInt64(shldData.flags), BitDepth_64);
}
void LLVMFunctionLifter::LiftVmShrd(const HandlerMatch& match) {
	// Порядок Pop: Count, Source, Dest
	auto& shldData = std::get<VmShldData>(match.matchData);
	

	llvm::Value* opSource = nullptr;
	llvm::Value* opDest = nullptr;
	llvm::Type* valType = nullptr;

	if (match.bitDepth == BitDepth_8) {
		llvm::Value* dest16 = Pop(BitDepth_16);
		llvm::Value* src16 = Pop(BitDepth_16);

		opDest = builder->CreateTrunc(dest16, builder->getInt8Ty());
		opSource = builder->CreateTrunc(src16, builder->getInt8Ty());
		
		valType = builder->getInt8Ty();
	}
	else {
		opDest = Pop(match.bitDepth);
		opSource = Pop(match.bitDepth);
		valType = GetTypeByDepth(match.bitDepth);
	}

	llvm::Value* countRaw = Pop(BitDepth_16); // Count всегда 1 байт, но на стеке >= 2 байт
	llvm::Value* count8 = builder->CreateTrunc(countRaw, builder->getInt8Ty(), "count_trunc_8");

	llvm::Value* countFinal = builder->CreateZExt(count8, valType);

	// SHRD = Funnel Shift Right
	llvm::Function* fshr = llvm::Intrinsic::getDeclaration(llvmModule, llvm::Intrinsic::fshr, { valType });
	llvm::Value* res = builder->CreateCall(fshr, { opDest, opSource, countFinal }, "shrd_res");

	// --- LOGGING ---
	if (jitDebugLogs) {
		// jitLogShld: bit depth, source, dst, shift(i8), result, flags
		// Обратите внимание на порядок source/dst в сигнатуре логгера!
		builder->CreateCall(jitLogShrd, {
			builder->getInt32(match.bitDepth),
			builder->CreateZExt(opSource, builder->getInt64Ty()),
			builder->CreateZExt(opDest, builder->getInt64Ty()),
			count8, // i8
			builder->CreateZExt(res, builder->getInt64Ty()),
			builder->getInt64(shldData.flags)
			});
	}
	// ----------------

	if (match.bitDepth == BitDepth_8) {
		llvm::Value* res16 = builder->CreateZExt(res, builder->getInt16Ty());
		Push(res16, BitDepth_16);
	}
	else {
		Push(res, match.bitDepth);
	}
	Push(builder->getInt64(shldData.flags), BitDepth_64);
}

void LLVMFunctionLifter::LiftVmExit(const VmExitData& data) {
	for (auto& reg : data.popedRegsOrder) {
		auto* value = Pop(BitDepth_64);

		int64_t index = GetNativeOffset(reg.id);

		if (index == -1) continue;

		llvm::Value* regPtr = builder->CreateConstInBoundsGEP1_64(
			i64,                // Тип элемента массива
			nativeContextPtr,   // Базовый указатель (i64*)
			index,              // Индекс (0 для RAX, 1 для RBX...)
			"native_reg_" + reg.name       // Имя переменной для отладки IR
		);

		builder->CreateStore(value, regPtr);
	}
	{
		auto* value = Pop(BitDepth_64);
		llvm::Value* regPtr = builder->CreateConstInBoundsGEP1_64(
			i64,                // Тип элемента массива
			nativeContextPtr,   // Базовый указатель (i64*)
			vmExitAddr,              // Индекс (0 для RAX, 1 для RBX...)
			"vm_exit_addr"       // Имя переменной для отладки IR
		);
		builder->CreateStore(value, regPtr);
	}
	{
		auto* value = Pop(BitDepth_64);
		llvm::Value* regPtr = builder->CreateConstInBoundsGEP1_64(
			i64,                // Тип элемента массива
			nativeContextPtr,   // Базовый указатель (i64*)
			retAddrOffset,              // Индекс (0 для RAX, 1 для RBX...)
			"vm_ret_addr"       // Имя переменной для отладки IR
		);
		builder->CreateStore(value, regPtr);
	}

	FlushVspToMemory();
	builder->CreateRet(builder->getInt64(0));
}
void LLVMFunctionLifter::LiftVmJmpIndirect(const VmJmpData& jmpData) {
	auto* jmpVip = Pop(BitDepth_64);

	// --- LOGGING ---
	if (jitDebugLogs) {
		builder->CreateCall(jitLogJmpIndirect, { jmpVip });
	}
	// ----------------

	FlushVspToMemory();
	builder->CreateRet(jmpVip);
}

void LLVMFunctionLifter::LiftVirtualCode(const std::string& functionName, const std::vector<HandlerMatch> handlers) {
	std::vector<llvm::BasicBlock*> basicBlocks;

	CreateFunction(functionName);

	auto* currentBasicBlock = llvm::BasicBlock::Create(*ctx, "BasicBlock_" + std::to_string(basicBlocks.size()), function);
	builder->SetInsertPoint(currentBasicBlock);
	llvm::Type* vmContextType = llvm::ArrayType::get(i8, 256);
	virtualContextPtr = builder->CreateAlloca(vmContextType, nullptr, "virtual_ctx");

	vspLocalVar = builder->CreateAlloca(ptrTy, nullptr, "vsp_local_cursor");
	llvm::Value* initialVsp = builder->CreateLoad(ptrTy, vspPtrAddr, "init_vsp");
	builder->CreateStore(initialVsp, vspLocalVar);

	bool stopLifting = false;
	for (auto& handler : handlers) {
		auto handlerType = handler.type;
		switch (handlerType)
		{
		case Handler_Unknown: continue;
		case Handler_VmEntry: LiftVmEntry(std::get<VmEntryHandlerData>(handler.matchData)); break;
		case Handler_VmPop: LiftVmPop(handler); break;
		case Handler_VmPushConst: LiftVmPushConst(handler); break;
		case Handler_VmPushReg: LiftVmPushReg(handler); break;
		case Handler_VmPushVsp: LiftVmPushVsp(handler); break;
		case Handler_VmPopVsp: LiftVmPopVsp(handler); break;
		case Handler_VmReadMem: 
			if (jitHookMemoryAccess) LiftVmReadMemHooked(handler);
			else LiftVmReadMem(handler); 
			break;
		case Handler_VmWriteMem: 
			if (jitHookMemoryAccess) LiftVmWriteMemHooked(handler);
			else LiftVmWriteMem(handler); 
			break;
		case Handler_VmAdd: LiftVmAdd(handler); break;
		case Handler_VmNor: LiftVmNor(handler); break;
		case Handler_VmNand: LiftVmNand(handler); break;
		case Handler_VmShr: LiftVmShr(handler); break;
		case Handler_VmShl: LiftVmShl(handler); break;
		case Handler_VmRol: LiftVmRol(handler); break;
		case Handler_VmRor: LiftVmRor(handler); break;
		case Handler_VmShld: LiftVmShld(handler); break;
		case Handler_VmShrd: LiftVmShrd(handler); break;
		case Handler_VmExit:
			LiftVmExit(std::get<VmExitData>(handler.matchData));
			stopLifting = true;
			break;
		case Handler_VmJmpIndirect:
			LiftVmJmpIndirect(std::get<VmJmpData>(handler.matchData));
			stopLifting = true;
			break;
		}

		if (stopLifting) break;
	}

	if (!builder->GetInsertBlock()->getTerminator()) {
		FlushVspToMemory();
		builder->CreateRet(builder->getInt64(2));
	}


}

void OptimizeModule(llvm::Module& llvmModule, bool enableOptimization){
	using namespace llvm;
	if (llvm::verifyModule(llvmModule, &llvm::errs())) {
		llvm::errs() << "!!! CRITICAL ERROR: Module verification failed AFTER INSTRUMENTATION !!!\n";
		llvmModule.print(llvm::errs(), nullptr);
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

	ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(enableOptimization ?
		OptimizationLevel::O3 :
		OptimizationLevel::O0);

	MPM.run(llvmModule, MAM);
}

